use crate::common::{
    base::{Configurable, Event},
    queue::Queue,
};
use serde::Deserialize;

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct CompletionConfig {
    event_queue_size: usize,
}

impl Default for CompletionConfig {
    fn default() -> Self {
        CompletionConfig {
            event_queue_size: 4,
        }
    }
}
pub struct Completion {
    pub eq: Queue<Event>,
}

impl Configurable<CompletionConfig> for Completion {
    fn new(config: CompletionConfig) -> Self {
        Completion {
            eq: Queue::new(config.event_queue_size),
        }
    }
}
