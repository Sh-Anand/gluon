use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::Cycles;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::Event;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::glug::engine::Engine;
use crate::glug::engine::EngineCommand;
use cyclotron::muon::sm::SM;
use cyclotron::info;
use cyclotron::muon::core::CtaId;
use cyclotron::muon::warp::ExecErr;
use cyclotron::sim::log::Logger;
use log::error;
use serde::Deserialize;
use std::collections::VecDeque;
use std::fmt;
use std::ops::Range;
use std::sync::Arc;

// the front end takes one command at a time: idle, copying its header in, reading the header
pub enum KernelEngineState {
    S0,
    S1,
    S2,
}

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

#[derive(Debug, Default, Clone, Copy)]
pub struct KernelCommand {
    pub sid: u8,
    pub cmd_id: u64,
    pub host_addr: u64,
    pub sz: u32,
    pub gpu_addr: u32,
}

impl KernelCommand {
    pub fn from_engine_cmd(cmd: EngineCommand) -> Self {
        let payload = cmd.bytes();

        let host_addr = u64::from_le_bytes(payload[0..8].try_into().unwrap());
        let sz = u32::from_le_bytes(payload[8..12].try_into().unwrap());
        let gpu_addr = u32::from_le_bytes(payload[12..16].try_into().unwrap());

        KernelCommand {
            sid: cmd.sid(),
            cmd_id: cmd.cmd_id(),
            host_addr,
            sz,
            gpu_addr,
        }
    }

    pub fn sid(&self) -> u8 {
        self.sid
    }
}

#[derive(Default, Clone, Copy)]
pub struct KernelPayload {
    start_pc: u32,
    kernel_pc: u32,
    params_sz: u32,
    stack_base_addr: u32,
    tls_base_addr: u32,
    grid: (u32, u32, u32),
    block: (u32, u32, u32),
    regs_per_thread: u8,
    shmem_per_block: u32,
    flags: u8,
    printf_host_addr: u32,
}

impl fmt::Debug for KernelPayload {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("KernelPayload")
            .field("start_pc", &format_args!("0x{:08x}", self.start_pc))
            .field("kernel_pc", &format_args!("0x{:08x}", self.kernel_pc))
            .field("params_sz", &self.params_sz)
            .field("stack_base_addr", &format_args!("0x{:08x}", self.stack_base_addr))
            .field("tls_base_addr", &format_args!("0x{:08x}", self.tls_base_addr))
            .field("grid", &self.grid)
            .field("block", &self.block)
            .field("regs_per_thread", &self.regs_per_thread)
            .field("shmem_per_block", &self.shmem_per_block)
            .field("flags", &self.flags)
            .field("printf_host_addr", &format_args!("0x{:08x}", self.printf_host_addr))
            .finish()
    }
}

