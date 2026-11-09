use cyclotron::muon::config::{MuonConfig, UnitsConfig};
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

/// Muon core timing, uniform across SMs
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct CoreConfig {
    pub ibuf_depth: usize,     // decoded instructions per warp available for issue
    pub redirect_latency: u64, // issue of a pc-changing control op to the first issue at its target
    pub units: UnitsConfig,    // functional unit widths and latencies
}

impl Default for CoreConfig {
    fn default() -> Self {
        CoreConfig { ibuf_depth: 4, redirect_latency: 1, units: UnitsConfig::default() }
    }
}

impl SMConfig {
    /// the Muon cores' config for this SM under the uniform core timing
    pub fn muon_config(&self, core: &CoreConfig) -> MuonConfig {
        MuonConfig {
            num_cores: self.num_cores,
            num_warps: self.num_warps,
            num_lanes: self.num_lanes,
            num_regs: 128, // architectural regs per thread
            regs_per_core: self.regs_per_core,
            smem_size: self.shmem,
            ibuf_depth: core.ibuf_depth,
            redirect_latency: core.redirect_latency,
            units: core.units,
            ..MuonConfig::default()
        }
    }
}
