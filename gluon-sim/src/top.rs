use crate::common::base::{Clocked, Command, Configurable, SimErr};
use crate::glug::glug::{GLUGConfig, GLUG};
use serde::Deserialize;

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct SimConfig {
    pub timeout_cycles: u64,
}

impl Default for SimConfig {
    fn default() -> Self {
        Self { timeout_cycles: 0 }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct TopConfig {
    pub sim: SimConfig,
    pub glug: GLUGConfig,
}

impl Default for TopConfig {
    fn default() -> Self {
        Self {
            sim: SimConfig::default(),
            glug: GLUGConfig::default(),
        }
    }
}

pub struct Top {
    glug: GLUG,
    cycles_elapsed: u64,
    cycles_timeout: u64,
}

impl Top {
    pub fn new(config: TopConfig) -> Self {
        Top {
            glug: GLUG::new(config.glug),
            cycles_elapsed: 0,
            cycles_timeout: config.sim.timeout_cycles,
        }
    }

    pub fn submit_command(&mut self, command: Command) {
        self.glug.submit_command(command);
    }

    pub fn cycles_elapsed(&self) -> u64 {
        self.cycles_elapsed
    }
}

impl Clocked for Top {
    fn tick(&mut self) -> Result<(), SimErr> {
        self.glug.tick()?;
        self.cycles_elapsed = self.cycles_elapsed.saturating_add(1);

        if self.cycles_timeout != 0 && self.cycles_elapsed >= self.cycles_timeout {
            Err(SimErr::TIMEOUT)
        } else {
            Ok(())
        }
    }

    fn busy(&mut self) -> bool {
        self.glug.busy()
    }
}

impl Configurable<TopConfig> for Top {
    fn new(config: TopConfig) -> Self {
        Top::new(config)
    }
}
