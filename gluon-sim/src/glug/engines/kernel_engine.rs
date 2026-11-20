use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::Cycles;
use crate::common::base::DMAReq;
use crate::common::base::Event;
use crate::common::base::MemReq;
use crate::common::base::SimErr;
use crate::glug::engine::Engine;
use crate::glug::engine::EngineCommand;
use crate::glug::engines::cs_engine::KernelEntry;
use cyclotron::base::mem::HasMemory;
use cyclotron::muon::cache::{self, Cache};
use cyclotron::muon::sm::SM;
use cyclotron::sim::flat_mem::FlatMemory;
use cyclotron::info;
use cyclotron::muon::core::{self, CtaId};
use cyclotron::muon::warp::ExecErr;
use cyclotron::sim::log::Logger;
use log::error;
use serde::Deserialize;
use std::collections::VecDeque;
use std::ops::Range;
use std::sync::Arc;

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct KernelEngineConfig {
    pub launch_slots: usize, // launches that can be placing CTAs at once
}

impl Default for KernelEngineConfig {
    fn default() -> Self {
        KernelEngineConfig { launch_slots: 8 }
    }
}

/// A kernel command: the launch entry, and the stream and command it came on.
#[derive(Debug, Default, Clone, Copy)]
pub struct KernelCommand {
    pub sid: u8,
    pub cmd_id: u64,
    pub params: u32, // address of the params block
    pub grid: (u32, u32, u32),
    pub block: (u32, u32, u32),
    pub kernel: u8, // kernel table entry
    pub smem: u32,  // bytes per block
}

impl KernelCommand {
    // the entry's four words are laid out in sw/driver/include/core.h; a host command has the kernel
    // id in them, a device launch in its instruction
    fn from_entry(sid: u8, cmd_id: u64, w: [u32; 4], kernel: u8) -> Self {
        KernelCommand {
            sid,
            cmd_id,
            params: w[0],
            grid: (w[1] & 0xffff, w[1] >> 16, w[2] & 0xffff),
            block: ((w[3] & 0x3ff) + 1, (w[3] >> 10 & 0x3ff) + 1, (w[2] >> 24 & 0x3f) + 1),
            kernel,
            smem: (w[3] >> 20) * 16,
        }
    }
}

// the L2: the cache backed by DRAM, which the command processor sits on
fn l2(caches: &[Cache]) -> usize {
    caches.iter().position(|cache| cache.depth == 0).unwrap()
}

// a launched CTA and the SM resources it holds
struct Cta {
    sm: usize,
    id: CtaId,
    smem: Range<usize>,
    sid: u8,
}

// a launch whose CTAs are still being placed
struct Launch {
    cmd: KernelCommand,
    tb_ctr: u32,
    total_tb: u32,
    blocked_at: Option<usize>, // SM frees seen when placement last failed
    ready: u64, // cycle it is in the slot: later than it was taken if it was read from the launch queue
}

// a stream's kernel command, which is done when nothing of it is outstanding
#[derive(Default)]
struct Stream {
    outstanding: u32, // its launches still placing, plus its CTAs running
    live: u32,        // its CTAs running
    err: Option<ExecErr>,
    cmd_id: u64, // of its host command; the launches under it carry it too
    kernel_pc: u32,
    cycles: Cycles, // since the command arrived
}

pub struct KernelEngine {
    slots: Vec<Option<Launch>>,
    // the launch queue: a ring of entries in device memory for launches that find every slot busy
    ring_base: u32,
    ring_slots: u32,
    head: u32, // entries read
    tail: u32, // entries written
    line: (u64, u64), // the cache line of the ring last read, and the cycle its data arrived
    streams: Vec<Stream>, // by stream id
    inflight: Vec<Cta>,
    next_cta: CtaId,
    retired_at: u64, // SM events seen when CTAs were last checked

    completions: VecDeque<Event>,
    done: Option<Event>, // the completion out this tick

