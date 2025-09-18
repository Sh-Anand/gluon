use crate::common::base::{Command, Configurable};
use crate::common::queue::Queue;

#[derive(Debug, Clone, Copy)]
pub struct FrontendConfig {
    command_queue_size: usize,
}

impl Default for FrontendConfig {
    fn default() -> Self {
        FrontendConfig { command_queue_size: 4 }
    }
}

#[derive(Default)]
pub struct Frontend {
    command_queue: Queue<Command>,
}

impl Configurable<FrontendConfig> for Frontend {
    fn instantiate(config: FrontendConfig) -> Self {
        Frontend { command_queue: Queue::new(config.command_queue_size) }
    }
}