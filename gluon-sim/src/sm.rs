use std::ops::Range;
use std::sync::{Arc, RwLock};

use cyclotron::{
    base::behavior::ModuleBehaviors,
    muon::{
        config::{LaneConfig, MuonConfig},
        core::MuonCore,
        warp::ExecErr,
    },
    neutrino::{config::NeutrinoConfig, neutrino::Neutrino},
    sim::{log::Logger, flat_mem::FlatMemory},
};
use serde::Deserialize;

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct SMConfig {
    pub id: usize,
    pub num_cores: usize,
    pub num_warps: usize,
    pub num_lanes: usize,
    pub regs_per_core: usize,
    pub shmem: usize,
}

impl Default for SMConfig {
    fn default() -> Self {
        SMConfig {
            id: 0,
            num_cores: 4,
            num_warps: 4,
            num_lanes: 16,
            regs_per_core: 256,
            shmem: 4096,
        }
    }
}

// cores sharing one smem; kernel engines launch onto and retire from it directly
pub struct SM {
    pub config: SMConfig,
    pub cores: Vec<MuonCore>,
    pub running: Vec<bool>,          // per core: has warps to tick
    pub errs: Vec<Option<ExecErr>>,  // per core: error that stopped it
    smem_used: Vec<Range<usize>>,    // allocated smem ranges, sorted by start
    neutrino: Neutrino,
}

impl SM {
    pub fn new(id: usize, config: SMConfig, muon_logger: Arc<Logger>, dram: Arc<RwLock<FlatMemory>>) -> Self {
        let muon_config = MuonConfig {
            num_cores: config.num_cores,
            num_warps: config.num_warps,
            num_lanes: config.num_lanes,
            num_regs: 128, // architectural regs per thread; regs_per_core is the CP-side physical budget
            start_pc: 0,
            smem_size: config.shmem,
            lane_config: LaneConfig::default(),
        };
        let shared_mem = Arc::new(RwLock::new(FlatMemory::new_with_size(config.shmem, None)));
        SM {
            config,
            cores: (0..config.num_cores)
                .map(|i| MuonCore::new(Arc::new(muon_config), id, i, &muon_logger, dram.clone(), shared_mem.clone()))
                .collect(),
            running: vec![false; config.num_cores],
            errs: vec![None; config.num_cores],
            smem_used: vec![],
            neutrino: Neutrino::new(Arc::new(NeutrinoConfig::default())),
        }
    }

    // first fit: the lowest gap between allocations that holds `len` bytes
    pub fn alloc_smem(&mut self, len: usize) -> Option<Range<usize>> {
        if len == 0 {
            return Some(0..0);
        }
        let mut base = 0;
        let idx = match self.smem_used.iter().position(|used| {
            let fits = used.start - base >= len;
            if !fits {
                base = used.end;
            }
            fits
        }) {
            Some(idx) => idx,
            None if self.config.shmem - base >= len => self.smem_used.len(),
            None => return None,
        };
        self.smem_used.insert(idx, base..base + len);
        Some(base..base + len)
    }

    pub fn free_smem(&mut self, range: &Range<usize>) {
        self.smem_used.retain(|used| used != range);
    }

    pub fn busy(&self) -> bool {
        self.running.contains(&true)
    }

    pub fn tick(&mut self) {
        if !self.busy() {
            return;
        }
        self.cores
            .iter_mut()
            .zip(self.running.iter_mut())
            .zip(self.errs.iter_mut())
            .filter(|((_, running), _)| **running)
            .for_each(|((core, running), err)| {
                core.tick_one();
                if let Err(e) = core.process(&mut self.neutrino) {
                    *err = Some(e);
                    *running = false;
                }
            });
        self.neutrino.tick_one();
        self.neutrino.update(
            &mut self.cores
                .iter_mut()
                .zip(self.running.iter())
                .filter(|(_, running)| **running)
                .map(|(core, _)| &mut core.scheduler)
                .collect(),
        );
    }
}
