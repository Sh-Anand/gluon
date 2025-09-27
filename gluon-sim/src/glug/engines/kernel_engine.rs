use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::common::base::ThreadBlock;
use crate::glug::engine::Engine;
use crate::glug::engine::EngineCommand;
use crate::glul::glul::GLULConfig;
use crate::glul::glul::GLULInterface;
use serde::Deserialize;
use std::mem::size_of;

pub enum KernelEngineState {
    S0,
    S1,
    S2,
    S3,
    S4,
    S5,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct KernelEngineConfig {
    pub gluls: Vec<GLULConfig>,
}

impl Default for KernelEngineConfig {
    fn default() -> Self {
        KernelEngineConfig {
            gluls: vec![GLULConfig::default()],
        }
    }
}

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

#[derive(Debug, Default, Clone, Copy)]
pub struct KernelPayload {
    entry_pc: u32,
    grid: (u16, u16, u16),
    block: (u16, u16, u16),
    regs_per_thread: u8,
    shmem_per_block: u32,
    flags: u8,
    printf_host_addr: u32,
    binary_sz: u32,
    params_sz: u32,
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
        let binary_sz = u32::from_le_bytes([bytes[26], bytes[27], bytes[28], bytes[29]]);
        let params_sz = u32::from_le_bytes([bytes[30], bytes[31], bytes[32], bytes[33]]);
        KernelPayload {
            entry_pc,
            grid,
            block,
            regs_per_thread,
            shmem_per_block,
            flags,
            printf_host_addr,
            binary_sz,
            params_sz,
        }
    }
}

pub struct KernelEngine {
    cmd: KernelCommand,

    dma_req: DMAReq,

    mem_req: MemReq,
    mem_resp: MemResp,

    state: KernelEngineState,
    tb_ctr: u32,
    total_tb: u32,
    done_tb: u32,

    gluls: Vec<GLULInterface>,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn new(_config: KernelEngineConfig) -> Self {
        KernelEngine {
            cmd: KernelCommand::default(),
            dma_req: DMAReq::default(),
            mem_req: MemReq::default(),
            mem_resp: MemResp::default(),
            state: KernelEngineState::S0,
            tb_ctr: 0,
            total_tb: 0,
            done_tb: 0,
            gluls: (0.._config.gluls.len())
                .map(|i| GLULInterface::new(_config.gluls[i]))
                .collect(),
        }
    }
}

impl Engine for KernelEngine {
    fn init(&mut self, cmd: EngineCommand) {
        self.cmd = KernelCommand::from_engine_cmd(cmd);
        self.state = KernelEngineState::S1;

        println!(
            "Init kernel engine: id={} host=0x{:08x} size=0x{:08x} gpu=0x{:08x}",
            self.cmd.id, self.cmd.host_addr, self.cmd.sz, self.cmd.gpu_addr
        );
    }

    fn busy(&self) -> bool {
        !matches!(self.state, KernelEngineState::S0)
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        crate::common::base::CmdType::KERNEL
    }

    fn get_dma_req(&self) -> Option<&DMAReq> {
        if self.dma_req.valid {
            Some(&self.dma_req)
        } else {
            None
        }
    }

    fn done_dma_req(&mut self) {
        if self.dma_req.valid {
            self.dma_req.done = true;
        }
    }

    fn get_mem_req(&self) -> Option<&MemReq> {
        if self.mem_req.valid {
            Some(&self.mem_req)
        } else {
            None
        }
    }

    fn set_mem_resp(&mut self, data: Option<&Vec<u8>>) {
        self.mem_resp.valid = true;
        if let Some(bytes) = data {
            self.mem_resp.valid = true;
            self.mem_resp.data = bytes.clone();
        }
    }

    fn get_glul_req(&self) -> Option<&GLULInterface> {
        self.gluls.iter().find(|glul| glul.n_tb > 0)
    }
}

impl Clocked for KernelEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        match &self.state {
            KernelEngineState::S0 => {}
            KernelEngineState::S1 => {
                if self.dma_req.valid {
                    if self.dma_req.done {
                        self.dma_req.valid = false;
                        self.state = KernelEngineState::S2;
                    }
                } else {
                    self.dma_req.valid = true;
                    self.dma_req.dir = DMADir::H2D;
                    self.dma_req.src_addr = self.cmd.host_addr;
                    self.dma_req.target_addr = self.cmd.gpu_addr;
                    self.dma_req.sz = self.cmd.sz;
                    println!("queues {:?}", self.dma_req);
                }
            }

            KernelEngineState::S2 => {
                if self.mem_req.valid {
                    if self.mem_resp.valid {
                        self.mem_req.valid = false;
                        self.mem_resp.valid = false;
                        self.state = KernelEngineState::S3;
                    }
                } else {
                    self.mem_req.valid = true;
                    self.mem_req.write = false;
                    self.mem_req.addr = self.cmd.gpu_addr;
                    self.mem_req.bytes = size_of::<KernelPayload>() as u32;
                    println!("Queued mem {:?}", self.mem_req);
                }
            }

            KernelEngineState::S3 => {
                let kernel_payload = KernelPayload::from_bytes(&self.mem_resp.data);
                println!("Kernel payload {:?}", kernel_payload);
                self.tb_ctr = 0;
                self.total_tb = kernel_payload.grid.0 as u32
                    * kernel_payload.grid.1 as u32
                    * kernel_payload.grid.2 as u32;
                let tb_size = kernel_payload.block.0 as u32
                    * kernel_payload.block.1 as u32
                    * kernel_payload.block.2 as u32;
                let available_tbs = self.total_tb - self.tb_ctr;
                if let Some((glul_if_idx, n_tb)) = self
                    .gluls
                    .iter_mut()
                    .enumerate()
                    .map(|(idx, glul)| {
                        (
                            idx,
                            glul.available_threads
                                / (tb_size as usize).min(
                                    glul.config.regs_per_core * glul.config.num_cores
                                        / (kernel_payload.regs_per_thread as usize
                                            * glul.config.num_lanes)
                                            .min(
                                                glul.config.shmem
                                                    / kernel_payload.shmem_per_block as usize,
                                            ),
                                ),
                        )
                    })
                    .filter(|(_, value)| *value > 0)
                    .min_by_key(|(_, value)| *value)
                {
                    let glul_if = &mut self.gluls[glul_if_idx];
                    glul_if.n_tb = (n_tb as u32).min(available_tbs);
                    glul_if.thread_block = ThreadBlock {
                        id: self.tb_ctr,
                        pc: kernel_payload.entry_pc,
                        dim: kernel_payload.block,
                        regs: kernel_payload.regs_per_thread as u32,
                        shmem: kernel_payload.shmem_per_block as u32,
                    };

                    self.tb_ctr += n_tb as u32;
                }

                if self.done_tb == self.total_tb {
                    self.state = KernelEngineState::S4;
                }
            }

            KernelEngineState::S4 => {}

            KernelEngineState::S5 => {}
        };

        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
