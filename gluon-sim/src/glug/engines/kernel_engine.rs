use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::Event;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::common::base::ThreadBlocks;
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
    pub sid: u8,
    pub host_addr: u32,
    pub sz: u32,
    pub gpu_addr: u32,
}

impl KernelCommand {
    pub fn from_engine_cmd(cmd: EngineCommand) -> Self {
        let payload = cmd.bytes();

        let host_addr = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        let sz = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
        let gpu_addr = u32::from_le_bytes([payload[8], payload[9], payload[10], payload[11]]);

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
    binary_sz: u32,
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
            .field("binary_sz", &self.binary_sz)
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
        let start_pc = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        let kernel_pc = u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
        let params_sz = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]);
        let binary_sz = u32::from_le_bytes([bytes[12], bytes[13], bytes[14], bytes[15]]);
        let stack_base_addr = u32::from_le_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]);
        let tls_base_addr = u32::from_le_bytes([bytes[20], bytes[21], bytes[22], bytes[23]]);
        let grid = (
            u32::from_le_bytes([bytes[24], bytes[25], bytes[26], bytes[27]]),
            u32::from_le_bytes([bytes[28], bytes[29], bytes[30], bytes[31]]),
            u32::from_le_bytes([bytes[32], bytes[33], bytes[34], bytes[35]]),
        );
        let block = (
            u32::from_le_bytes([bytes[36], bytes[37], bytes[38], bytes[39]]),
            u32::from_le_bytes([bytes[40], bytes[41], bytes[42], bytes[43]]),
            u32::from_le_bytes([bytes[44], bytes[45], bytes[46], bytes[47]]),
        );
        let printf_host_addr = u32::from_le_bytes([bytes[48], bytes[49], bytes[50], bytes[51]]);
        let regs_per_thread = bytes[52];
        let shmem_per_block = u32::from_le_bytes([bytes[53], bytes[54], bytes[55], bytes[56]]);
        let flags = bytes[57];
        KernelPayload {
            start_pc,
            kernel_pc,
            params_sz,
            binary_sz,
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

pub struct KernelEngine {
    cmd: Option<KernelCommand>,

    dma_req: Option<DMAReq>,
    mem_req: Option<MemReq>,
    mem_resp: Option<MemResp>,

    kernel_payload: KernelPayload,
    state: KernelEngineState,
    tb_size: u32,
    tb_ctr: u32,
    total_tb: u32,
    tb_done: u32,

    gluls: Vec<GLULStatus>,
    glul_req: GLULReq,

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
            tb_size: 0,
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

    fn set_gluls(&mut self, gluls: Vec<GLULStatus>) {
        self.gluls = gluls
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

    fn get_glul_req(&self) -> Option<&GLULReq> {
        if self.glul_req.thread_blocks.is_some() {
            Some(&self.glul_req)
        } else {
            None
        }
    }

    fn clear_glul_req(&mut self) {
        self.glul_req.thread_blocks = None;
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
                    self.tb_done = 0;
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
                    let mut dma_req = DMAReq::default();
                    dma_req.done = false;
                    dma_req.dir = DMADir::H2D;
                    dma_req.src_addr = self
                        .cmd
                        .expect("Unreachable:Kernel command not set")
                        .host_addr;
                    dma_req.target_addr = self
                        .cmd
                        .expect("Unreachable:Kernel command not set")
                        .gpu_addr;
                    dma_req.sz = self.cmd.expect("Unreachable:Kernel command not set").sz;
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
                        self.total_tb = self.kernel_payload.grid.0 as u32
                            * self.kernel_payload.grid.1 as u32
                            * self.kernel_payload.grid.2 as u32;
                        self.tb_size = self.kernel_payload.block.0 as u32
                            * self.kernel_payload.block.1 as u32
                            * self.kernel_payload.block.2 as u32;
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
                let available_tbs = self.total_tb - self.tb_ctr;

                if available_tbs > 0 {
                    let threads_per_block = self.kernel_payload.block.0 as u32
                        * self.kernel_payload.block.1 as u32
                        * self.kernel_payload.block.2 as u32;
                    if let Some((glul_if_idx, n_tb)) = self
                        .gluls
                        .iter()
                        .filter(|glul| !*glul.busy.read().expect("GLUL busy poisoned"))
                        .enumerate()
                        .map(|(idx, glul)| {
                            let glul_cfg = glul.config;
                            let warps_per_tb = (threads_per_block / glul_cfg.num_lanes as u32).max(1);
                            let cores_per_tb = (warps_per_tb as f32 / glul_cfg.num_warps as f32).ceil() as usize;
                            (
                                idx,
                                (glul_cfg.num_cores
                                    / cores_per_tb).min(
                                        glul_cfg.regs_per_core * glul_cfg.num_cores
                                            / (self.kernel_payload.regs_per_thread as usize
                                                * glul_cfg.num_lanes)
                                                .min(
                                                    glul_cfg.shmem
                                                        / self.kernel_payload.shmem_per_block as usize,
                                                ),
                                    ),
                            )
                        })
                        .filter(|(_, value)| *value > 0)
                        .min_by_key(|(_, value)| *value)
                    {
                        let n_tb_req = (n_tb as u32).min(available_tbs);
                        let mut block_idxs = Vec::new();
                        for _ in 0..n_tb_req {
                            let gx = self.kernel_payload.grid.0;
                            let gy = self.kernel_payload.grid.1;
                            let plane = gx * gy;
                            let block_z = self.tb_ctr / plane;
                            let rem = self.tb_ctr % plane;
                            let block_y = rem / gx;
                            let block_x = rem % gx;
                            block_idxs.push((block_x, block_y, block_z));
                            self.tb_ctr += 1;
                        }
                        self.glul_req.thread_blocks = Some(ThreadBlocks {
                            pc: self.kernel_payload.start_pc,
                            block_idxs: block_idxs,
                            block_dim: self.kernel_payload.block,
                            regs: self.kernel_payload.regs_per_thread as u32,
                            shmem: self.kernel_payload.shmem_per_block,
                            bp: self.cmd.expect("Unreachable: Kernel command not set").gpu_addr,
                        });
                        self.glul_req.idx = glul_if_idx;
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
