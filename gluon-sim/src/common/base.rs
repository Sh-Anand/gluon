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

#[derive(Debug, Default, Clone, Copy)]
pub struct DMAReq {
    pub dir: DMADir,
    pub src_addr: u32,
    pub target_addr: u32,
    pub sz: u32,

    pub valid: bool,
    pub done: bool,
}

#[derive(Debug, Default, Clone)]
pub struct MemReq {
    pub addr: u32,
    pub write: bool,
    pub bytes: u32,
    pub data: Vec<u8>,
    pub valid: bool,
}

#[derive(Debug, Default, Clone)]
pub struct MemResp {
    pub data: Vec<u8>,
    pub valid: bool,
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
    done: bool,
}
