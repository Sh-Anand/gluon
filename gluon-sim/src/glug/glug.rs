use crate::common::base::{Clocked, CmdType, Command, Configurable, DMADir, SimErr};
use crate::glug::completion::{Completion, CompletionConfig};
use crate::glug::decode_dispatch::{DecodeDispatch, DecodeDispatchConfig};
use crate::glug::engine::{Engine, EngineConfig};
use crate::glug::frontend::{Frontend, FrontendConfig};
use crate::glul::glul::{GLULConfig, GLULStatus, GLUL};
use cyclotron::sim::log::Logger;
use cyclotron::sim::toy_mem::ToyMemory;
use serde::Deserialize;
use std::sync::{Arc, RwLock};

#[derive(Debug, Default, Clone, Deserialize)]
#[serde(default)]
pub struct GLUGConfig {
    pub frontend: FrontendConfig,
    #[serde(rename = "decode_dispatch")]
    pub decode_dispatch: DecodeDispatchConfig,
    pub engine: EngineConfig,
    pub completion: CompletionConfig,
    pub gluls: Vec<GLULConfig>,
    pub log_level: u64,
}

pub struct GLUG {
    cmd_valid: bool,
    cmd: Command,

    frontend: Frontend,
    decode_dispatch: DecodeDispatch,
    engines: Vec<Box<dyn Engine>>,
    completion: Completion,

    gluls: Vec<GLUL>,

    dram: Arc<RwLock<ToyMemory>>,

    logger: Arc<Logger>,
}

impl GLUG {
    pub fn submit_command(&mut self, command: Command) {
        self.cmd_valid = true;
        self.cmd = command;
    }
}

impl Configurable<GLUGConfig> for GLUG {
    fn new(config: GLUGConfig) -> Self {
        let glul_configs = config.gluls.clone();
        let engine_config = config.engine.clone();

        let dram = Arc::new(RwLock::new(ToyMemory::default()));
        let logger = Arc::new(Logger::new(config.log_level));

        let gluls = glul_configs
            .iter()
            .copied()
            .map(|config| GLUL::new_with_logger_dram(config, logger.clone(), dram.clone()))
            .collect::<Vec<_>>();

        let mut engines = engine_config.generate_engines();
        engines.iter_mut().for_each(|engine| {
            engine.set_gluls(
                gluls
                    .iter()
                    .map(|glul| glul.get_status().clone())
                    .collect::<Vec<_>>(),
            );
        });

        GLUG {
            cmd: Command::default(),
            cmd_valid: false,
            frontend: Frontend::new(config.frontend),
            decode_dispatch: DecodeDispatch::new(config.decode_dispatch),
            engines,
            completion: Completion::new(config.completion),
            gluls,
            dram,
            logger,
        }
    }
}

impl Clocked for GLUG {
    fn tick(&mut self) -> Result<(), SimErr> {
        // TODO: Tick completion

        // Notify GLUL completions
        self.gluls
            .iter_mut()
            .filter_map(|glul| glul.try_acknowledge_done())
            .for_each(|(engine_idx, tbs)| {
                self.engines
                    .get_mut(engine_idx)
                    .expect("Engine idx out of bounds")
                    .notify_glul_done(tbs);
            });

        // Service GLUL schedules
        self.engines
            .iter_mut()
            .enumerate()
            .for_each(|(idx, engine)| {
                if let Some(glul_req) = engine.get_glul_req() {
                    self.gluls[glul_req.idx].submit_thread_block(
                        glul_req.thread_block,
                        glul_req.n_tb,
                        idx,
                    );
                    engine.clear_glul_req();
                }
            });

        // Tick GLULs
        self.gluls.iter_mut().try_for_each(|glul| glul.tick())?;

        // Service Mem requests
        if let Some(engine) = self
            .engines
            .iter_mut()
            .find(|engine| engine.get_mem_req().is_some())
        {
            let mem_req = engine.get_mem_req().expect("Mem: unreachable");
            let mut dram = self.dram.write().expect("gmem poisoned");
            println!("Served mem {:?}", mem_req);
            if mem_req.write {
                mem_req.data.iter().enumerate().for_each(|(idx, byte)| {
                    dram.write_byte((mem_req.addr + idx as u32) as usize, *byte)
                        .expect("gmem write errored")
                });

                engine.set_mem_resp(None);
            } else {
                let read_data = {
                    (0..mem_req.bytes)
                        .map(|i| {
                            dram.read_byte((mem_req.addr + i) as usize)
                                .expect("gmem read impossible")
                        })
                        .collect::<Vec<u8>>()
                };
                engine.set_mem_resp(Some(&read_data));
                println!("Served mem");
            }
        }

        // Service DMA requests
        if let Some(engine) = self
            .engines
            .iter_mut()
            .find(|engine| engine.get_dma_req().is_some())
        {
            let dma_req = engine.get_dma_req().expect("DMA: unreachable");
            match dma_req.dir {
                DMADir::H2D => {
                    let mut dram = self.dram.write().expect("gmem poisoned");

                    (0..dma_req.sz)
                        .map(|byte| unsafe { *((dma_req.src_addr + byte) as *const u8) })
                        .enumerate()
                        .for_each(|(idx, byte)| {
                            dram.write_byte((dma_req.target_addr + idx as u32) as usize, byte)
                                .expect("gmem write errored")
                        });
                }

                DMADir::D2H => {
                    let data = {
                        let mut dram = self.dram.write().expect("gmem poisoned");
                        (0..dma_req.sz)
                            .map(|i| {
                                dram.read_byte((dma_req.src_addr + i) as usize)
                                    .expect("gmem read impossible")
                            })
                            .collect::<Vec<u8>>()
                    };
                    data.iter().enumerate().for_each(|(idx, byte)| unsafe {
                        *((dma_req.target_addr + idx as u32) as *mut u8) = *byte;
                    });
                }
            };

            engine.done_dma_req();
        }

        // Tick engines
        self.engines
            .iter_mut()
            .try_for_each(|engine| engine.tick())?;

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
                        .set_cmd(*engine_cmd);
                }
            });

        // Tick frontend
        if self.cmd_valid && self.frontend.command_queue.push(self.cmd) {
            println!("Pushed {:?} to command queue", self.cmd);
            self.cmd_valid = false;
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

        Ok(())
    }

    fn busy(&mut self) -> bool {
        self.frontend.command_queue.full()
    }
}
