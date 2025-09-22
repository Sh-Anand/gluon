use crate::{
    common::base::{Clocked, CmdType, Configurable, SimErr},
    glug::engine::Engine,
};
use serde::Deserialize;

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct MemEngineConfig {}

pub struct MemEngine {}

impl Engine for MemEngine {
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

impl Configurable<MemEngineConfig> for MemEngine {
    fn new(_config: MemEngineConfig) -> Self {
        MemEngine {}
    }
}

impl Clocked for MemEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
