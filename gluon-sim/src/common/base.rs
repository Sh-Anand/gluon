use cyclotron::muon::warp::ExecErr;

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub enum CmdType {
    #[default]
    NOP,
    KERNEL,
    MEM,
    CSR,
    FENCE,
    UNDEFINED,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimErr {
    TIMEOUT,
    EXECUTION,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Completion {
    OK,
    EXECUTION,
}

pub trait Clocked {
    fn tick(&mut self) -> Result<(), SimErr>;
    fn busy(&mut self) -> bool;
}

pub trait Configurable<T: Default> {
    fn new(config: T) -> Self;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct Command {
    bytes: [u8; 16],
}

impl Command {
    pub fn from_bytes(bytes: [u8; 16]) -> Self {
        Command { bytes }
    }

    pub fn cmd_type(&self) -> CmdType {
        match self.bytes[0] {
            0 => CmdType::KERNEL,
            1 => CmdType::MEM,
            2 => CmdType::CSR,
            3 => CmdType::FENCE,
            _ => CmdType::UNDEFINED,
        }
    }

    pub fn id(&self) -> u8 {
        self.bytes[1]
    }

    pub fn is_fence(&self) -> bool {
        match self.cmd_type() {
            CmdType::FENCE => true,
            _ => false,
        }
    }

    pub fn slice(&self, i: usize, j: usize) -> &[u8] {
        &self.bytes[i..j]
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub enum DMADir {
    #[default]
    H2D,
    D2H,
}

impl From<bool> for DMADir {
    fn from(value: bool) -> Self {
        if !value {
            DMADir::D2H
        } else {
            DMADir::H2D
        }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct DMAReq {
    pub dir: DMADir,
    pub src_addr: u32,
    pub target_addr: u32,
    pub sz: u32,
    pub done: bool,
}

#[derive(Debug, Default, Clone)]
pub struct MemReq {
    pub addr: u32,
    pub write: bool,
    pub bytes: u32,
    pub data: Vec<u8>,
}

#[derive(Debug, Default, Clone)]
pub struct MemResp {
    pub data: Option<Vec<u8>>,
}

#[derive(Debug, Default, Clone, Copy)]
pub struct ThreadBlock {
    pub id: u32,
    pub pc: u32,
    pub dim: (u16, u16, u16),
    pub regs: u32,
    pub shmem: u32,
}
#[derive(Debug, Default, Clone, Copy)]
pub struct Event {
    bytes: [u8; 16],
}

impl Event {
    pub fn from_kernel_err(cmd_id: u8, err: Result<(), ExecErr>) -> Self {
        if let Err(err) = err {
            let mut bytes = [0u8; 16];
            bytes[0] = cmd_id;
            bytes[1] = Completion::EXECUTION as u8;
            bytes[2..6].copy_from_slice(&err.pc.to_le_bytes());
            bytes[6..10].copy_from_slice(&(err.warp_id as u32).to_le_bytes());
            Event { bytes }
        } else {
            Event::from_ok(cmd_id)
        }
    }

    pub fn from_ok(cmd_id: u8) -> Self {
        let mut bytes = [0u8; 16];
        bytes[0] = cmd_id;
        bytes[1] = Completion::OK as u8;
        Event { bytes }
    }

    pub fn to_exec_err(&self) -> ExecErr {
        ExecErr {
            pc: u32::from_le_bytes([self.bytes[2], self.bytes[3], self.bytes[4], self.bytes[5]]),
            warp_id: self.bytes[6] as usize,
            message: None,
        }
    }
}
