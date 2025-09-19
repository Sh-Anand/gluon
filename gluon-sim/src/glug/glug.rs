use crate::common::base::{Clocked, CmdType, Command, Configurable};
use crate::glug::completion::{Completion, CompletionConfig};
use crate::glug::decode_dispatch::{DecodeDispatch, DecodeDispatchConfig};
use crate::glug::engine::{Engine, EngineConfig};
use crate::glug::frontend::{Frontend, FrontendConfig};

#[derive(Debug, Default, Clone, Copy)]
pub struct GLUGConfig {
    frontend_config: FrontendConfig,
    decode_dispatch_config: DecodeDispatchConfig,
    engine_config: EngineConfig,
    completion_config: CompletionConfig,
}

pub struct GLUG {
    cmd: Command,
    frontend: Frontend,
    decode_dispatch: DecodeDispatch,
    engines: Vec<Box<dyn Engine>>,
    completion: Completion,
}

impl GLUG {
    pub fn submit_command(&mut self, command: Command) {
        self.cmd = command;
    }
}

impl Configurable<GLUGConfig> for GLUG {
    fn instantiate(config: GLUGConfig) -> Self {
        GLUG {
            cmd: Command::default(),
            frontend: Frontend::instantiate(config.frontend_config),
            decode_dispatch: DecodeDispatch::instantiate(config.decode_dispatch_config),
            engines: config.engine_config.generate_engines(),
            completion: Completion::instantiate(config.completion_config),
        }
    }
}

impl Clocked for GLUG {
    fn tick(&mut self) {
        // TODO: Tick completion

        // Tick engines
        self.engines.iter_mut().for_each(|engine| engine.tick());

        // Tick decode
        self.decode_dispatch
            .qs
            .iter_mut()
            .map(|eq| {
                (
                    eq.q.pop(),
                    self.engines
                        .iter_mut()
                        .enumerate()
                        .find(|(_, engine)| engine.cmd_type() == eq.engine_type && !engine.busy())
                        .map(|(idx, _)| idx),
                )
            })
            .collect::<Vec<_>>()
            .iter()
            .for_each(|x| {
                if let (Some(engine_cmd), Some(engine_idx)) = x {
                    self.engines
                        .get_mut(*engine_idx)
                        .expect("Engine idx must exist!")
                        .init(*engine_cmd);
                }
            });

        // Tick frontend
        if self.frontend.command_queue.push(self.cmd) {
            self.cmd = Command::default();
        }

        if let Some(frontend_out_cmd) = self
            .frontend
            .command_queue
            .peek()
            .map(|cmd| match cmd.cmd_type() {
                CmdType::FENCE => self.completion.eq.empty(),
                cmd_type => self.decode_dispatch.can_enqueue(cmd_type),
            })
            .unwrap_or(false)
            .then(|| {
                self.frontend
                    .command_queue
                    .pop()
                    .expect("Cannot be empty here")
            })
        {
            self.decode_dispatch.enqueue(frontend_out_cmd);
            // TODO create completion
        }
    }

    fn busy(&mut self) -> bool {
        self.frontend.command_queue.full()
    }
}