impl KernelPayload {
    pub fn from_bytes(bytes: &[u8]) -> Self {
        let start_pc = u32::from_le_bytes(bytes[0..4].try_into().unwrap());
        let kernel_pc = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
        let params_sz = u32::from_le_bytes(bytes[8..12].try_into().unwrap());
        let stack_base_addr = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
        let tls_base_addr = u32::from_le_bytes(bytes[16..20].try_into().unwrap());
        let grid = (
            u32::from_le_bytes(bytes[20..24].try_into().unwrap()),
            u32::from_le_bytes(bytes[24..28].try_into().unwrap()),
            u32::from_le_bytes(bytes[28..32].try_into().unwrap()),
        );
        let block = (
            u32::from_le_bytes(bytes[32..36].try_into().unwrap()),
            u32::from_le_bytes(bytes[36..40].try_into().unwrap()),
            u32::from_le_bytes(bytes[40..44].try_into().unwrap()),
        );
        let printf_host_addr = u32::from_le_bytes(bytes[44..48].try_into().unwrap());
        let regs_per_thread = bytes[48];
        let shmem_per_block = u32::from_le_bytes(bytes[49..53].try_into().unwrap());
        let flags = bytes[53];
        KernelPayload {
            start_pc,
            kernel_pc,
            params_sz,
            stack_base_addr,
            tls_base_addr,
            grid,
            block,
            regs_per_thread,
            shmem_per_block,
            flags,
            printf_host_addr,
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
    payload: KernelPayload,
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
    cmd: Option<KernelCommand>,

    dma_req: Option<DMAReq>,
    mem_req: Option<MemReq>,
    mem_resp: Option<MemResp>,
    state: KernelEngineState,

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
            cmd: None,
            dma_req: None,
            mem_req: None,
            mem_resp: None,
            state: KernelEngineState::S0,
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

    // spawn `launch`'s next CTA on `sm`, as CTA `id`, if its cores' free warp slots, regs and its smem hold it
    fn place(launch: &Launch, id: CtaId, sm_idx: usize, sm: &mut SM, logger: &Logger) -> Option<Cta> {
        let (p, cmd) = (launch.payload, launch.cmd);
        let regs = p.regs_per_thread as usize;
        // thread idxs grouped into warps, x fastest
        let (bx, by, bz) = p.block;
        let thread_idxs: Vec<_> = (0..bx * by * bz).map(|t| (t % bx, t / bx % by, t / (bx * by))).collect();
        let warps: Vec<_> = thread_idxs.chunks(sm.config.num_lanes).collect();
        let slots: Vec<_> = sm.free_slots(regs).into_iter().take(warps.len()).collect();
        if slots.len() < warps.len() {
            return None;
        }
        let smem = sm.alloc_smem(p.shmem_per_block as usize)?;
        let (gx, gy, _) = p.grid;
        let block_idx = (launch.tb_ctr % gx, launch.tb_ctr / gx % gy, launch.tb_ctr / (gx * gy));
        for (&(core, w), threads) in slots.iter().zip(warps) {
            let slot = [(w, threads.to_vec())];
            sm.cores[core].spawn_n_warps(p.start_pc, block_idx, &slot, cmd.gpu_addr, smem.start as u32, cmd.cmd_id, id, regs);
        }
        info!(logger, "Spawning block_idx {:?} on sm {} (core, warp) {:?}", block_idx, sm_idx, slots);
        Some(Cta { sm: sm_idx, id, smem, sid: cmd.sid })
    }
}

impl Engine for KernelEngine {
    fn set_cmd(&mut self, cmd: EngineCommand) {
        self.cmd = Some(KernelCommand::from_engine_cmd(cmd));
    }

    fn busy(&self) -> bool {
        self.cmd.is_some()
            || self.streams.iter().any(|s| s.outstanding > 0)
            || self.done.is_some()
            || !self.completions.is_empty()
    }

    // the front end is free and there is a slot for the launch it would produce
    fn idle(&self) -> bool {
        self.cmd.is_none() && self.slots.iter().any(Option::is_none)
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        crate::common::base::CmdType::KERNEL
    }

    fn set_logger(&mut self, logger: Arc<Logger>) {
        self.logger = logger;
    }

    fn get_dma_req(&self) -> Option<&DMAReq> {
        self.dma_req.as_ref()
    }

    fn done_dma_req(&mut self) {
        self.dma_req.as_mut().expect("Kernel engine: DMA req not set").done = true;
    }

    fn get_mem_req(&self) -> Option<&MemReq> {
        self.mem_req.as_ref()
    }

    fn set_mem_resp(&mut self, data: Option<&Vec<u8>>) {
        self.mem_resp = data.map(|bytes| MemResp { data: Some(bytes.clone()) });
    }

    fn launch(&mut self, sms: &mut [SM]) -> Result<(), SimErr> {
        self.retire(sms);
        let frees = sms.iter().map(|sm| sm.frees).sum();
        let n = sms.len();
        for i in 0..self.slots.len() {
            let Some(mut launch) = self.slots[i].take() else { continue };
            let regs = launch.payload.regs_per_thread as usize;
            // a warp that exceeds an empty core's regs can never launch
            if sms.iter().all(|sm| sm.config.regs_per_core < regs) {
                error!("kernel needs {} regs per warp, more than any core has", regs);
                return Err(SimErr::EXECUTION);
            }
            // place CTAs in order until the next one fits nowhere; CTA i tries SMs from i % num_sms on.
            // If nothing was freed since placement last failed, it would fail again
            while launch.tb_ctr < launch.total_tb && launch.blocked_at != Some(frees) {
                let first = launch.tb_ctr as usize % n;
                let placed = (0..n)
                    .map(|k| (first + k) % n)
                    .find_map(|idx| Self::place(&launch, self.next_cta, idx, &mut sms[idx], &self.logger));
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
        if let (Some(cmd), false) = (self.cmd, matches!(self.state, KernelEngineState::S0)) {
            self.stream(cmd.sid).cycles.tick(false);
        }
        self.streams.iter_mut().filter(|s| s.outstanding > 0).for_each(|s| s.cycles.tick(s.live > 0));
        self.done = self.completions.pop_front();
        match &self.state {
            KernelEngineState::S0 => {
                if let Some(cmd) = &self.cmd {
                    self.state = KernelEngineState::S1;
                     info!(
                        self.logger,
                        "Init kernel engine: id={} host=0x{:08x} size=0x{:08x} gpu=0x{:08x}",
                        cmd.sid, cmd.host_addr, cmd.sz, cmd.gpu_addr
                    );
                }
            }

            KernelEngineState::S1 => {
                if let Some(dma_req) = &self.dma_req {
                    if dma_req.done {
                        self.dma_req = None;
                        self.state = KernelEngineState::S2;
                    }
                } else {
                    let cmd = self.cmd.expect("Unreachable:Kernel command not set");
                    let dma_req = DMAReq {
                        dir: DMADir::H2D,
                        src_addr: cmd.host_addr,
                        target_addr: cmd.gpu_addr as u64,
                        sz: cmd.sz,
                        host_runtime: false,
                        done: false,
                    };
                    self.dma_req = Some(dma_req);
                    info!(
                        self.logger,
                        "Kernel engine DMA req: {:?}", dma_req
                    );
                }
            }

            KernelEngineState::S2 => {
                if self.mem_req.is_some() {
                    if let Some(resp) = self.mem_resp.take() {
                        self.mem_req = None;
                        let payload = KernelPayload::from_bytes(&resp.data.expect("Unreachable:Kernel mem resp no data"));
                        info!(
                            self.logger,
                            "Received kernel payload: {:?}", payload
                        );
                        // the launch goes straight to a free slot, which idle() saw before the command was taken
                        let cmd = self.cmd.take().expect("Unreachable:Kernel command not set");
                        let (gx, gy, gz) = payload.grid;
                        let launch = Launch { cmd, payload, tb_ctr: 0, total_tb: gx * gy * gz, blocked_at: None };
                        *self.slots.iter_mut().find(|slot| slot.is_none()).expect("no free launch slot") = Some(launch);
                        let stream = self.stream(cmd.sid);
                        stream.outstanding += 1;
                        stream.kernel_pc = payload.kernel_pc;
                        self.state = KernelEngineState::S0;
                    }
                } else {
                    self.mem_req = Some(MemReq {
                        addr: self
                            .cmd
                            .expect("Unreachable:Kernel command not set")
                            .gpu_addr,
                        write: false,
                        bytes: size_of::<KernelPayload>() as u32,
                        data: vec![],
                    });
                    info!(
                        self.logger,
                        "Queued mem {:?}", self.mem_req
                    );
                }
            }
        };

        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
