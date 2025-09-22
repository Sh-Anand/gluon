use crate::{
    common::base::{Clocked, CmdType, Configurable, SimErr},
    glug::engine::Engine,
};
use serde::Deserialize;

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct CSEngineConfig {}

pub struct CSEngine {}

impl Engine for CSEngine {
    fn init(&mut self, cmd: crate::glug::engine::EngineCommand) {}

    fn busy(&self) -> bool {
        false
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        CmdType::MEM
    }

    fn get_dma_req(&self) -> Option<&crate::common::base::DMAReq> {
        None
    }

    fn get_mem_req(&self) -> Option<&crate::common::base::MemReq> {
        None
    }

    fn done_dma_req(&mut self) {}
}

impl Configurable<CSEngineConfig> for CSEngine {
    fn new(_config: CSEngineConfig) -> Self {
        CSEngine {}
    }
}

impl Clocked for CSEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