    logger: Arc<Logger>,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn new(config: &KernelEngineConfig) -> Self {
        KernelEngine {
            slots: (0..config.launch_slots).map(|_| None).collect(),
            ring_base: 0,
            ring_slots: 0,
            head: 0,
            tail: 0,
            line: (u64::MAX, 0),
            streams: vec![],
            inflight: vec![],
            next_cta: 0,
            retired_at: 0,
            completions: VecDeque::new(),
            done: None,
            logger: Arc::new(Logger::new(0)),
        }
    }
}

impl KernelEngine {
    fn stream(&mut self, sid: u8) -> &mut Stream {
        if self.streams.len() <= sid as usize {
            self.streams.resize_with(sid as usize + 1, Stream::default);
        }
        &mut self.streams[sid as usize]
    }

    // one thing fewer is outstanding on stream `sid`; with the last, its command completes
    fn finish(&mut self, sid: u8) {
        let s = self.stream(sid);
        s.outstanding -= 1;
        if s.outstanding == 0 {
            println!("kernel pc 0x{:08x}: {}", s.kernel_pc, std::mem::take(&mut s.cycles));
            let result = s.err.take().map_or(Ok(()), Err);
            self.completions.push_back(Event::from_kernel_err(sid, result));
        }
    }

    // place `launch`'s CTAs from a free slot, from cycle `ready` on
    fn fill(&mut self, launch: core::Launch, ready: u64) {
        let cmd_id = self.stream(launch.stream).cmd_id;
        let cmd = KernelCommand::from_entry(launch.stream, cmd_id, launch.words, launch.kernel);
        info!(self.logger, "Kernel engine: {:?}", cmd);
        let (gx, gy, gz) = cmd.grid;
        let launch = Launch { cmd, tb_ctr: 0, total_tb: gx * gy * gz, blocked_at: None, ready };
        *self.slots.iter_mut().find(|slot| slot.is_none()).expect("no free launch slot") = Some(launch);
    }

    fn free_slot(&self) -> bool {
        self.slots.iter().any(Option::is_none)
    }

    // the address of ring entry `i`, and the cache line it is in
    fn ring_entry(&self, i: u32, sms: &[SM]) -> (usize, u64) {
        let addr = self.ring_base as usize + (i % self.ring_slots) as usize * 16;
        (addr, (addr / sms[0].config.gmem.line) as u64)
    }

    // take a launch a core issued: straight into a free slot if none is queued ahead of it, usable
    // once it has reached the command processor, else onto the ring.  The stored entry keeps the
    // stream in the low bits of the params address and the kernel id in the bits a thread leaves
    // free; it does not keep the cycle the launch arrives, so one read back sooner starts early
    fn arrive(&mut self, launch: core::Launch, now: u64, sms: &[SM], dram: &mut FlatMemory, caches: &mut [Cache]) {
        if self.idle() {
            return self.fill(launch, launch.ready);
        }
        let mut w = launch.words;
        w[0] = w[0] & !3 | launch.stream as u32;
        w[2] = w[2] & !(0xff << 16) | (launch.kernel as u32) << 16;
        let (addr, line) = self.ring_entry(self.tail, sms);
        let bytes: Vec<u8> = w.iter().flat_map(|word| word.to_le_bytes()).collect();
        dram.write(addr, &bytes).expect("launch queue write failed");
        cache::access(caches, l2(caches), line, now, true);
        info!(self.logger, "Launch queue: entry {} written", self.tail);
        self.tail = self.tail.wrapping_add(1);
    }

    // read the ring's next entry into a free slot.  One access to the L2 brings in a whole line of entries
    fn refill(&mut self, now: u64, sms: &[SM], dram: &mut FlatMemory, caches: &mut [Cache]) {
        let (addr, line) = self.ring_entry(self.head, sms);
        if line != self.line.0 {
            self.line = (line, cache::access(caches, l2(caches), line, now, false).1);
        }
        let bytes = dram.read(addr, 16).expect("launch queue read failed");
        let w: Vec<u32> = bytes.chunks(4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).collect();
        let launch = core::Launch { words: [w[0] & !3, w[1], w[2], w[3]], kernel: (w[2] >> 16) as u8, stream: (w[0] & 3) as u8, ready: 0 };
        self.head = self.head.wrapping_add(1);
        self.fill(launch, self.line.1.max(now));
    }

