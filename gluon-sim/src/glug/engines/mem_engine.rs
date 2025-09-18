use crate::{common::base::{Clocked, Configurable}, glug::engine::Engine};

#[derive(Debug, Default, Clone, Copy)]
pub struct MemEngineConfig {
}

pub struct MemEngine {

}

impl Engine for MemEngine {
    fn init(&mut self, cmd: crate::common::base::EngineCommand) {
        
    }

    fn busy(&self) -> bool {
        false
    }
}

impl Configurable<MemEngineConfig> for MemEngine {
    fn instantiate(config: MemEngineConfig) -> Self {
        MemEngine {  }
    }
}

impl Clocked for MemEngine {
    fn tick(&mut self) {
        
    }

    fn reset(&mut self) {
        
    }
}