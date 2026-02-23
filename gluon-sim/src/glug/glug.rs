use crate::common::base::{Clocked, Command, Configurable, DMADir, Event, SimErr};
use crate::glug::completion::Completion;
use crate::glug::dispatch::{Dispatch, DispatchConfig};
use crate::glug::engine::{Engine, EngineConfig};
use crate::glug::intake::{Intake, IntakeConfig};
use crate::glug::stream::{Stream, StreamConfig};
use crate::glul::glul::{GLULConfig, GLUL};
use cyclotron::base::mem::HasMemory;
use cyclotron::info;
use cyclotron::sim::config::MemConfig;
use cyclotron::sim::log::Logger;
use cyclotron::sim::flat_mem::FlatMemory;
use serde::Deserialize;
use std::sync::{Arc, RwLock};

#[derive(Debug, Default, Clone, Deserialize)]
#[serde(default)]
pub struct GLUGConfig {
    pub intake: IntakeConfig,
    pub dispatch: DispatchConfig,
    pub stream: StreamConfig,
    pub engine: EngineConfig,
    pub gluls: Vec<GLULConfig>,
    pub host_pid: i32,
    pub gluon_log_level: u64,
    pub muon_log_level: u64,
}

pub struct GLUG {
    cmd_valid: bool,
    cmd: Command,
    sq_idx: usize,

    intake: Intake,
    dispatch: Dispatch,
    stream: Stream,
    engines: Vec<Box<dyn Engine>>,
    completion: Completion,

    gluls: Vec<GLUL>,

    dram: Arc<RwLock<FlatMemory>>,

    logger: Arc<Logger>,
    host_pid: libc::pid_t,
}

impl GLUG {
    pub fn submit_command(&mut self, command: Command) {
        self.cmd_valid = true;
        self.cmd = command;
    }

    pub fn get_completion(&mut self) -> Option<Event> {
        if let Some(event) = self.completion.try_clear_completion() {
            self.stream.clear_in_flight(event.sid());
            Some(event)
        } else {
            None
        }
    }
}

impl Configurable<GLUGConfig> for GLUG {
    fn new(config: &GLUGConfig) -> Self {
        let glul_configs = config.gluls.clone();
        let engine_config = config.engine.clone();

        let flat_mem = FlatMemory::new(Some(MemConfig::default()));
        let dram = Arc::new(RwLock::new(flat_mem));
        let logger = Arc::new(Logger::new(config.gluon_log_level));
        let muon_logger = Arc::new(Logger::new(config.muon_log_level));

        let gluls = glul_configs
            .iter()
            .copied()
            .enumerate()
            .map(|(idx, config)| GLUL::new_with_logger_dram(idx, config, logger.clone(), muon_logger.clone(), dram.clone()))
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
            sq_idx: 0,
            intake: Intake::new(&config.intake),
            dispatch: Dispatch::new(&config.dispatch),
            stream: Stream::new(&config.stream),
            engines,
            completion: Completion::new(&config.stream),
            gluls,
            dram,
            logger,
            host_pid: config.host_pid,
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
            if let Some(event) = engine.get_completion() {
                self.completion.set_completion(event);
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
                        .map(|byte| unsafe { *((dma_req.src_addr + byte as u64) as *const u8) })
                        .collect::<Vec<u8>>();
                    dram.write(dma_req.target_addr as usize, &data).expect("gmem write errored");
                }

                DMADir::D2H => {
                    let dram = self.dram.read().expect("gmem poisoned");
                    let data = dram.read(dma_req.src_addr as usize, dma_req.sz as usize).expect("gmem read errored");
                    if self.host_pid > 0 {
                        let local = libc::iovec {
                            iov_base: data.as_ptr() as *mut libc::c_void,
                            iov_len: data.len(),
                        };
                        let remote = libc::iovec {
                            iov_base: dma_req.target_addr as usize as *mut libc::c_void,
                            iov_len: data.len(),
                        };
                        unsafe {
                            let _ = libc::process_vm_writev(self.host_pid, &local, 1, &remote, 1, 0);
                        }
                    }
                }
            };

            engine.done_dma_req();
        }

        // Tick engines
        self.engines
            .iter_mut()
            .try_for_each(|engine| engine.tick())?;

        // Tick decode
        self.dispatch
            .qs
            .iter_mut()
            .for_each(|eq| {
                if let Some(engine_idx) = self.engines
                        .iter_mut()
                        .enumerate()
                        .find(|(_, engine)| engine.cmd_type() == eq.engine_type && !engine.busy())
                        .map(|(idx, _)| idx) && 
                   let Some(engine_cmd) = eq.q.pop() {
                        self.engines
                            .get_mut(engine_idx)
                            .expect("Engine idx must exist!")
                            .set_cmd(engine_cmd);
                }
        });

        // Tick stream
        self.sq_idx = (self.sq_idx + 1) % self.stream.sqs.len();
        let decode_push_candidates = self.stream.sqs
            .iter()
            .enumerate()
            .filter(|(_, sq)|
                 !sq.in_flight && !sq.q.empty() && self.dispatch.can_enqueue(sq.q.peek().expect("impossible").cmd_type())
            )
            .map(|(idx, _)| idx )
            .collect::<Vec<_>>();

        if !decode_push_candidates.is_empty() {
            let x = &decode_push_candidates[self.sq_idx % decode_push_candidates.len()];
            self.dispatch.enqueue(self.stream.try_pop(*x as u8).expect("impossible"));
        }

        // Tick intake
        if self.cmd_valid && self.intake.command_queue.push(self.cmd) {
            info!(self.logger, "Pushed {:?} to command queue", self.cmd);
            self.cmd_valid = false;
            self.cmd = Command::default();
        }

        if let Some(intake_out_cmd) = self
            .intake
            .command_queue
            .peek()
            .map(|cmd| self.stream.can_enqueue(cmd.sid())) 
            .unwrap_or(false)
            .then(|| {
                self.intake
                    .command_queue
                    .pop()
                    .expect("Cannot be empty here")
            })
        {
            self.stream.enqueue(intake_out_cmd.sid(), intake_out_cmd);
        }

        Ok(())
    }

    fn busy(&mut self) -> bool {
        self.intake.command_queue.full()
    }
}
