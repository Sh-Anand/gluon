use crate::common::{base::{Configurable, EngineCommand}, queue::Queue};

#[derive(Debug, Clone, Copy)]
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

pub struct DecodeDispatch {
    kq: Queue<EngineCommand>,
    mq: Queue<EngineCommand>,
    csq: Queue<EngineCommand>,
}

impl Configurable<DecodeDispatchConfig> for DecodeDispatch {
    fn instantiate(config: DecodeDispatchConfig) -> Self {
        DecodeDispatch { kq: Queue::new(config.kq_size),
                         mq: Queue::new(config.mq_size),
                         csq: Queue::new(config.csq_size) 
                    }
    }
}
