use crate::{
    common::base::{Clocked, CmdType, Configurable, DMADir, DMAReq, Event, MemReq, MemResp, SimErr},
    glug::engine::{Engine, EngineCommand},
    glul::glul::{GLULReq, GLULStatus},
};
use cyclotron::{info, muon::warp::ExecErr};
use cyclotron::sim::log::Logger;
use std::sync::Arc;
use serde::Deserialize;

#[derive(Debug, Default, Clone, Copy, Deserialize)]
#[serde(default)]
pub struct MemEngineConfig {}

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub enum MemEngineState {
    #[default]
    I, 
    C0,
    C1,
    S0,
    S1,
}

#[derive(Debug, Default, Clone, Copy)]
pub enum MemOp {
    #[default]
    COPY,
    SET,
}

impl From<u8> for MemOp {
    fn from(value: u8) -> Self {
        match value {
            0 => MemOp::COPY,
            1 => MemOp::SET,
            _ => panic!("Invalid mem op"),
        }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct MemCommand {
    pub id: u8,
    pub op: MemOp,
    pub bytes: [u8; 13],
}

#[derive(Debug, Default, Clone, Copy)]
pub struct CopyCommand {
    pub src: u32,
    pub dst: u32,
    pub len: u32,
    pub flags: u8,
}

impl CopyCommand {
    pub fn from_bytes(bytes: [u8; 13]) -> Self {
        CopyCommand {
            src: u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            dst: u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]),
            len: u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]),
            flags: bytes[12],
        }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct SetCommand {
    pub dst: u32,
    pub value: u32,
    pub len: u32,
    pub flags: u8,
}

impl SetCommand {
    pub fn from_bytes(bytes: [u8; 13]) -> Self {
        SetCommand {
            dst: u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            value: u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]),
            len: u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]),
            flags: bytes[12],
        }
    }
}

impl MemCommand {
    pub fn from_engine_cmd(cmd: EngineCommand) -> Self {
        let payload = cmd.payload();

        let op = MemOp::from(payload[0]);
        let bytes = payload[1..13].try_into().unwrap();

        MemCommand {
            id: cmd.id(),
            op,
            bytes,
        }
    }
}

pub struct MemEngine {
    cmd: Option<(MemCommand, usize)>, // (command, completion idx)

    dma_req: Option<DMAReq>,
    mem_req: Option<MemReq>,
    mem_resp: Option<MemResp>,

    state: MemEngineState,
    err: Option<Result<(), String>>, // TODO mem errors

    logger: Arc<Logger>,
}

impl Engine for MemEngine {
    fn set_cmd(&mut self, cmd: EngineCommand, completion_idx: usize) {
        self.cmd = Some((MemCommand::from_engine_cmd(cmd), completion_idx));
    }

    fn busy(&self) -> bool {
        self.state != MemEngineState::I
    }

    fn cmd_type(&self) -> CmdType {
        CmdType::MEM
    }

    fn set_logger(&mut self, logger: Arc<Logger>) {
        self.logger = logger;
    }

    fn get_dma_req(&self) -> Option<&DMAReq> {
        self.dma_req.as_ref()
    }

    fn done_dma_req(&mut self) {
        self.dma_req.expect("Mem engine: DMA req not set").done = true;
    }

    fn get_mem_req(&self) -> Option<&MemReq> {
        self.mem_req.as_ref()
    }

    fn set_mem_resp(&mut self, data: Option<&Vec<u8>>) {
        assert!(data.is_none(), "Mem engine: has issued a read");
        self.mem_resp = Some(MemResp { data: None });
    }

    fn get_glul_req(&self) -> Option<&GLULReq> {
        None
    }

    fn clear_glul_req(&mut self) {
        panic!("Mem engine: no gluls to clear");
    }

    fn notify_glul_done(&mut self, _: u32) {
        panic!("Mem engine: no gluls to notify");
    }

    fn set_gluls(&mut self, _: Vec<GLULStatus>) {}

    fn notify_glul_err(&mut self, _: ExecErr) {
        panic!("Mem engine: no gluls to notify");
    }

    fn get_completion(&self) -> Option<(Event, usize)> {
        self.err.as_ref().map(|err|{
            assert!(err.is_ok(), "Mem engine: cannot error");
            (Event::default(), self.cmd.expect("Command not set, no completion exists").1)
        })
    }
}

impl Configurable<MemEngineConfig> for MemEngine {
    fn new(_config: MemEngineConfig) -> Self {
        MemEngine {
            cmd: None,
            dma_req: None,
            mem_req: None,
            mem_resp: None,
            state: MemEngineState::I,
            err: None,
            logger: Arc::new(Logger::new(0)),
        }
    }
}

impl Clocked for MemEngine {
    fn tick(&mut self) -> Result<(), SimErr> {
        match self.state {
            MemEngineState::I => {
                if let Some(cmd) = &self.cmd {
                    match cmd.0.op {
                        MemOp::COPY => {
                            self.state = MemEngineState::C0;
                        }
                        MemOp::SET => {
                            self.state = MemEngineState::S0;
                        }
                    }
                    self.err = None;
                    info!(self.logger, "Mem engine: command {:?}", cmd.0);
                }
            }
            MemEngineState::C0 => {
                let copy_cmd = CopyCommand::from_bytes(self.cmd.expect("Mem engine: Command not set").0.bytes);
                let dir = DMADir::from(copy_cmd.flags & 1 == 1);
                self.dma_req = Some(DMAReq {
                    dir,
                    src_addr: copy_cmd.src,
                    target_addr: copy_cmd.dst,
                    sz: copy_cmd.len,
                    done: false,
                });
                self.state = MemEngineState::C1;
                info!(self.logger, "Mem engine: DMA req {:?}", self.dma_req.as_ref().expect("Mem engine: DMA req not set"));
            }
            MemEngineState::C1 => {
                if let Some(dma_req) = &self.dma_req {
                    if dma_req.done {
                        self.cmd = None;
                        self.dma_req = None;
                        self.state = MemEngineState::I;
                        self.err = Some(Ok(()));
                        info!(self.logger, "Mem engine: DMA req done");
                    }
                }
            }
            MemEngineState::S0 => {
                let set_cmd = SetCommand::from_bytes(self.cmd.expect("Mem engine: Command not set").0.bytes);
                self.mem_req = Some(MemReq {
                    addr: set_cmd.dst,
                    write: true,
                    bytes: set_cmd.len,
                    data: vec![set_cmd.value as u8; set_cmd.len as usize], // TODO support upto 4 byte words
                });
                self.state = MemEngineState::S1;
                info!(self.logger, "Mem engine: command {:?}", set_cmd);
            }
            MemEngineState::S1 => {
                if let Some(_) = &self.mem_resp {
                    self.cmd = None;
                    self.mem_req = None;
                    self.mem_resp = None;
                    self.state = MemEngineState::I;
                    self.err = Some(Ok(()));
                    info!(self.logger, "Mem engine: mem set done");
                }
            }
        }

        Ok(())
    }

    fn busy(&mut self) -> bool {
        false
    }
}
