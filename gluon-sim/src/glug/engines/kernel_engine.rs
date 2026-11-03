use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::Event;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::glug::engine::Engine;
use crate::glug::engine::EngineCommand;
use crate::sm::SM;
use cyclotron::info;
use cyclotron::muon::warp::ExecErr;
use cyclotron::sim::log::Logger;
use log::error;
use serde::Deserialize;
use std::fmt;
use std::ops::Range;
use std::sync::Arc;

pub enum KernelEngineState {
    S0,
    S1,
    S2,
    S3,
    S4,
    S5,
}

#[derive(Default, Debug, Clone, Deserialize)]
#[serde(default)]
pub struct KernelEngineConfig {}

#[derive(Debug, Default, Clone, Copy)]
pub struct KernelCommand {
    pub sid: u8,
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
    cores: Vec<(usize, usize)>, // (core, regs held there)
    smem: Range<usize>,
}

impl Cta {
    fn free(&self, sms: &mut [SM]) {
        let sm = &mut sms[self.sm];
        for &(core, regs) in &self.cores {
            sm.running[core] = false;
            sm.errs[core] = None;
            sm.free_regs[core] += regs;
        }
        sm.free_smem(&self.smem);
    }
}

pub struct KernelEngine {
    cmd: Option<KernelCommand>,

    dma_req: Option<DMAReq>,
    mem_req: Option<MemReq>,
    mem_resp: Option<MemResp>,

    kernel_payload: KernelPayload,
    state: KernelEngineState,
    tb_ctr: u32,
    total_tb: u32,
    inflight: Vec<Cta>,

    err: Option<Result<(), ExecErr>>,

    logger: Arc<Logger>,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn new(_config: &KernelEngineConfig) -> Self {
        KernelEngine {
            cmd: None,
            dma_req: None,
            mem_req: None,
            mem_resp: None,
            kernel_payload: KernelPayload::default(),
            state: KernelEngineState::S0,
            tb_ctr: 0,
            total_tb: 0,
            inflight: vec![],
            err: None,
            logger: Arc::new(Logger::new(0)),
        }
    }
}

impl KernelEngine {
    // free retired CTAs; an error on any core kills the whole kernel
    fn retire(&mut self, sms: &mut [SM]) {
        let err = self
            .inflight
            .iter()
            .find_map(|cta| cta.cores.iter().find_map(|&(core, _)| sms[cta.sm].errs[core].clone()));
        if let Some(err) = err {
            self.inflight.drain(..).for_each(|cta| cta.free(sms));
            self.err = Some(Err(err));
            self.state = KernelEngineState::S5;
            return;
        }
        self.inflight.retain(|cta| {
            let retired = cta.cores.iter().all(|&(core, _)| sms[cta.sm].cores[core].all_warps_retired());
            if retired {
                cta.free(sms);
            }
            !retired
        });
    }
}

impl Engine for KernelEngine {
    fn set_cmd(&mut self, cmd: EngineCommand) {
        self.cmd = Some(KernelCommand::from_engine_cmd(cmd));
    }

