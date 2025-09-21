use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::DMADir;
use crate::common::base::DMAReq;
use crate::common::base::EngineCommand;
use crate::common::base::KernelCommand;
use crate::common::base::MemReq;
use crate::common::base::MemResp;
use crate::common::base::SimErr;
use crate::glug::engine::Engine;
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

pub struct KernelEngine {
    cmd: KernelCommand,

    dma_req_valid: bool,
    dma_req: DMAReq,
    dma_req_done: bool,

    mem_req_valid: bool,
    mem_req: MemReq,
    mem_resp_valid: bool,
    mem_resp: MemResp,

    state: KernelEngineState,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn new(_config: KernelEngineConfig) -> Self {
        KernelEngine {
            cmd: KernelCommand::default(),
            dma_req_valid: false,
            dma_req: DMAReq::default(),
            dma_req_done: false,
            mem_req_valid: false,
            mem_req: MemReq::default(),
            mem_resp_valid: false,
            mem_resp: MemResp::default(),
            state: KernelEngineState::S0,
        }
    }
}
impl Engine for KernelEngine {
    fn init(&mut self, cmd: EngineCommand) {
        self.cmd = cmd.to_kernel_cmd();
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

    fn get_dma_req(&self) -> Option<DMAReq> {
        if self.dma_req_valid {
            Some(self.dma_req)
        } else {
            None
        }
    }

    fn done_dma_req(&mut self) {
        if self.dma_req_valid {
            self.dma_req_done;
        }
    }
}

impl Clocked for KernelEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        match &self.state {
            KernelEngineState::S0 => {
                if self.dma_req_valid {
                    if self.dma_req_done {
                        self.dma_req_valid = false;
                        self.state = KernelEngineState::S2;
                    }
                } else {
                    self.dma_req_valid = true;
                    self.dma_req = DMAReq {
                        dir: DMADir::H2D,
                        src_addr: self.cmd.host_addr,
                        target_addr: self.cmd.gpu_addr,
                        sz: self.cmd.sz,
                    };
                }
            }

            KernelEngineState::S1 => {}

            KernelEngineState::S2 => {}

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
