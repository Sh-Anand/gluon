use crate::common::base::Clocked;
use crate::common::base::Configurable;
use crate::common::base::EngineCommand;
use crate::glug::engine::Engine;

pub enum KernelEngineState {
    S0,
    S1,
    S2,
    S3,
    S4,
    S5,
}

#[derive(Debug, Default, Clone, Copy)]
pub struct KernelEngineConfig {
    // TODO: future configs
}

pub struct KernelEngine {
    state: KernelEngineState,
}

impl Configurable<KernelEngineConfig> for KernelEngine {
    fn instantiate(config: KernelEngineConfig) -> Self {
        KernelEngine {
            state: KernelEngineState::S0,
        }
    }
}
impl Engine for KernelEngine {
    fn init(&mut self, _cmd: EngineCommand) {}

    fn busy(&self) -> bool {
        !matches!(self.state, KernelEngineState::S0)
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        crate::common::base::CmdType::KERNEL
    }
}

impl Clocked for KernelEngine {
    fn tick(&mut self) {}

    fn busy(&mut self) -> bool {
        false
    }
}
