use crate::{common::base::{Clocked, Configurable}, glug::{completion::{Completion, CompletionConfig}, decode_dispatch::{DecodeDispatch, DecodeDispatchConfig}, engine::{Engine, EngineConfig}, frontend::{Frontend, FrontendConfig}}};

#[derive(Debug, Default, Clone, Copy)]
pub struct GLUGConfig {
    frontend_config: FrontendConfig,
    decode_dispatch_config: DecodeDispatchConfig,
    engine_config: EngineConfig,
    completion_config: CompletionConfig,
}

pub struct GLUG {
    frontend: Frontend,
    decode_dispatch: DecodeDispatch,
    engines: Vec<Box<dyn Engine>>,
    completion: Completion,
}

impl Configurable<GLUGConfig> for GLUG {
    fn instantiate(config: GLUGConfig) -> Self {
        GLUG { frontend: Frontend::instantiate(config.frontend_config), 
               decode_dispatch: DecodeDispatch::instantiate(config.decode_dispatch_config),
               engines: config.engine_config.generate_engines(), 
               completion: Completion::instantiate(config.completion_config) 
            }
    }
}

impl Clocked for GLUG {
    fn tick(&mut self) {
        
    }

    fn reset(&mut self) {
        
    }
}