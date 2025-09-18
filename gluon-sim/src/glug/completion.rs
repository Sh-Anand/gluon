use crate::common::{base::{Configurable, Event}, queue::Queue};

#[derive(Debug, Clone, Copy)]
pub struct CompletionConfig {
    event_queue_size: usize,
}

impl Default for CompletionConfig {
    fn default() -> Self {
        CompletionConfig { event_queue_size: 4 }
    }
}
pub struct Completion {
    eq: Queue<Event>
}

impl Configurable<CompletionConfig> for Completion {
    fn instantiate(config: CompletionConfig) -> Self {
        Completion { eq: Queue::new(config.event_queue_size) }
    }
}