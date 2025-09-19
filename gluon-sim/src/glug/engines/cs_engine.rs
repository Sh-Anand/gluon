use crate::{
    common::base::{Clocked, Configurable},
    glug::engine::Engine,
};

#[derive(Debug, Default, Clone, Copy)]
pub struct CSEngineConfig {}

pub struct CSEngine {}

impl Engine for CSEngine {
    fn init(&mut self, cmd: crate::common::base::EngineCommand) {}

    fn busy(&self) -> bool {
        false
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        crate::common::base::CmdType::CSR
    }
}

impl Configurable<CSEngineConfig> for CSEngine {
    fn instantiate(config: CSEngineConfig) -> Self {
        CSEngine {}
    }
}

impl Clocked for CSEngine {
    fn tick(&mut self) {}

    fn busy(&mut self) -> bool {
        false
    }
}
