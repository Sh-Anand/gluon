use crate::{
    common::base::{Clocked, Configurable},
    glug::engine::Engine,
};

#[derive(Debug, Default, Clone, Copy)]
pub struct MemEngineConfig {}

pub struct MemEngine {}

impl Engine for MemEngine {
    fn init(&mut self, cmd: crate::common::base::EngineCommand) {}

    fn busy(&self) -> bool {
        false
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        crate::common::base::CmdType::MEM
    }
}

impl Configurable<MemEngineConfig> for MemEngine {
    fn instantiate(config: MemEngineConfig) -> Self {
        MemEngine {}
    }
}

impl Clocked for MemEngine {
    fn tick(&mut self) {}

    fn busy(&mut self) -> bool {
        false
    }
}
