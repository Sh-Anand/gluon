use std::iter::repeat_with;

use crate::common::base::{Clocked, CmdType, Command, Configurable, DMAReq, MemReq};
use crate::glug::engines::{
    cs_engine::{CSEngine, CSEngineConfig},
    kernel_engine::{KernelEngine, KernelEngineConfig},
    mem_engine::{MemEngine, MemEngineConfig},
};
use crate::glul::glul::GLULInterface;
use serde::Deserialize;

pub trait Engine: Clocked + Send {
    fn init(&mut self, cmd: EngineCommand);
    fn busy(&self) -> bool;
    fn cmd_type(&self) -> CmdType;
    fn get_dma_req(&self) -> Option<&DMAReq>;
    fn done_dma_req(&mut self);
    fn get_mem_req(&self) -> Option<&MemReq>;
    fn set_mem_resp(&mut self, data: Option<&Vec<u8>>);
    fn get_glul_req(&self) -> Option<&GLULInterface>;
    fn clear_glul_req(&mut self, id: usize);
}

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct EngineConfig {
    pub num_kernel_engines: usize,
    pub num_mem_engines: usize,
    pub num_cs_engines: usize,
    pub kernel_engine_config: KernelEngineConfig,
    pub mem_engine_config: MemEngineConfig,
    pub cs_engine_config: CSEngineConfig,
}

impl Default for EngineConfig {
    fn default() -> Self {
        EngineConfig {
            num_kernel_engines: 1,
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
        self.num_kernel_engines + self.num_mem_engines + self.num_cs_engines
    }

    pub fn generate_engines(&self) -> Vec<Box<dyn Engine>> {
        repeat_with(|| {
            Box::new(KernelEngine::new(self.kernel_engine_config.clone())) as Box<dyn Engine>
        })
        .take(self.num_kernel_engines)
        .chain(
            repeat_with(|| Box::new(MemEngine::new(self.mem_engine_config)) as Box<dyn Engine>)
                .take(self.num_mem_engines),
        )
        .chain(
            repeat_with(|| Box::new(CSEngine::new(self.cs_engine_config)) as Box<dyn Engine>)
                .take(self.num_cs_engines),
        )
        .collect()
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct EngineCommand {
    id: u8,
    bytes: [u8; 14],
}

impl EngineCommand {
    pub fn from_command(cmd: Command) -> Self {
        let mut bytes = [0u8; 14];
        bytes.copy_from_slice(cmd.slice(2, 16));
        EngineCommand {
            id: cmd.id(),
            bytes,
        }
    }

    pub fn id(&self) -> u8 {
        self.id
    }

    pub fn payload(&self) -> &[u8; 14] {
        &self.bytes
    }
}
