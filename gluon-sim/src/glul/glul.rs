use std::sync::{Arc, RwLock};

use cyclotron::{
    base::behavior::ModuleBehaviors,
    base::mem::HasMemory,
    muon::{
        config::{LaneConfig, MuonConfig},
        core::MuonCore,
    },
    neutrino::{config::NeutrinoConfig, neutrino::Neutrino},
    sim::{log::Logger, toy_mem::ToyMemory},
};
use serde::Deserialize;

use crate::common::base::{Clocked, Configurable, SimErr, ThreadBlock};

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct GLULConfig {
    pub id: usize,
    pub num_cores: usize,
    pub num_warps: usize,
    pub num_lanes: usize,
    pub regs_per_core: usize,
    pub shmem: usize,
}

impl Default for GLULConfig {
    fn default() -> Self {
        GLULConfig {
            id: 0,
            num_cores: 4,
            num_warps: 4,
            num_lanes: 16,
            regs_per_core: 256,
            shmem: 4096,
        }
    }
}

impl GLULConfig {
    pub fn default_id(id: usize) -> Self {
        GLULConfig {
            id,
            ..Default::default()
        }
    }
}

#[derive(Default, Debug, Clone, Copy)]
pub struct GLULReq {
    pub thread_block: ThreadBlock,
    pub n_tb: u32,
    pub idx: usize,
}

#[derive(Default, Debug, Clone)]
pub struct GLULStatus {
    pub config: GLULConfig,
    pub busy: Arc<RwLock<bool>>,
}

impl Configurable<GLULConfig> for GLULStatus {
    fn new(config: GLULConfig) -> Self {
        GLULStatus {
            config,
            busy: Arc::new(RwLock::new(false)),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum GLULState {
    S0,
    S1,
    S2,
    S3,
}

pub struct GLUL {
    status: GLULStatus,
    cores: Vec<MuonCore>,
    neutrino: Neutrino,
    dram: Arc<RwLock<ToyMemory>>,
    logger: Arc<Logger>,

    state: GLULState,
    thread_block: ThreadBlock,
    n_tb: u32,
    engine_idx: usize,

    done: bool,
}

impl Configurable<GLULConfig> for GLUL {
    fn new(config: GLULConfig) -> Self {
        let muon_config = MuonConfig {
            num_cores: config.num_cores,
            num_warps: config.num_warps,
            num_lanes: config.num_lanes,
            lane_config: LaneConfig::default(),
        };
        let logger = Arc::new(Logger::new(0));
        let dram = Arc::new(RwLock::new(ToyMemory::default()));
        GLUL {
            status: GLULStatus::new(config),
            cores: (0..config.num_cores)
                .map(|i| MuonCore::new(Arc::new(muon_config), i, &logger, dram.clone()))
                .collect(),
            neutrino: Neutrino::new(Arc::new(NeutrinoConfig::default())),
            dram,
            logger,
            state: GLULState::S0,
            thread_block: ThreadBlock::default(),
            n_tb: 0,
            engine_idx: 0,
            done: false,
        }
    }
}

impl Clocked for GLUL {
    fn tick(&mut self) -> Result<(), SimErr> {
        match self.state {
            GLULState::S0 => {}
            GLULState::S1 => {
                let threads_per_tb = self.thread_block.dim.0 as u32
                    * self.thread_block.dim.1 as u32
                    * self.thread_block.dim.2 as u32;
                let warps_per_tb = threads_per_tb / self.status.config.num_lanes as u32;
                let warps_per_core = (warps_per_tb / self.status.config.num_cores as u32).max(1);
                let cores_per_tb = self.status.config.num_cores / self.n_tb as usize;
                println!(
                    "Threads per TB: {}, Warps per TB: {}, Warps per Core: {}, Cores per TB: {}",
                    threads_per_tb, warps_per_tb, warps_per_core, cores_per_tb
                );
                (0..self.n_tb).for_each(|tb_idx| {
                    let core_start = tb_idx * cores_per_tb as u32;
                    let core_end = core_start + cores_per_tb as u32;
                    println!("Core start: {}, Core end: {}", core_start, core_end);
                    (core_start..core_end).for_each(|core_idx| {
                        self.cores
                            .get_mut(core_idx as usize)
                            .expect("Core index out of bounds")
                            .spawn_n_warps(self.thread_block.pc, warps_per_core as usize);
                    });
                });
                self.state = GLULState::S2;
            }
            GLULState::S2 => {
                self.cores.iter_mut().for_each(|core| {
                    core.tick_one();
                    core.execute(&mut self.neutrino);
                });
                self.neutrino.tick_one();
                self.neutrino
                    .update(&mut self.cores.iter_mut().map(|c| &mut c.scheduler).collect());

                if self.cores.iter().all(|core| core.all_warps_retired()) {
                    self.state = GLULState::S3;
                }
            }
            GLULState::S3 => {
                self.done = true;
                self.state = GLULState::S0;
                *self.status.busy.write().expect("GLUL busy poisoned") = false;
            }
        };

        Ok(())
    }

    fn busy(&mut self) -> bool {
        self.state != GLULState::S0
    }
}

impl GLUL {
    pub fn new_with_logger_dram(
        config: GLULConfig,
        logger: Arc<Logger>,
        dram: Arc<RwLock<ToyMemory>>,
    ) -> Self {
        let muon_config = MuonConfig {
            num_cores: config.num_cores,
            num_warps: config.num_warps,
            num_lanes: config.num_lanes,
            lane_config: LaneConfig::default(),
        };
        GLUL {
            status: GLULStatus::new(config),
            cores: (0..config.num_cores)
                .map(|i| MuonCore::new(Arc::new(muon_config), i, &logger, dram.clone()))
                .collect(),
            neutrino: Neutrino::new(Arc::new(NeutrinoConfig::default())),
            dram,
            logger,
            state: GLULState::S0,
            thread_block: ThreadBlock::default(),
            n_tb: 0,
            engine_idx: 0,
            done: false,
        }
    }

    pub fn submit_thread_block(&mut self, thread_block: ThreadBlock, n_tb: u32, engine_idx: usize) {
        println!(
            "Submitting {} thread blocks {:?} to GLUL {:?}",
            n_tb, thread_block, self.status.config
        );
        let mut dram = self.dram.write().expect("gmem poisoned");
        let pc = thread_block.pc as usize;
        (0..8).for_each(|idx| {
            let addr = pc + idx * 8;
            let instr = dram.read::<8>(addr).expect("instruction read failed");
            let bytes = instr
                .iter()
                .map(|byte| format!("{:02x}", byte))
                .collect::<Vec<_>>()
                .join("");
            println!("PC 0x{:08x}: {}", addr, bytes);
        });
        self.thread_block = thread_block;
        self.n_tb = n_tb;
        self.engine_idx = engine_idx;
        self.state = GLULState::S1;
        *self.status.busy.write().expect("GLUL busy poisoned") = true;
    }

    pub fn try_acknowledge_done(&mut self) -> Option<(usize, u32)> {
        if self.done {
            self.done = false;
            Some((self.engine_idx, self.n_tb))
        } else {
            None
        }
    }

    pub fn get_status(&self) -> &GLULStatus {
        &self.status
    }
}
