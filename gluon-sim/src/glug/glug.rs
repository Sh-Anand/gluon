use crate::common::base::{Clocked, CmdType, Command, Configurable, DMADir, SimErr};
use crate::glug::completion::{Completion, CompletionConfig};
use crate::glug::decode_dispatch::{DecodeDispatch, DecodeDispatchConfig};
use crate::glug::engine::{Engine, EngineConfig};
use crate::glug::frontend::{Frontend, FrontendConfig};
use crate::glul::glul::{GLULConfig, GLUL};
use cyclotron::sim::log::Logger;
use serde::Deserialize;
use std::collections::HashMap;
use std::sync::Arc;

pub struct Memory {
    data: HashMap<u32, u8>,
}

impl Memory {
    pub fn read(&self, addr: &u32) -> u8 {
        *self.data.get(addr).expect("Invalid DRAM Addr")
    }

    pub fn write(&mut self, addr: u32, data: u8) {
        self.data.insert(addr, data);
    }

    pub fn read_bytes(&self, addr: u32, bytes: u32) -> Vec<u8> {
        (0..bytes)
            .map(|offset| {
                let byte_addr = addr + offset as u32;
                *self.data.get(&byte_addr).expect("Invalid DRAM Addr")
            })
            .collect()
    }

    pub fn write_bytes(&mut self, addr: u32, data: &Vec<u8>) {
        data.iter().enumerate().for_each(|(idx, byte)| {
            self.data.insert(addr + idx as u32, *byte);
        });
    }
}
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

    dram: Memory,

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
        GLUG {
            cmd: Command::default(),
            cmd_valid: false,
            frontend: Frontend::new(config.frontend),
            decode_dispatch: DecodeDispatch::new(config.decode_dispatch),
            engines: config.engine.generate_engines(),
            completion: Completion::new(config.completion),
            gluls: (0..config.gluls.len())
                .map(|i| GLUL::new(config.gluls[i]))
                .collect(),
            dram: Memory {
                data: HashMap::new(),
            },
            logger: Arc::new(Logger::new(config.log_level)),
        }
    }
}

impl Clocked for GLUG {
    fn tick(&mut self) -> Result<(), SimErr> {
        // TODO: Tick completion

        // Service GLUL schedules
        self.engines
            .iter_mut()
            .filter_map(|engine| engine.get_glul_req())
            .for_each(|glul_if| {
                self.gluls[glul_if.config.id].submit_thread_block(glul_if.thread_block);
            });

        self.gluls.iter_mut().for_each(|glul| {
            glul.tick();
        });

        // Service Mem requests
        if let Some(engine) = self
            .engines
            .iter_mut()
            .find(|engine| engine.get_mem_req().is_some())
        {
            let mem_req = engine.get_mem_req().expect("Mem: unreachable");
            println!("Served mem {:?}", mem_req);
            if mem_req.write {
                self.dram.write_bytes(mem_req.addr, &mem_req.data);
                engine.set_mem_resp(None);
            } else {
                let read_data = self.dram.read_bytes(mem_req.addr, mem_req.bytes);
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
                    let data: Vec<u8> = (0..dma_req.sz)
                        .map(|byte| unsafe { *((dma_req.src_addr + byte) as *const u8) })
                        .collect();

                    self.dram.write_bytes(dma_req.target_addr, &data);
                }

                DMADir::D2H => {
                    let data = self.dram.read_bytes(dma_req.src_addr, dma_req.sz);
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
                        .init(*engine_cmd);
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
