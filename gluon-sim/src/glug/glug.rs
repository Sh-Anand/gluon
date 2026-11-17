use crate::common::base::{Clocked, Command, Configurable, Cycles, DMADir, Event, SimErr};
use crate::glug::completion::Completion;
use crate::glug::dispatch::{Dispatch, DispatchConfig};
use crate::glug::engine::{Engine, EngineConfig};
use crate::glug::engines::cs_engine::KernelEntry;
use crate::glug::intake::{Intake, IntakeConfig};
use crate::glug::stream::{Stream, StreamConfig};
use crate::sm::{CoreConfig, SMConfig};
use cyclotron::muon::cache::{self, Cache};
use cyclotron::muon::sm::SM;
use cyclotron::base::mem::HasMemory;
use cyclotron::info;
use cyclotron::muon::stats::CoreStats;
use cyclotron::sim::config::MemConfig;
use cyclotron::sim::log::Logger;
use cyclotron::sim::flat_mem::FlatMemory;
use serde::Deserialize;
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct GLUGConfig {
    pub intake: IntakeConfig,
    pub dispatch: DispatchConfig,
    pub stream: StreamConfig,
    pub engine: EngineConfig,
    pub sms: Vec<SMConfig>,
    pub core: CoreConfig, // Muon core timing, shared by all SMs
    pub kernel_table_size: usize, // entries; kernel ids are 8 bits
    pub host_pid: i32,
    pub driver_pid: i32,
    pub gluon_log_level: u64,
    pub muon_log_level: u64,
}

impl Default for GLUGConfig {
    fn default() -> Self {
        GLUGConfig {
            intake: IntakeConfig::default(),
            dispatch: DispatchConfig::default(),
            stream: StreamConfig::default(),
            engine: EngineConfig::default(),
            sms: vec![],
            core: CoreConfig::default(),
            kernel_table_size: 256,
            host_pid: 0,
            driver_pid: 0,
            gluon_log_level: 0,
            muon_log_level: 0,
        }
    }
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

    sms: Vec<SM>,
    kernels: Vec<KernelEntry>, // the kernel table, by kernel id

    // whole-run totals over busy ticks
    cycles: Cycles,
    sim_time: Duration,
    core_time: Duration, // inside cyclotron

    dram: FlatMemory,
    caches: Vec<Cache>, // global memory caches: the L2, then each SM's L1 and L0s

    logger: Arc<Logger>,
    host_pid: libc::pid_t,
    driver_pid: libc::pid_t,
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
        let engine_config = config.engine.clone();

        let dram = FlatMemory::new(Some(MemConfig::default()));
        let logger = Arc::new(Logger::new(config.gluon_log_level));
        let muon_logger = Arc::new(Logger::new(config.muon_log_level));

        let mut caches = vec![];
        let l2 = cache::add(&mut caches, config.core.gmem.l2, None);
        let sms = config
            .sms
            .iter()
            .copied()
            .enumerate()
            .map(|(idx, sm)| SM::new(idx, sm.muon_config(&config.core), &muon_logger, &mut caches, l2))
            .collect::<Vec<_>>();

        let engines = engine_config.generate_engines(logger.clone());

        GLUG {
            cmd: Command::default(),
            cmd_valid: false,
            sq_idx: 0,
            intake: Intake::new(&config.intake),
            dispatch: Dispatch::new(&config.dispatch),
            stream: Stream::new(&config.stream),
            engines,
            completion: Completion::new(&config.stream),
            sms,
            kernels: vec![KernelEntry::default(); config.kernel_table_size],
            cycles: Cycles::default(),
            sim_time: Duration::ZERO,
            core_time: Duration::ZERO,
            dram,
            caches,
            logger,
            host_pid: config.host_pid,
            driver_pid: config.driver_pid,
        }
    }
}

/// Wall-clock time is measured on one run of 256 consecutive ticks in this many and scaled up; reading
/// the clock on every tick is a measurable share of the run, and on an isolated tick the read is cold
/// and overstates it.  Prime, so it does not lock onto a loop in the workload.
const TIMED_EVERY: u64 = 61;

