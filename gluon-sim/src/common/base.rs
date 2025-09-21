use crate::glug::engine::Engine;

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

    pub fn get_engine_cmd(&self) -> EngineCommand {
        let mut bytes = [0u8; 14];
        bytes.copy_from_slice(&self.bytes[2..]);
        EngineCommand {
            id: bytes[1],
            bytes: bytes,
        }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct EngineCommand {
    id: u8,
    bytes: [u8; 14],
}

impl EngineCommand {
    pub fn to_kernel_cmd(&self) -> KernelCommand {
        let host_addr =
            u32::from_le_bytes([self.bytes[0], self.bytes[1], self.bytes[2], self.bytes[3]]);

        let sz = u32::from_le_bytes([self.bytes[4], self.bytes[5], self.bytes[6], self.bytes[7]]);

        let gpu_addr =
            u32::from_le_bytes([self.bytes[8], self.bytes[9], self.bytes[10], self.bytes[11]]);

        KernelCommand {
            id: self.id,
            host_addr,
            sz,
            gpu_addr,
        }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct KernelCommand {
    pub id: u8,
    pub host_addr: u32,
    pub sz: u32,
    pub gpu_addr: u32,
}

impl KernelCommand {
    pub fn id(&self) -> u8 {
        self.id
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
}

#[derive(Debug, Default, Clone, Copy)]
pub struct MemReq {
    pub addr: u32,
    pub write: bool,
    pub data: u32,
}

#[derive(Debug, Default, Clone, Copy)]
pub struct MemResp {
    pub data: u32,
}

#[derive(Debug, Default, Clone, Copy)]
pub struct Event {
    bytes: [u8; 16],

    done: bool,
}
