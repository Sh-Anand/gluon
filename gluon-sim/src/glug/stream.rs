use serde::Deserialize;

use crate::common::{base::{Command, Configurable}, queue::Queue};

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct StreamConfig {
    pub num_sq: usize,
    pub sq_entries: Vec<usize>,
}

impl Default for StreamConfig {
    fn default() -> Self {
        Self {
            num_sq: 4,
            sq_entries: vec![8; 4],
        }
    }
}

pub struct Stream {
    pub sq: Vec<Queue<Command>>,
}


impl Configurable<StreamConfig> for Stream {
    fn new(config: StreamConfig) -> Self {
        Stream { sq: (0..config.num_sq).map(|i| Queue::new(config.sq_entries[i])).collect() }
    }
}

impl Stream {
    pub fn can_enqueue(&self, sq_idx: usize) -> bool {
        !self.sq[sq_idx].full()
    }

    pub fn enqueue(&mut self, sq_idx: usize, cmd: Command) {
        self.sq[sq_idx].push(cmd);
    }
}