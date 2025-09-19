use std::iter::repeat_with;

use crate::common::base::{Clocked, CmdType, Configurable, EngineCommand};
use crate::glug::engines::{
    cs_engine::{CSEngine, CSEngineConfig},
    kernel_engine::{KernelEngine, KernelEngineConfig},
    mem_engine::{MemEngine, MemEngineConfig},
};

pub trait Engine: Clocked + Send {
    fn init(&mut self, cmd: EngineCommand);
    fn busy(&self) -> bool;
    fn cmd_type(&self) -> CmdType;
}

#[derive(Debug, Clone, Copy)]
pub struct EngineConfig {
    num_kernel_engines: usize,
    num_mem_engines: usize,
    num_cs_engines: usize,
    kernel_engine_config: KernelEngineConfig,
    mem_engine_config: MemEngineConfig,
    cs_engine_config: CSEngineConfig,
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
            Box::new(KernelEngine::instantiate(self.kernel_engine_config)) as Box<dyn Engine>
        })
        .take(self.num_kernel_engines)
        .chain(
            repeat_with(|| {
                Box::new(MemEngine::instantiate(self.mem_engine_config)) as Box<dyn Engine>
            })
            .take(self.num_mem_engines),
        )
        .chain(
            repeat_with(|| {
                Box::new(CSEngine::instantiate(self.cs_engine_config)) as Box<dyn Engine>
            })
            .take(self.num_cs_engines),
        )
        .collect()
    }
}
