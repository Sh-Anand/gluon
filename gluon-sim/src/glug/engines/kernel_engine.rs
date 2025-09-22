use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::glug::engine::Engine;
use crate::glug::engine::EngineCommand;
use serde::Deserialize;

pub enum KernelEngineState {
    S0,
    S1,
    S2,
    S3,
    S4,
    S5,
}

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct KernelEngineConfig {
    // TODO: future configs
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

pub struct KernelEngine {
    cmd: KernelCommand,

    dma_req: DMAReq,

    mem_req: MemReq,
    mem_resp: MemResp,

    state: KernelEngineState,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn new(_config: KernelEngineConfig) -> Self {
        KernelEngine {
            cmd: KernelCommand::default(),
            dma_req: DMAReq::default(),
            mem_req: MemReq::default(),
            mem_resp: MemResp::default(),
            state: KernelEngineState::S0,
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
        )
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

    fn get_mem_req(&self) -> Option<&MemReq> {
        if self.mem_req.valid {
            Some(&self.mem_req)
        } else {
            None
        }
    }

    fn done_dma_req(&mut self) {
        if self.dma_req.valid {
            self.dma_req.done = true;
        }
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
                        self.state = KernelEngineState::S3;
                    } else {
                        self.mem_req.valid = true;
                        self.mem_req.write = false;
                        self.mem_req.addr = self.cmd.gpu_addr;
                        self.mem_req.bytes = size_of::<KernelPayload>() as u32;
                    }
                }
            }

            KernelEngineState::S3 => {}

            KernelEngineState::S4 => {}

            KernelEngineState::S5 => {}
        };

        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
