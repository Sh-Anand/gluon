use crate::common::base::{Command, Configurable};
use crate::common::queue::Queue;
use serde::Deserialize;

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct IntakeConfig {
    command_queue_size: usize,
}

impl Default for IntakeConfig {
    fn default() -> Self {
        IntakeConfig {
            command_queue_size: 4,
        }
    }
}

#[derive(Default)]
pub struct Intake {
    pub command_queue: Queue<Command>,
}

impl Configurable<IntakeConfig> for Intake {
    fn new(config: &IntakeConfig) -> Self {
        Intake {
            command_queue: Queue::new(config.command_queue_size),
        }
    }
}
