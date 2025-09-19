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

pub trait Clocked {
    fn tick(&mut self);
    fn busy(&mut self) -> bool;
}

pub trait Configurable<T: Default> {
    fn instantiate(config: T) -> Self;
}

pub trait Module: Clocked {}

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
        let mut bytes = [0u8; 15];
        bytes.copy_from_slice(&self.bytes[1..]);
        EngineCommand { bytes }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct EngineCommand {
    bytes: [u8; 15],
}

impl EngineCommand {
    pub fn id(&self) -> u8 {
        self.bytes[0]
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct Event {
    bytes: [u8; 16],

    done: bool,
}
