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
    pub sq_in_flight: Vec<bool>,
}


impl Configurable<StreamConfig> for Stream {
    fn new(config: &StreamConfig) -> Self {
        Stream { sq: (0..config.num_sq).map(|i| Queue::new(config.sq_entries[i])).collect(), 
                 sq_in_flight: vec![false; config.num_sq] }
    }
}

impl Stream {
    pub fn can_enqueue(&self, sid: u8) -> bool {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        !self.sq[sid as usize].full()
    }

    pub fn enqueue(&mut self, sid: u8, cmd: Command) {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        self.sq[sid as usize].push(cmd);
    }

    pub fn try_pop(&mut self, sid: u8) -> Option<Command> {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        if self.sq_in_flight[sid as usize] {
            None
        } else {
            self.sq_in_flight[sid as usize] = true;
            self.sq[sid as usize].pop()
        }
    }

    pub fn clear_in_flight(&mut self, sid: u8) {
        assert!(sid < self.sq.len() as u8, "sid out of bounds");
        self.sq_in_flight[sid as usize] = false;
    }
}