impl Clocked for GLUG {
    fn tick(&mut self) -> Result<(), SimErr> {
        // TODO: Report erroring threadid
        let start = ((self.cycles.total() >> 8) % TIMED_EVERY == 0).then(Instant::now);

        // Enqueue engine completions
        self.engines.iter_mut().for_each(|engine| {
            if let Some(event) = engine.get_completion() {
                self.completion.set_completion(event);
                println!(
                    "sim total: {}, {:.3?} (cyclotron {:.3?}, gluon {:.3?})",
                    self.cycles, self.sim_time, self.core_time, self.sim_time - self.core_time
                );
                let mut stats = CoreStats::default();
                self.sms.iter().flat_map(|sm| &sm.cores).for_each(|core| stats.add(&core.stats));
                println!("{stats}");
                let mut levels = [(0u64, 0u64); 3]; // (accesses, misses) of the L2, the L1s and the L0s
                self.caches.iter().for_each(|c| levels[c.depth] = (levels[c.depth].0 + c.accesses, levels[c.depth].1 + c.misses));
                let level = |(l, (a, m)): (usize, &(u64, u64))| format!(" | L{l} {a} accesses, {m} misses ({:.1}%)", 100.0 * *m as f64 / (*a).max(1) as f64);
                println!("cache stats{}", levels.iter().rev().enumerate().map(level).collect::<String>());
            }
        });

        // Engines retire and launch CTAs on SMs, in engine order
        self.engines
            .iter_mut()
            .try_for_each(|engine| engine.launch(&mut self.sms, &self.kernels))?;

        // Tick SMs
        let cores_busy = self.sms.iter().any(SM::busy);
        let busy = cores_busy || self.engines.iter().any(|engine| engine.busy());
        let core_start = start.map(|_| Instant::now());
        let now = self.cycles.total();
        self.sms.iter_mut().for_each(|sm| sm.tick(now, &mut self.dram, &mut self.caches));
        let core_time = core_start.map(|t| t.elapsed());

        // Service Mem requests
        if let Some(engine) = self
            .engines
            .iter_mut()
            .find(|engine| engine.get_mem_req().is_some())
        {
            let mem_req = engine.get_mem_req().expect("Mem: unreachable");
            let dram = &mut self.dram;
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
                    let dram = &mut self.dram;

                    let mut data = vec![0u8; dma_req.sz as usize];
                    let read_pid = if dma_req.host_runtime { self.host_pid } else { self.driver_pid };
                    if read_pid > 0 {
                        let mut local = libc::iovec {
                            iov_base: data.as_mut_ptr() as *mut libc::c_void,
                            iov_len: data.len(),
                        };
                        let remote = libc::iovec {
                            iov_base: dma_req.src_addr as usize as *mut libc::c_void,
                            iov_len: data.len(),
                        };
                        unsafe {
                            let _ = libc::process_vm_readv(read_pid, &mut local, 1, &remote, 1, 0);
                        }
                    }
                    dram.write(dma_req.target_addr as usize, &data).expect("gmem write errored");
                }

                DMADir::D2H => {
                    let dram = &self.dram;
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

        // Apply kernel table writes
        for (id, field, value) in self.engines.iter().filter_map(|engine| engine.table_write()) {
            self.kernels[id as usize].set(field, value);
            info!(self.logger, "Kernel table[{}] = {:?}", id, self.kernels[id as usize]);
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
                        .find(|(_, engine)| engine.cmd_type() == eq.engine_type && engine.idle())
                        .map(|(idx, _)| idx) && 
                   let Some(engine_cmd) = eq.q.pop() {
                        self.engines
                            .get_mut(engine_idx)
                            .expect("Engine idx must exist!")
                            .set_cmd(engine_cmd);
                }
        });

        // Tick stream
        if let Some(cmd) = self.stream.sqs[self.sq_idx].q.peek() && cmd.is_wait() {
            let (w_sid, w_cid) = cmd.get_wait_ids();
            if self.stream.sqs[w_sid as usize].cmd_id >= w_cid {
                self.stream.sqs[self.sq_idx].q.pop();
            }
        }
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
            // unique command id: stream id in the top byte, the stream's command count below
            let cmd_id = (*x as u64) << 56 | self.stream.sqs[*x].cmd_id;
            self.dispatch.enqueue(self.stream.try_pop(*x as u8).expect("impossible"), cmd_id);
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

        if busy {
            self.cycles.tick(cores_busy);
            if let (Some(start), Some(core_time)) = (start, core_time) {
                self.sim_time += start.elapsed() * TIMED_EVERY as u32;
                self.core_time += core_time * TIMED_EVERY as u32;
            }
        }

        Ok(())
    }

    fn busy(&mut self) -> bool {
        self.intake.command_queue.full()
    }
}