    // free retired CTAs; an error on a core kills everything its CTAs' streams have
    fn retire(&mut self, sms: &mut [SM]) {
        let events = sms.iter().map(|sm| sm.events).sum();
        if self.retired_at == events {
            return;
        }
        self.retired_at = events;
        while let Some((sid, err)) =
            self.inflight.iter().find_map(|cta| sms[cta.sm].cta_err(cta.id).map(|err| (cta.sid, err)))
        {
            self.stream(sid).err = Some(err);
            let mut dropped = 0;
            self.slots.iter_mut().filter(|slot| slot.as_ref().is_some_and(|l| l.cmd.sid == sid)).for_each(|slot| {
                *slot = None;
                dropped += 1;
            });
            self.inflight.retain(|cta| {
                if cta.sid == sid {
                    sms[cta.sm].free_cta(cta.id, &cta.smem);
                    dropped += 1;
                }
                cta.sid != sid
            });
            self.stream(sid).live = 0;
            (0..dropped).for_each(|_| self.finish(sid));
        }
        let mut retired = vec![];
        self.inflight.retain(|cta| {
            let live = sms[cta.sm].cta_live(cta.id);
            if !live {
                sms[cta.sm].free_cta(cta.id, &cta.smem);
                retired.push(cta.sid);
            }
            live
        });
        for sid in retired {
            self.stream(sid).live -= 1;
            self.finish(sid);
        }
    }

    // spawn `launch`'s next CTA on `sm`, as CTA `id`, if its cores' free warp slots, regs and its smem hold it.
    // Its warps begin at `start` and jump to `kernel`'s PC
    fn place(launch: &Launch, id: CtaId, start: u32, kernel: KernelEntry, sm_idx: usize, sm: &mut SM, logger: &Logger) -> Option<Cta> {
        let cmd = launch.cmd;
        let regs = kernel.regs as usize;
        // thread idxs grouped into warps, x fastest
        let (bx, by, bz) = cmd.block;
        let thread_idxs: Vec<_> = (0..bx * by * bz).map(|t| (t % bx, t / bx % by, t / (bx * by))).collect();
        let warps: Vec<_> = thread_idxs.chunks(sm.config.num_lanes).collect();
        let slots: Vec<_> = sm.free_slots(regs).into_iter().take(warps.len()).collect();
        if slots.len() < warps.len() {
            return None;
        }
        let smem = sm.alloc_smem(cmd.smem as usize)?;
        let (gx, gy, _) = cmd.grid;
        let block_idx = (launch.tb_ctr % gx, launch.tb_ctr / gx % gy, launch.tb_ctr / (gx * gy));
        for (&(core, w), threads) in slots.iter().zip(warps) {
            let slot = [(w, threads.to_vec())];
            sm.cores[core].spawn_n_warps(start, block_idx, &slot, cmd.params, smem.start as u32, cmd.cmd_id, id, kernel.pc, regs);
        }
        info!(logger, "Spawning block_idx {:?} on sm {} (core, warp) {:?}", block_idx, sm_idx, slots);
        Some(Cta { sm: sm_idx, id, smem, sid: cmd.sid })
    }
}

impl Engine for KernelEngine {
    // a host launch goes straight to a free slot, which idle() saw before the command was handed over
    fn set_cmd(&mut self, cmd: EngineCommand) {
        let w: Vec<u32> = cmd.bytes()[..16].chunks(4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).collect();
        let stream = self.stream(cmd.sid());
        stream.outstanding += 1;
        stream.cmd_id = cmd.cmd_id();
        self.fill(core::Launch { words: [w[0], w[1], w[2], w[3]], kernel: (w[2] >> 16) as u8, stream: cmd.sid(), ready: 0 }, 0);
    }

    fn busy(&self) -> bool {
        self.streams.iter().any(|s| s.outstanding > 0) || self.done.is_some() || !self.completions.is_empty()
    }

