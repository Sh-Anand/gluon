use std::sync::{Arc, RwLock};

use cyclotron::{
    base::behavior::ModuleBehaviors,
    info,
    muon::{
        config::{LaneConfig, MuonConfig},
        core::MuonCore,
        warp::ExecErr,
    },
    neutrino::{config::NeutrinoConfig, neutrino::Neutrino},
    sim::{log::Logger, toy_mem::ToyMemory},
};
use serde::Deserialize;

use crate::common::base::{Clocked, Configurable, SimErr, ThreadBlocks};

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

#[derive(Default, Debug, Clone)]
pub struct GLULReq {
    pub thread_blocks: Option<ThreadBlocks>,
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
    logger: Arc<Logger>,

    state: GLULState,
    thread_blocks: Option<ThreadBlocks>,
    engine_idx: usize,

    dram: Arc<RwLock<ToyMemory>>,

    done: bool,
    err: Result<(), ExecErr>,
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
            logger,
            state: GLULState::S0,
            thread_blocks: None,
            engine_idx: 0,
            done: false,
            err: Ok(()),
            dram: Arc::new(RwLock::new(ToyMemory::default())),
        }
    }
}

impl Clocked for GLUL {
    fn tick(&mut self) -> Result<(), SimErr> {
        match self.state {
            GLULState::S0 => {
                if self.thread_blocks.is_some() {
                    *self.status.busy.write().expect("GLUL busy poisoned") = true;
                    self.done = false;
                    self.cores.iter_mut().for_each(|core| core.reset());
                    self.neutrino.reset();
                    self.state = GLULState::S1;
                }
            }
            GLULState::S1 => {
                let thread_blocks = self.thread_blocks.as_ref().expect("Thread blocks not set");
                let threads_per_tb = thread_blocks.block_dim.0 as u32
                    * thread_blocks.block_dim.1 as u32
                    * thread_blocks.block_dim.2 as u32;
                let warps_per_tb = threads_per_tb / self.status.config.num_lanes as u32;
                let cores_per_tb = (warps_per_tb as f32 / self.status.config.num_warps as f32).ceil() as usize;
                let warps_per_core = warps_per_tb / cores_per_tb as u32;
                let mut thread_idx = (0, 0, 0);
                thread_blocks.block_idxs.iter().enumerate().for_each(|(tb_idx, block_idx)| {
                    let core_start = tb_idx * cores_per_tb;
                    let core_end = core_start + cores_per_tb;
                    (core_start..core_end).for_each(|core_idx| {
                        let mut thread_idxs = Vec::new();
                        for _ in 0..warps_per_core {
                            let mut warp_thread_idxs = Vec::new();
                            for _ in 0..self.status.config.num_lanes {
                                warp_thread_idxs.push(thread_idx);
                                thread_idx.0 = (thread_idx.0 + 1) % thread_blocks.block_dim.0 as u16;
                                if block_idx.0 == 0 {
                                    thread_idx.1 = (thread_idx.1 + 1) % thread_blocks.block_dim.1 as u16;
                                    if thread_idx.1 == 0 {
                                        thread_idx.2 = (thread_idx.2 + 1) % thread_blocks.block_dim.2 as u16;
                                    }
                                }
                            }
                            thread_idxs.push(warp_thread_idxs);
                        }
                        info!(
                            self.logger,
                            "Spawning block_idx{:?} warps{:?} to core {:?}", block_idx, thread_idxs, core_idx
                        );
                        self.cores
                            .get_mut(core_idx)
                            .expect("Core index out of bounds")
                            .spawn_n_warps(thread_blocks.pc, block_idx.clone(), thread_idxs);
                    });
                });
                self.state = GLULState::S2;
            }
            GLULState::S2 => {
                self.cores.iter_mut().for_each(|core| {
                    core.tick_one();
                    if let Err(e) = core.execute(&mut self.neutrino) {
                        self.err = Err(e);
                        self.state = GLULState::S3;
                    }
                });
                self.neutrino.tick_one();
                self.neutrino
                    .update(&mut self.cores.iter_mut().map(|c| &mut c.scheduler).collect());

                if self.cores.iter().all(|core| core.all_warps_retired()) {
                    self.err = Ok(());
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
            logger,
            state: GLULState::S0,
            thread_blocks: None,
            engine_idx: 0,
            done: false,
            err: Ok(()),
            dram,
        }
    }

    pub fn submit_thread_block(&mut self, thread_blocks: ThreadBlocks, engine_idx: usize) {
        info!(
            self.logger,
            "Submitting {:?} to {:?}", thread_blocks, self.status.config
        );
        self.thread_blocks = Some(thread_blocks);
        self.engine_idx = engine_idx;
        self.state = GLULState::S1;
    }

    pub fn try_acknowledge_done_err(&mut self) -> Option<Result<(usize, u32), (usize, ExecErr)>> {
        if self.done {
            self.done = false;
            let n_tb = self.thread_blocks.as_ref().expect("Thread blocks not set").block_idxs.len() as u32;
            self.thread_blocks = None;
            Some(
                self.err
                    .clone()
                    .map(|()| (self.engine_idx, n_tb))
                    .map_err(|e| (self.engine_idx, e)),
            )
        } else {
            None
        }
    }

    pub fn try_kill(&mut self, engine_idx: usize) {
        if self.state != GLULState::S0 && self.engine_idx == engine_idx {
            self.done = false;
            self.state = GLULState::S0;
            *self.status.busy.write().expect("GLUL busy poisoned") = false;
        }
    }

    pub fn get_status(&self) -> &GLULStatus {
        &self.status
    }
}
