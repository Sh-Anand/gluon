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
use cyclotron::muon::sm::SM;
use cyclotron::info;
use cyclotron::muon::core::CtaId;
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
    // the entry's four words are laid out in sw/driver/include/core.h
    pub fn from_engine_cmd(cmd: EngineCommand) -> Self {
        let w: Vec<u32> = cmd.bytes()[..16].chunks(4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).collect();
        KernelCommand {
            sid: cmd.sid(),
            cmd_id: cmd.cmd_id(),
            params: w[0],
            grid: (w[1] & 0xffff, w[1] >> 16, w[2] & 0xffff),
            block: ((w[3] & 0x3ff) + 1, (w[3] >> 10 & 0x3ff) + 1, (w[2] >> 24 & 0x3f) + 1),
            kernel: (w[2] >> 16) as u8,
            smem: (w[3] >> 20) * 16,
        }
    }
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
}

// a stream's kernel command, which is done when nothing of it is outstanding
#[derive(Default)]
struct Stream {
    outstanding: u32, // its launches still placing, plus its CTAs running
    live: u32,        // its CTAs running
    err: Option<ExecErr>,
    kernel_pc: u32,
    cycles: Cycles, // since the command arrived
}

pub struct KernelEngine {
    slots: Vec<Option<Launch>>,
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
    // the launch goes straight to a free slot, which idle() saw before the command was handed over
    fn set_cmd(&mut self, cmd: EngineCommand) {
        let cmd = KernelCommand::from_engine_cmd(cmd);
        info!(self.logger, "Kernel engine: {:?}", cmd);
        let (gx, gy, gz) = cmd.grid;
        let launch = Launch { cmd, tb_ctr: 0, total_tb: gx * gy * gz, blocked_at: None };
        *self.slots.iter_mut().find(|slot| slot.is_none()).expect("no free launch slot") = Some(launch);
        self.stream(cmd.sid).outstanding += 1;
    }

    fn busy(&self) -> bool {
        self.streams.iter().any(|s| s.outstanding > 0) || self.done.is_some() || !self.completions.is_empty()
    }

    fn idle(&self) -> bool {
        self.slots.iter().any(Option::is_none)
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

    fn launch(&mut self, sms: &mut [SM], kernels: &[KernelEntry]) -> Result<(), SimErr> {
        self.retire(sms);
        let frees = sms.iter().map(|sm| sm.frees).sum();
        let n = sms.len();
        let start = kernels[0].pc; // entry 0 is _start
        for i in 0..self.slots.len() {
            let Some(mut launch) = self.slots[i].take() else { continue };
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
                let placed = (0..n)
                    .map(|k| (first + k) % n)
                    .find_map(|idx| Self::place(&launch, self.next_cta, start, kernel, idx, &mut sms[idx], &self.logger));
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
