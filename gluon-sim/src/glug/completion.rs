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
    pub eq: Queue<(Event, bool)>,
}

impl Configurable<CompletionConfig> for Completion {
    fn new(config: CompletionConfig) -> Self {
        Completion {
            eq: Queue::new(config.event_queue_size),
        }
    }
}

impl Completion {
    // enqueue and return idx
    pub fn allocate(&mut self) -> usize {
        self.eq.push((Event::default(), false));
        self.eq.len() - 1
    }

    pub fn set(&mut self, idx: usize, event: Event) {
        let (evnt, done) = self.eq.get_mut(idx).expect("completion idx out of bounds");
        *evnt = event;
        *done = true;
    }

    pub fn pop_completion(&mut self) -> Option<Event> {
        if let Some((_, true)) = self.eq.peek() {
            return self.eq.pop().map(|(event, _)| event);
        }
        None
    }
}
