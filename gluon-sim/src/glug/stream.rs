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
    pub sqs: Vec<StreamQueue>,
}


impl Configurable<StreamConfig> for Stream {
    fn new(config: &StreamConfig) -> Self {
        Stream { sqs: (0..config.num_sq).map(|i| StreamQueue::new(config.sq_entries[i])).collect()}
    }
}

impl Stream {
    pub fn can_enqueue(&self, sid: u8) -> bool {
        assert!(sid < self.sqs.len() as u8, "sid out of bounds");
        !self.sqs[sid as usize].q.full()
    }

    pub fn enqueue(&mut self, sid: u8, cmd: Command) {
        assert!(sid < self.sqs.len() as u8, "sid out of bounds");
        if cmd.is_wait() {
            let (w_sid, w_cmd_id) = cmd.get_wait_ids();
            if self.sqs[w_sid as usize].cmd_id >= w_cmd_id {
                return;
            }
        }
        self.sqs[sid as usize].q.push(cmd);
    }

    pub fn try_pop(&mut self, sid: u8) -> Option<Command> {
        assert!(sid < self.sqs.len() as u8, "sid out of bounds");
        let idx = sid as usize;
        if self.sqs[idx].in_flight || self.sqs[idx].q.empty() || self.sqs[idx].q.peek().expect("impossible").is_wait() {
            None
        } else {
            self.sqs[idx].in_flight = true;
            self.sqs[idx].q.pop()
        }
    }

    pub fn clear_in_flight(&mut self, sid: u8) {
        assert!(sid < self.sqs.len() as u8, "sid out of bounds");
        let idx = sid as usize;
        self.sqs[idx].in_flight = false;
        self.sqs[idx].cmd_id += 1;
        let cmd_id = self.sqs[idx].cmd_id;
        self.sqs.iter_mut().enumerate()
        .filter(|(s_idx, _)| *s_idx != idx)
        .for_each(|(_, sq)| {
            if !sq.q.empty() && sq.q.peek().expect("impossible").is_wait() {
                let (w_sid, w_cmd_id) = sq.q.peek().expect("impossible").get_wait_ids();
                if w_sid == sid && cmd_id >= w_cmd_id {
                    sq.q.pop();
                } 
            }
        })
    }
}