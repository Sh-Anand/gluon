use crate::{
    common::base::{Clocked, CmdType, Configurable, SimErr},
    glug::engine::{Engine, EngineCommand},
    glul::glul::GLULStatus,
};
use serde::Deserialize;

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct CSEngineConfig {}

pub struct CSEngine {}

impl Engine for CSEngine {
    fn set_cmd(&mut self, cmd: EngineCommand) {}

    fn busy(&self) -> bool {
        false
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        CmdType::MEM
    }

    fn get_dma_req(&self) -> Option<&crate::common::base::DMAReq> {
        None
    }

    fn done_dma_req(&mut self) {}

    fn get_mem_req(&self) -> Option<&crate::common::base::MemReq> {
        None
    }

    fn set_mem_resp(&mut self, _: Option<&Vec<u8>>) {}

    fn get_glul_req(&self) -> Option<&crate::glul::glul::GLULReq> {
        None
    }

    fn clear_glul_req(&mut self) {}

    fn notify_glul_done(&mut self, _: u32) {}

    fn set_gluls(&mut self, _: Vec<GLULStatus>) {}
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
