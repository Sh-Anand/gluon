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

#[derive(Debug, Default, Clone)]
pub struct StreamQueue {
    pub q: Queue<Command>,
    pub in_flight: bool,
    pub cmd_id: u64,
}

impl StreamQueue {
    pub fn new(cap: usize) -> Self {
        StreamQueue { q: Queue::new(cap), in_flight: false, cmd_id: 0 }
    }
}

pub struct Stream {
    pub sq: Vec<StreamQueue>,
}


impl Configurable<StreamConfig> for Stream {
    fn new(config: &StreamConfig) -> Self {
        Stream { sq: (0..config.num_sq).map(|i| StreamQueue::new(config.sq_entries[i])).collect()}
    }
}

impl Stream {
    pub fn can_enqueue(&self, sid: u8) -> bool {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        !self.sq[sid as usize].q.full()
    }

    pub fn enqueue(&mut self, sid: u8, cmd: Command) {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        self.sq[sid as usize].q.push(cmd);
    }

    pub fn try_pop(&mut self, sid: u8) -> Option<Command> {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        if self.sq[sid as usize].in_flight {
            None
        } else {
            self.sq[sid as usize].in_flight = true;
            self.sq[sid as usize].q.pop()
        }
    }

    pub fn clear_in_flight(&mut self, sid: u8) {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        self.sq[sid as usize].in_flight = false;
        self.sq[sid as usize].cmd_id += 1;
    }
}