    fn busy(&self) -> bool {
        !matches!(self.state, KernelEngineState::S0)
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
        if !matches!(self.state, KernelEngineState::S3) || self.tb_ctr == self.total_tb {
            return Ok(());
        }
        let p = self.kernel_payload;
        let regs = p.regs_per_thread as usize;
        // a warp that exceeds an empty core's regs can never launch
        if sms.iter().all(|sm| sm.config.regs_per_core < regs) {
            error!("kernel needs {} regs per warp, more than any core has", regs);
            return Err(SimErr::EXECUTION);
        }
        let (bx, by, bz) = p.block;
        let threads = bx * by * bz;
        let smem = p.shmem_per_block as usize;

        // pick the idle SM that fits the fewest CTAs: (sm, CTAs, warps per core)
        let Some((sm_idx, n_tb, warps_per_core)) = sms
            .iter()
            .enumerate()
            .filter(|(_, sm)| !sm.running.contains(&true))
            .filter_map(|(idx, sm)| {
                let warps_per_core = sm.config.num_warps.min(sm.free_regs.iter().min()? / regs);
                if warps_per_core == 0 {
                    return None;
                }
                let cores_per_tb = (threads as usize).div_ceil(sm.config.num_lanes).div_ceil(warps_per_core);
                // 0 bytes of smem per block places no limit; the SM is idle so all of it is free
                let smem_limit = sm.config.shmem.checked_div(smem).unwrap_or(usize::MAX);
                let n_tb = (sm.config.num_cores / cores_per_tb).min(smem_limit);
                (n_tb > 0).then_some((idx, n_tb, warps_per_core))
            })
            .min_by_key(|&(_, n_tb, _)| n_tb)
        else {
            return Ok(());
        };

        let sm = &mut sms[sm_idx];
        // thread idxs grouped into warps, x fastest
        let thread_idxs: Vec<_> = (0..threads).map(|t| (t % bx, t / bx % by, t / (bx * by))).collect();
        let warps: Vec<_> = thread_idxs.chunks(sm.config.num_lanes).map(<[_]>::to_vec).collect();
        let bp = self.cmd.expect("Unreachable: Kernel command not set").gpu_addr;
        let (gx, gy, _) = p.grid;
        let mut next_core = 0;
        for _ in 0..n_tb.min((self.total_tb - self.tb_ctr) as usize) {
            let Some(smem_range) = sm.alloc_smem(smem) else {
                break;
            };
            let block_idx = (self.tb_ctr % gx, self.tb_ctr / gx % gy, self.tb_ctr / (gx * gy));
            self.tb_ctr += 1;
            // pack the CTA's warps onto consecutive cores, warps_per_core each
            let cores: Vec<_> = (next_core..)
                .zip(warps.chunks(warps_per_core))
                .map(|(core, chunk)| {
                    let core_regs = chunk.len() * regs;
                    let slots: Vec<_> = chunk.iter().cloned().enumerate().collect();
                    sm.cores[core].spawn_n_warps(p.start_pc, block_idx, &slots, bp, smem_range.start as u32);
                    sm.running[core] = true;
                    sm.free_regs[core] -= core_regs;
                    (core, core_regs)
                })
                .collect();
            next_core += cores.len();
            info!(self.logger, "Spawning block_idx {:?} on sm {} cores {:?}", block_idx, sm_idx, cores);
            self.inflight.push(Cta { sm: sm_idx, cores, smem: smem_range });
        }
        Ok(())
    }

    fn get_completion(&self) -> Option<Event> {
        self.err
            .as_ref()
            .map(|err| {
                Event::from_kernel_err(
                    self.cmd.expect("Command not set, no completion exists").sid,
                    err.clone(),
                )
            })
    }
}

impl Clocked for KernelEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        match &self.state {
            KernelEngineState::S0 => {
                if let Some(cmd) = &self.cmd {
                    self.state = KernelEngineState::S1;
                    self.tb_ctr = 0;
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
                    if self.mem_resp.is_some() {
                        self.mem_req = None;
                        self.kernel_payload = KernelPayload::from_bytes(&self.mem_resp.as_ref().expect("Unreachable:Kernel mem resp not set").data.as_ref().expect("Unreachable:Kernel mem resp no data"));
                        info!(
                            self.logger,
                            "Received kernel payload: {:?}", self.kernel_payload
                        );
                        let (gx, gy, gz) = self.kernel_payload.grid;
                        self.total_tb = gx * gy * gz;
                        self.state = KernelEngineState::S3;
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

            KernelEngineState::S3 => {
                if self.tb_ctr == self.total_tb && self.inflight.is_empty() {
                    self.state = KernelEngineState::S4;
                }
            }

            KernelEngineState::S4 => {
                self.state = KernelEngineState::S5;
                self.err = Some(Ok(()));
            }

            KernelEngineState::S5 => {
                self.state = KernelEngineState::S0;
                self.cmd = None;
                self.err = None;
            }
        };

        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
