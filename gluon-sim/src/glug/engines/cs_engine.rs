use crate::{
    common::base::{Clocked, CmdType, Configurable, Event, SimErr},
    glug::engine::{Engine, EngineCommand},
};
use cyclotron::sim::log::Logger;
use std::sync::Arc;
use serde::Deserialize;

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct CSEngineConfig {}

/// A kernel table entry.  A CSR command sets one of its fields.
#[derive(Debug, Default, Clone, Copy)]
pub struct KernelEntry {
    pub pc: u32,
    pub regs: u8, // per thread
}

impl KernelEntry {
    pub fn set(&mut self, field: u8, value: u32) {
        match field {
            0 => self.pc = value,
            1 => self.regs = value as u8,
            _ => panic!("kernel table has no field {field}"),
        }
    }
}

pub struct CSEngine {
    cmd: Option<EngineCommand>,
    done: bool, // its write was handed over; the completion is out this tick
    logger: Arc<Logger>,
}

impl Engine for CSEngine {
    fn set_cmd(&mut self, cmd: EngineCommand) {
        self.cmd = Some(cmd);
    }

    fn busy(&self) -> bool {
        self.cmd.is_some()
    }

    // (entry, field, value), read once: the tick after it is handed over the command is done
    fn table_write(&self) -> Option<(u8, u8, u32)> {
        let b = self.cmd.filter(|_| !self.done)?.bytes().to_owned();
        Some((b[0], b[1], u32::from_le_bytes(b[2..6].try_into().unwrap())))
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

    fn get_completion(&self) -> Option<Event> {
        self.cmd.filter(|_| self.done).map(|cmd| Event::from_ok(cmd.sid()))
    }
}

impl Configurable<CSEngineConfig> for CSEngine {
    fn new(_config: &CSEngineConfig) -> Self {
        CSEngine {
            cmd: None,
            done: false,
            logger: Arc::new(Logger::new(0)),
        }
    }
}

impl Clocked for CSEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        if self.done {
            self.cmd = None;
        }
        self.done = self.cmd.is_some();
        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
