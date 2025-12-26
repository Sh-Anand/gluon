use crate::common::base::{Clocked, CmdType, Command, Configurable, DMADir, Event, SimErr};
use crate::glug::completion::{Completion, CompletionConfig};
use crate::glug::decode_dispatch::{DecodeDispatch, DecodeDispatchConfig};
use crate::glug::engine::{Engine, EngineConfig};
use crate::glug::frontend::{Frontend, FrontendConfig};
use crate::glul::glul::{GLULConfig, GLUL};
use cyclotron::base::mem::HasMemory;
use cyclotron::info;
use cyclotron::sim::log::Logger;
use cyclotron::sim::flat_mem::FlatMemory;
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
    pub gluon_log_level: u64,
    pub muon_log_level: u64,
}

pub struct GLUG {
    cmd_valid: bool,
    cmd: Command,

    frontend: Frontend,
    decode_dispatch: DecodeDispatch,
    engines: Vec<Box<dyn Engine>>,
    completion: Completion,

    gluls: Vec<GLUL>,

    dram: Arc<RwLock<FlatMemory>>,

    logger: Arc<Logger>,
}

impl GLUG {
    pub fn submit_command(&mut self, command: Command) {
        self.cmd_valid = true;
        self.cmd = command;
    }

    pub fn get_completion(&mut self) -> Option<Event> {
        self.completion.pop_completion()
    }
}

impl Configurable<GLUGConfig> for GLUG {
    fn new(config: GLUGConfig) -> Self {
        let glul_configs = config.gluls.clone();
        let engine_config = config.engine.clone();

        let flat_mem = FlatMemory::new(None);
        let dram = Arc::new(RwLock::new(flat_mem));
        let logger = Arc::new(Logger::new(config.gluon_log_level));
        let muon_logger = Arc::new(Logger::new(config.muon_log_level));

        let gluls = glul_configs
            .iter()
            .copied()
            .map(|config| GLUL::new_with_logger_dram(config, logger.clone(), muon_logger.clone(), dram.clone()))
            .collect::<Vec<_>>();

        let mut engines = engine_config.generate_engines(logger.clone());
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
        // TODO: Report erroring threadid

        // Check GLUL completions, notify engines of completion or error, terminate GLULs of erroring engines
        self.gluls
            .iter_mut()
            .filter_map(|glul| glul.try_acknowledge_done_err())
            .collect::<Vec<_>>()
            .into_iter()
            .for_each(|result| {
                if let Ok((engine_idx, tbs)) = result {
                    self.engines
                        .get_mut(engine_idx)
                        .expect("Engine idx out of bounds")
                        .notify_glul_done(tbs);
                } else if let Err((engine_idx, err)) = result {
                    self.engines
                        .get_mut(engine_idx)
                        .expect("Engine idx out of bounds")
                        .notify_glul_err(err);
                    self.gluls
                        .iter_mut()
                        .for_each(|glul| glul.try_kill(engine_idx));
                }
            });

        // Enqueue engine completions
        self.engines.iter_mut().for_each(|engine| {
            if let Some((event, completion_idx)) = engine.get_completion() {
                self.completion.set(completion_idx, event);
            }
        });

        // Service GLUL schedules
        self.engines
            .iter_mut()
            .enumerate()
            .for_each(|(idx, engine)| {
                if let Some(glul_req) = engine.get_glul_req() {
                    let thread_blocks = glul_req.thread_blocks.as_ref().expect("Thread blocks not set").clone();
                    self.gluls[glul_req.idx].submit_thread_block(
                        thread_blocks,
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
            info!(self.logger, "Served mem {:?}", mem_req);
            if mem_req.write {
                dram.write(mem_req.addr as usize, &mem_req.data).expect("gmem write errored");
                engine.set_mem_resp(None);
            } else {
                let read_data = dram.read(mem_req.addr as usize, mem_req.bytes as usize).expect("gmem read errored");
                engine.set_mem_resp(Some(&read_data.to_vec()));
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

                    let data = (0..dma_req.sz)
                        .map(|byte| unsafe { *((dma_req.src_addr + byte) as *const u8) })
                        .collect::<Vec<u8>>();
                    dram.write(dma_req.target_addr as usize, &data).expect("gmem write errored");
                }

                DMADir::D2H => {
                    let dram = self.dram.read().expect("gmem poisoned");
                    let data = dram.read(dma_req.src_addr as usize, dma_req.sz as usize).expect("gmem read errored");
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
                if let (Some((engine_cmd, completion_idx)), Some(engine_idx)) = x {
                    self.engines
                        .get_mut(*engine_idx)
                        .expect("Engine idx must exist!")
                        .set_cmd(*engine_cmd, *completion_idx);
                }
            });

        // Tick frontend
        if self.cmd_valid && self.frontend.command_queue.push(self.cmd) {
            info!(self.logger, "Pushed {:?} to command queue", self.cmd);
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
            let completion_idx = self.completion.allocate();
            self.decode_dispatch
                .enqueue(frontend_out_cmd, completion_idx);
        }

        Ok(())
    }

    fn busy(&mut self) -> bool {
        self.frontend.command_queue.full()
    }
}
