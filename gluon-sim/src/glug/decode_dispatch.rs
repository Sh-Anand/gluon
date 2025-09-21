use crate::common::{
    base::{CmdType, Command, Configurable, EngineCommand},
    queue::Queue,
};
use serde::Deserialize;

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct DecodeDispatchConfig {
    pub kq_size: usize,
    pub mq_size: usize,
    pub csq_size: usize,
}

impl Default for DecodeDispatchConfig {
    fn default() -> Self {
        Self {
            kq_size: 4,
            mq_size: 4,
            csq_size: 4,
        }
    }
}

#[derive(Debug, Default, Clone)]
pub struct EngineQueue {
    pub q: Queue<EngineCommand>,
    pub engine_type: CmdType,
}

pub struct DecodeDispatch {
    pub qs: [EngineQueue; 3],
}

impl Configurable<DecodeDispatchConfig> for DecodeDispatch {
    fn new(config: DecodeDispatchConfig) -> Self {
        DecodeDispatch {
            qs: [
                EngineQueue {
                    q: Queue::new(config.kq_size),
                    engine_type: CmdType::KERNEL,
                },
                EngineQueue {
                    q: Queue::new(config.mq_size),
                    engine_type: CmdType::MEM,
                },
                EngineQueue {
                    q: Queue::new(config.csq_size),
                    engine_type: CmdType::CSR,
                },
            ],
        }
    }
}

impl DecodeDispatch {
    pub fn can_enqueue(&self, cmd_type: CmdType) -> bool {
        self.qs
            .iter()
            .filter(|eq| eq.engine_type == cmd_type)
            .any(|eq| !eq.q.full())
    }

    pub fn enqueue(&mut self, cmd: Command) {
        if let Some(engine_queue) = self
            .qs
            .iter_mut()
            .find(|eq| eq.engine_type == cmd.cmd_type() && !eq.q.full())
        {
            engine_queue.q.push(cmd.get_engine_cmd());
        }
    }
}