    // a launch can go straight to a slot: one is free and none is queued ahead of it
    fn idle(&self) -> bool {
        self.head == self.tail && self.free_slot()
    }

    fn setting(&mut self, field: u8, value: u32) {
        match field {
            2 => self.ring_base = value,
            3 => self.ring_slots = value,
            _ => panic!("kernel engine has no setting {field}"),
        }
        info!(self.logger, "Launch queue: {} slots at 0x{:08x}", self.ring_slots, self.ring_base);
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        crate::common::base::CmdType::KERNEL
    }

    fn set_logger(&mut self, logger: Arc<Logger>) {
        self.logger = logger;
    }

    fn get_dma_req(&self) -> Option<&DMAReq> {
        None
    }

    fn done_dma_req(&mut self) {}

    fn get_mem_req(&self) -> Option<&MemReq> {
        None
    }

    fn set_mem_resp(&mut self, _: Option<&Vec<u8>>) {
        panic!("Kernel engine: cannot set mem resp");
    }

    fn launch(
        &mut self,
        sms: &mut [SM],
        kernels: &[KernelEntry],
        now: u64,
        dram: &mut FlatMemory,
        caches: &mut [Cache],
    ) -> Result<(), SimErr> {
        // take what the cores launched.  Each counts on its stream at once, before anything retires,
        // so a parent's exit cannot complete the stream ahead of its children
        let issued: Vec<_> = sms.iter_mut().flat_map(|sm| &mut sm.cores).flat_map(|core| core.launches.drain(..)).collect();
        for launch in issued {
            self.stream(launch.stream).outstanding += 1;
            self.arrive(launch, now, sms, dram, caches);
        }
        self.retire(sms);
        while self.head != self.tail && self.free_slot() {
            self.refill(now, sms, dram, caches);
        }
        let frees = sms.iter().map(|sm| sm.frees).sum();
        let n = sms.len();
        let start = kernels[0].pc; // entry 0 is _start
        for i in 0..self.slots.len() {
            let Some(mut launch) = self.slots[i].take() else { continue };
            if launch.ready > now {
                self.slots[i] = Some(launch);
                continue;
            }
            let kernel = kernels[launch.cmd.kernel as usize];
            self.stream(launch.cmd.sid).kernel_pc = kernel.pc;
            // a warp that exceeds an empty core's regs can never launch
            if sms.iter().all(|sm| sm.config.regs_per_core < kernel.regs as usize) {
                error!("kernel needs {} regs per warp, more than any core has", kernel.regs);
                return Err(SimErr::EXECUTION);
            }
            // place CTAs in order until the next one fits nowhere; CTA i tries SMs from i % num_sms on.
            // If nothing was freed since placement last failed, it would fail again
            while launch.tb_ctr < launch.total_tb && launch.blocked_at != Some(frees) {
                let first = launch.tb_ctr as usize % n;
                let mut order: Vec<usize> = (0..n).map(|k| (first + k) % n).collect();
                order.sort_by_key(|&idx| std::cmp::Reverse(sms[idx].free_slots(kernel.regs as usize).len())); // roomiest SM first; ties keep that order
                let placed = order.into_iter().find_map(|idx| Self::place(&launch, self.next_cta, start, kernel, idx, &mut sms[idx], &self.logger));
                let Some(cta) = placed else {
                    launch.blocked_at = Some(frees);
                    break;
                };
                launch.tb_ctr += 1;
                self.next_cta += 1;
                let stream = self.stream(cta.sid);
                stream.outstanding += 1;
                stream.live += 1;
                self.inflight.push(cta);
            }
            if launch.tb_ctr < launch.total_tb {
                self.slots[i] = Some(launch);
            } else {
                self.finish(launch.cmd.sid); // the launch itself: fully placed
            }
        }
        Ok(())
    }

    fn get_completion(&self) -> Option<Event> {
        self.done
    }
}

impl Clocked for KernelEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        self.streams.iter_mut().filter(|s| s.outstanding > 0).for_each(|s| s.cycles.tick(s.live > 0));
        self.done = self.completions.pop_front();
        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
