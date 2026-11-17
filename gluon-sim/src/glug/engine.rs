use std::iter::repeat_with;

use crate::common::base::{Clocked, CmdType, Command, Configurable, DMAReq, Event, MemReq, SimErr};
use crate::glug::engines::{
    cs_engine::{CSEngine, CSEngineConfig},
    kernel_engine::{KernelEngine, KernelEngineConfig},
    mem_engine::{MemEngine, MemEngineConfig},
};
use cyclotron::muon::sm::SM;
use cyclotron::sim::log::Logger;
use std::sync::Arc;
use serde::Deserialize;

pub trait Engine: Clocked + Send {
    fn set_cmd(&mut self, cmd: EngineCommand);
    fn busy(&self) -> bool;
    // whether it can take a command now; an engine that serves several at once can while busy
    fn idle(&self) -> bool {
        !self.busy()
    }
    fn cmd_type(&self) -> CmdType;
    fn set_logger(&mut self, logger: Arc<Logger>);
    fn get_dma_req(&self) -> Option<&DMAReq>;
    fn done_dma_req(&mut self);
    fn get_mem_req(&self) -> Option<&MemReq>;
    fn set_mem_resp(&mut self, data: Option<&Vec<u8>>);
    // retire finished CTAs and launch new ones onto SMs, in engine order
    fn launch(&mut self, _sms: &mut [SM]) -> Result<(), SimErr> {
        Ok(())
    }
    // a write to the kernel table: (entry, field, value)
    fn table_write(&self) -> Option<(u8, u8, u32)> {
        None
    }
    fn get_completion(&self) -> Option<Event>;
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct EngineConfig {
    pub num_mem_engines: usize,
    pub num_cs_engines: usize,
    pub kernel_engine_config: KernelEngineConfig,
    pub mem_engine_config: MemEngineConfig,
    pub cs_engine_config: CSEngineConfig,
}

impl Default for EngineConfig {
    fn default() -> Self {
        EngineConfig {
            num_mem_engines: 1,
            num_cs_engines: 1,
            kernel_engine_config: KernelEngineConfig::default(),
            mem_engine_config: MemEngineConfig::default(),
            cs_engine_config: CSEngineConfig::default(),
        }
    }
}

impl EngineConfig {
    pub fn num_engines(&self) -> usize {
        1 + self.num_mem_engines + self.num_cs_engines
    }

    pub fn generate_engines(&self, logger: Arc<Logger>) -> Vec<Box<dyn Engine>> {
        let kernel_engine = Box::new(KernelEngine::new(&self.kernel_engine_config)) as Box<dyn Engine>;
        let mut engines: Vec<Box<dyn Engine>> = std::iter::once(kernel_engine)
        .chain(
            repeat_with(|| Box::new(MemEngine::new(&self.mem_engine_config)) as Box<dyn Engine>)
                .take(self.num_mem_engines),
        )
        .chain(
            repeat_with(|| Box::new(CSEngine::new(&self.cs_engine_config)) as Box<dyn Engine>)
                .take(self.num_cs_engines),
        )
        .collect();

        engines.iter_mut().for_each(|engine| {
            engine.set_logger(logger.clone());
        });

        engines
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct EngineCommand {
    sid: u8,
    cmd_id: u64,
    bytes: [u8; 22],
}

impl EngineCommand {
    pub fn from_command(cmd: Command, cmd_id: u64) -> Self {
        let mut bytes = [0u8; 22];
        bytes.copy_from_slice(cmd.slice(2, 24));
        EngineCommand {
            sid: cmd.sid(),
            cmd_id,
            bytes,
        }
    }

    pub fn sid(&self) -> u8 {
        self.sid
    }

    pub fn cmd_id(&self) -> u64 {
        self.cmd_id
    }

    pub fn bytes(&self) -> &[u8; 22] {
        &self.bytes
    }
}
