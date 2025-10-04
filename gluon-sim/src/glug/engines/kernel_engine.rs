use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::Event;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::common::base::ThreadBlock;
use crate::glug::engine::Engine;
use crate::glug::engine::EngineCommand;
use crate::glul::glul::GLULReq;
use crate::glul::glul::GLULStatus;
use cyclotron::info;
use cyclotron::muon::warp::ExecErr;
use cyclotron::sim::log::Logger;
use serde::Deserialize;
use std::fmt;
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
    pub id: u8,
    pub host_addr: u32,
    pub sz: u32,
    pub gpu_addr: u32,
}

impl KernelCommand {
    pub fn from_engine_cmd(cmd: EngineCommand) -> Self {
        let payload = cmd.payload();

        let host_addr = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        let sz = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
        let gpu_addr = u32::from_le_bytes([payload[8], payload[9], payload[10], payload[11]]);

        KernelCommand {
            id: cmd.id(),
            host_addr,
            sz,
            gpu_addr,
        }
    }

    pub fn id(&self) -> u8 {
        self.id
    }
}

#[derive(Default, Clone, Copy)]
pub struct KernelPayload {
    entry_pc: u32,
    grid: (u16, u16, u16),
    block: (u16, u16, u16),
    regs_per_thread: u8,
    shmem_per_block: u32,
    flags: u8,
    printf_host_addr: u32,
    params_sz: u32,
    binary_sz: u32,
}

impl fmt::Debug for KernelPayload {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("KernelPayload")
            .field("entry_pc", &format_args!("0x{:08x}", self.entry_pc))
            .field("grid", &self.grid)
            .field("block", &self.block)
            .field("regs_per_thread", &self.regs_per_thread)
            .field("shmem_per_block", &self.shmem_per_block)
            .field("flags", &self.flags)
            .field(
                "printf_host_addr",
                &format_args!("0x{:08x}", self.printf_host_addr),
            )
            .field("params_sz", &self.params_sz)
            .field("binary_sz", &self.binary_sz)
            .finish()
    }
}

impl KernelPayload {
    pub fn from_bytes(bytes: &[u8]) -> Self {
        let entry_pc = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        let grid = (
            u16::from_le_bytes([bytes[4], bytes[5]]),
            u16::from_le_bytes([bytes[6], bytes[7]]),
            u16::from_le_bytes([bytes[8], bytes[9]]),
        );
        let block = (
            u16::from_le_bytes([bytes[10], bytes[11]]),
            u16::from_le_bytes([bytes[12], bytes[13]]),
            u16::from_le_bytes([bytes[14], bytes[15]]),
        );
        let regs_per_thread = bytes[16];
        let shmem_per_block = u32::from_le_bytes([bytes[17], bytes[18], bytes[19], bytes[20]]);
        let flags = bytes[21];
        let printf_host_addr = u32::from_le_bytes([bytes[22], bytes[23], bytes[24], bytes[25]]);
        let params_sz = u32::from_le_bytes([bytes[26], bytes[27], bytes[28], bytes[29]]);
        let binary_sz = u32::from_le_bytes([bytes[30], bytes[31], bytes[32], bytes[33]]);
        KernelPayload {
            entry_pc,
            grid,
            block,
            regs_per_thread,
            shmem_per_block,
            flags,
            printf_host_addr,
            params_sz,
            binary_sz,
        }
    }
}

pub struct KernelEngine {
    cmd: Option<(KernelCommand, usize)>, // (command, completion idx)

    dma_req: Option<DMAReq>,
    mem_req: Option<MemReq>,
    mem_resp: Option<MemResp>,

    state: KernelEngineState,
    tb_ctr: u32,
    total_tb: u32,
    tb_done: u32,

    gluls: Vec<GLULStatus>,
    glul_req: GLULReq,

    err: Option<Result<(), ExecErr>>,

    logger: Arc<Logger>,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn new(_config: KernelEngineConfig) -> Self {
        KernelEngine {
            cmd: None,
            dma_req: None,
            mem_req: None,
            mem_resp: None,
            state: KernelEngineState::S0,
            tb_ctr: 0,
            total_tb: 0,
            tb_done: 0,
            gluls: vec![],
            glul_req: GLULReq::default(),
            err: None,
            logger: Arc::new(Logger::new(0)),
        }
    }
}

