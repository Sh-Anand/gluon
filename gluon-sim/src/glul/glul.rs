use std::sync::Arc;

use cyclotron::{cluster::Cluster, sim::{log::Logger, top::ClusterConfig}};
use serde::Deserialize;

use crate::common::base::{Clocked, Configurable, SimErr, ThreadBlock};

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct GLULConfig {
    pub id: usize,
    pub num_cores: usize,
    pub num_warps: usize,
    pub num_lanes: usize,
    pub regs_per_core: usize,
    pub shmem: usize,
}

impl Default for GLULConfig {
    fn default() -> Self {
        GLULConfig {
            id: 0,
            num_cores: 4,
            num_warps: 4,
            num_lanes: 16,
            regs_per_core: 256,
            shmem: 4096,
        }
    }
}

impl GLULConfig {
    pub fn default_id(id: usize) -> Self {
        GLULConfig {
            id,
            ..Default::default()
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct GLULInterface {
    pub config: GLULConfig,
    pub available_threads: usize,
    pub thread_block: ThreadBlock,
    pub n_tb: u32,
}

impl Default for GLULInterface {
    fn default() -> Self {
        let config = GLULConfig::default();
        GLULInterface {
            config: config,
            available_threads: config.num_lanes * config.num_warps * config.num_cores,
            thread_block: ThreadBlock::default(),
            n_tb: 0,
        }
    }
}

impl Configurable<GLULConfig> for GLULInterface {
    fn new(config: GLULConfig) -> Self {
        GLULInterface {
            config,
            available_threads: config.num_lanes * config.num_warps * config.num_cores,
            thread_block: ThreadBlock::default(),
            n_tb: 0,
        }
    }
}

pub struct GLUL {
    num_free_cores: usize,
    config: GLULConfig,
    logger: Arc<Logger>,
}

impl Configurable<GLULConfig> for GLUL {
    fn new(config: GLULConfig) -> Self {
        GLUL {
            num_free_cores: config.num_cores,
            config,
            logger: Arc::new(Logger::new(0)),
        }
    }
}

impl Clocked for GLUL {
    fn tick(&mut self) -> Result<(), SimErr> {
        Ok(())
    }

    fn busy(&mut self) -> bool {
        self.num_free_cores == 0
    }
}

impl GLUL {
    pub fn new_with_logger(config: GLULConfig, logger: Arc<Logger>) -> Self {
        GLUL {
            num_free_cores: config.num_cores,
            config,
            logger,
        }
    }

    pub fn submit_thread_block(&mut self, thread_block: ThreadBlock) {}
}
