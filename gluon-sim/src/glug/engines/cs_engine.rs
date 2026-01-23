use crate::{
    common::base::{Clocked, CmdType, Configurable, Event, SimErr},
    glug::engine::{Engine, EngineCommand},
    glul::glul::GLULStatus,
};
use cyclotron::muon::warp::ExecErr;
use cyclotron::sim::log::Logger;
use std::sync::Arc;
use serde::Deserialize;

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct CSEngineConfig {}

pub struct CSEngine {
    logger: Arc<Logger>,
}

impl Engine for CSEngine {
    fn set_cmd(&mut self, _: EngineCommand) {}

    fn busy(&self) -> bool {
        false
    }

    fn cmd_type(&self) -> crate::common::base::CmdType {
        CmdType::CSR
    }

    fn set_logger(&mut self, logger: Arc<Logger>) {
        self.logger = logger;
    }

    fn get_dma_req(&self) -> Option<&crate::common::base::DMAReq> {
        None
    }

    fn done_dma_req(&mut self) {}

    fn get_mem_req(&self) -> Option<&crate::common::base::MemReq> {
        None
    }

    fn set_mem_resp(&mut self, _: Option<&Vec<u8>>) {
        panic!("CSR engine: cannot set mem resp");
    }

    fn get_glul_req(&self) -> Option<&crate::glul::glul::GLULReq> {
        None
    }

    fn clear_glul_req(&mut self) {
        panic!("CSR engine: cannot clear glul req");
    }

    fn notify_glul_done(&mut self, _: u32) {
        panic!("CSR engine: cannot notify glul done");
    }

    fn set_gluls(&mut self, _: Vec<GLULStatus>) {}

    fn notify_glul_err(&mut self, _: ExecErr) {
        panic!("CSR engine: cannot notify glul err");
    }

    fn get_completion(&self) -> Option<Event> {
        None
    }
}

impl Configurable<CSEngineConfig> for CSEngine {
    fn new(_config: &CSEngineConfig) -> Self {
        CSEngine {
            logger: Arc::new(Logger::new(0)),
        }
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