impl Engine for KernelEngine {
    fn set_cmd(&mut self, cmd: EngineCommand, completion_idx: usize) {
        self.cmd = Some((KernelCommand::from_engine_cmd(cmd), completion_idx));
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

    fn set_gluls(&mut self, gluls: Vec<GLULStatus>) {
        self.gluls = gluls
    }

    fn get_dma_req(&self) -> Option<&DMAReq> {
        self.dma_req.as_ref()
    }

    fn done_dma_req(&mut self) {
        self.dma_req.expect("Kernel engine: DMA req not set").done = true;
    }

    fn get_mem_req(&self) -> Option<&MemReq> {
        self.mem_req.as_ref()
    }

    fn set_mem_resp(&mut self, data: Option<&Vec<u8>>) {
        self.mem_resp = data.map(|bytes| MemResp { data: bytes.clone() });
    }

    fn get_glul_req(&self) -> Option<&GLULReq> {
        if self.glul_req.n_tb > 0 {
            Some(&self.glul_req)
        } else {
            None
        }
    }

    fn clear_glul_req(&mut self) {
        self.glul_req.n_tb = 0;
    }

    fn notify_glul_done(&mut self, tbs: u32) {
        assert!(self.tb_done + tbs <= self.total_tb);
        self.tb_done += tbs;
    }

    fn notify_glul_err(&mut self, err: ExecErr) {
        assert_ne!(self.total_tb, 0);
        assert_ne!(self.tb_ctr, 0);
        self.state = KernelEngineState::S5;
        self.err = Some(Err(err));
    }

    fn get_completion(&self) -> Option<(Event, usize)> {
        self.err
            .as_ref()
            .map(|err| {
                Event::from_kernel_err(
                    self.cmd
                        .expect("Command not set, no completion exists")
                        .0
                        .id,
                    err.clone(),
                )
            })
            .map(|event| {
                (
                    event,
                    self.cmd.expect("Command not set, no completion exists").1,
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
                        cmd.0.id, cmd.0.host_addr, cmd.0.sz, cmd.0.gpu_addr
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
                    let mut dma_req = DMAReq::default();
                    dma_req.done = true;
                    dma_req.dir = DMADir::H2D;
                    dma_req.src_addr = self
                        .cmd
                        .expect("Unreachable:Kernel command not set")
                        .0
                        .host_addr;
                    dma_req.target_addr = self
                        .cmd
                        .expect("Unreachable:Kernel command not set")
                        .0
                        .gpu_addr;
                    dma_req.sz = self.cmd.expect("Unreachable:Kernel command not set").0.sz;
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
                        self.state = KernelEngineState::S3;
                    }
                } else {
                    self.mem_req = Some(MemReq {
                        addr: self
                            .cmd
                            .expect("Unreachable:Kernel command not set")
                            .0
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
                let kernel_payload = KernelPayload::from_bytes(&self.mem_resp.as_ref().expect("Kernel engine: Mem resp not set").data);
                self.total_tb = kernel_payload.grid.0 as u32
                    * kernel_payload.grid.1 as u32
                    * kernel_payload.grid.2 as u32;
                let tb_size = kernel_payload.block.0 as u32
                    * kernel_payload.block.1 as u32
                    * kernel_payload.block.2 as u32;
                let available_tbs = self.total_tb - self.tb_ctr;

                if available_tbs > 0 {
                    if let Some((glul_if_idx, n_tb)) = self
                        .gluls
                        .iter()
                        .filter(|glul| !*glul.busy.read().expect("GLUL busy poisoned"))
                        .enumerate()
                        .map(|(idx, glul)| {
                            let glul_cfg = glul.config;
                            (
                                idx,
                                glul_cfg.num_cores * glul_cfg.num_warps * glul_cfg.num_lanes
                                    / (tb_size as usize).min(
                                        glul_cfg.regs_per_core * glul_cfg.num_cores
                                            / (kernel_payload.regs_per_thread as usize
                                                * glul_cfg.num_lanes)
                                                .min(
                                                    glul_cfg.shmem
                                                        / kernel_payload.shmem_per_block as usize,
                                                ),
                                    ),
                            )
                        })
                        .filter(|(_, value)| *value > 0)
                        .min_by_key(|(_, value)| *value)
                    {
                        self.glul_req.n_tb = (n_tb as u32).min(available_tbs);
                        self.glul_req.thread_block = ThreadBlock {
                            id: self.tb_ctr,
                            pc: self
                                .cmd
                                .expect("Unreachable:Kernel command not set")
                                .0
                                .gpu_addr
                                + size_of::<KernelPayload>() as u32
                                + kernel_payload.params_sz
                                + kernel_payload.entry_pc,
                            dim: kernel_payload.block,
                            regs: kernel_payload.regs_per_thread as u32,
                            shmem: kernel_payload.shmem_per_block as u32,
                        };
                        self.glul_req.idx = glul_if_idx;

                        self.tb_ctr += self.glul_req.n_tb as u32;
                    }
                }

                if self.tb_done == self.total_tb {
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
