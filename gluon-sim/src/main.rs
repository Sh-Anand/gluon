use std::convert::TryInto;
use std::env;
use std::error::Error;
use std::fs;
use std::io;
use std::io::ErrorKind;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::Path;
use std::sync::Arc;

use gluon::common::base::Clocked;
use serde::Deserialize;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{unix::SocketAddr, UnixListener, UnixStream};
use tokio::sync::Mutex;

use gluon::common::base::Command;
use gluon::glug::completion::CompletionConfig;
use gluon::glug::decode_dispatch::DecodeDispatchConfig;
use gluon::glug::engine::EngineConfig;
use gluon::glug::engines::cs_engine::CSEngineConfig;
use gluon::glug::engines::kernel_engine::KernelEngineConfig;
use gluon::glug::engines::mem_engine::MemEngineConfig;
use gluon::glug::frontend::FrontendConfig;
use gluon::glug::glug::GLUGConfig;
use gluon::top::{SimConfig, Top, TopConfig};

mod shared_memory;
use shared_memory::SharedMemoryRegion;

const DEFAULT_CONFIG_PATH: &str = "config.toml";

#[derive(Deserialize)]
struct Config {
    server: ServerConfig,
    #[serde(default)]
    sim: SimConfig,
    #[serde(default)]
    glug: GLUGConfig,
    #[serde(default)]
    frontend: FrontendConfig,
    #[serde(default, rename = "decode_dispatch")]
    decode_dispatch: DecodeDispatchConfig,
    #[serde(default)]
    completion: CompletionConfig,
    #[serde(default)]
    engine: EngineConfig,
    #[serde(default, rename = "kernel_engine")]
    kernel_engine: KernelEngineConfig,
    #[serde(default, rename = "mem_engine")]
    mem_engine: MemEngineConfig,
    #[serde(default, rename = "cs_engine")]
    cs_engine: CSEngineConfig,
}

#[derive(Deserialize)]
struct ServerConfig {
    socket_path: String,
}

impl Config {
    fn into_server_and_top(self) -> (ServerConfig, TopConfig) {
        let mut glug_config = self.glug;
        let mut engine_config = self.engine;
        engine_config.kernel_engine_config = self.kernel_engine;
        engine_config.mem_engine_config = self.mem_engine;
        engine_config.cs_engine_config = self.cs_engine;

        glug_config.frontend = self.frontend;
        glug_config.decode_dispatch = self.decode_dispatch;
        glug_config.engine = engine_config;
        glug_config.completion = self.completion;

        (
            self.server,
            TopConfig {
                sim: self.sim,
                glug: glug_config,
            },
        )
    }
}

fn load_config(path: &str) -> Result<Config, Box<dyn Error>> {
    let contents = fs::read_to_string(path)?;
    let config = toml::from_str(&contents)?;
    Ok(config)
}

async fn receive_shared_memory_region(stream: &UnixStream) -> io::Result<SharedMemoryRegion> {
    loop {
        stream.readable().await?;
        match recv_memfd(stream.as_raw_fd()) {
            Ok(fd) => return SharedMemoryRegion::from_owned_fd(fd),
            Err(err) if err.kind() == ErrorKind::WouldBlock => continue,
            Err(err) => return Err(err),
        }
    }
}

fn recv_memfd(socket_fd: RawFd) -> io::Result<OwnedFd> {
    const CMSG_BUFFER_LEN: usize =
        unsafe { libc::CMSG_SPACE(std::mem::size_of::<RawFd>() as u32) as usize };

    let mut data_buf = [0u8; 1];
    let mut cmsg_buffer = [0u8; CMSG_BUFFER_LEN];

    loop {
        let mut iov = libc::iovec {
            iov_base: data_buf.as_mut_ptr().cast(),
            iov_len: data_buf.len(),
        };

        let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buffer.as_mut_ptr().cast();
        msg.msg_controllen = cmsg_buffer.len();

        let received = unsafe { libc::recvmsg(socket_fd, &mut msg, 0) };
        if received < 0 {
            let err = io::Error::last_os_error();
            if err.kind() == ErrorKind::WouldBlock || err.kind() == ErrorKind::Interrupted {
                continue;
            }
            return Err(err);
        }

        if received == 0 {
            return Err(io::Error::new(
                ErrorKind::UnexpectedEof,
                "client closed connection before sending shared memory fd",
            ));
        }

        let mut cmsg = unsafe { libc::CMSG_FIRSTHDR(&msg) };
        while !cmsg.is_null() {
            let hdr = unsafe { &*cmsg };
            if hdr.cmsg_level == libc::SOL_SOCKET && hdr.cmsg_type == libc::SCM_RIGHTS {
                let data = unsafe { libc::CMSG_DATA(cmsg) as *const RawFd };
                if !data.is_null() {
                    let fd = unsafe { *data };
                    return Ok(unsafe { OwnedFd::from_raw_fd(fd) });
                }
            }

            cmsg = unsafe { libc::CMSG_NXTHDR(&msg, cmsg) };
        }

        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "did not receive a file descriptor in ancillary data",
        ));
    }
}

async fn handle_client(
    mut stream: UnixStream,
    addr: SocketAddr,
    top: Arc<Mutex<Top>>,
) -> tokio::io::Result<()> {
    println!("Client connected: {addr:?}");

    let shared_memory = receive_shared_memory_region(&stream).await?;

    loop {
        let mut buffer = [0_u8; 16];
        match stream.read_exact(&mut buffer).await {
            Ok(_) => {
                let mut raw_cmd = buffer;

                if raw_cmd[0] == 0 {
                    let host_offset =
                        raw_cmd[2..6]
                            .try_into()
                            .map(u32::from_le_bytes)
                            .map_err(|_| {
                                io::Error::new(
                                    ErrorKind::InvalidData,
                                    "kernel command missing host address",
                                )
                            })?;

                    let payload_size =
                        raw_cmd[6..10]
                            .try_into()
                            .map(u32::from_le_bytes)
                            .map_err(|_| {
                                io::Error::new(
                                    ErrorKind::InvalidData,
                                    "kernel command missing payload size",
                                )
                            })?;

                    let mapped_addr = shared_memory
                        .pointer_value(host_offset, payload_size)
                        .map_err(|err| {
                            io::Error::new(
                                ErrorKind::InvalidData,
                                format!("failed to obtain shared memory pointer value: {err}"),
                            )
                        })?;

                    raw_cmd[2..6].copy_from_slice(&mapped_addr.to_le_bytes());
                }

                let command = Command::from_bytes(raw_cmd);

                let sim_err = {
                    let mut top_guard = top.lock().await;
                    top_guard.submit_command(command);
                    top_guard.tick()
                };

                sim_err.expect("Sim exit");
            }
            Err(err) if err.kind() == io::ErrorKind::UnexpectedEof => {
                println!("Client closed connection: {addr:?}");
                break;
            }
            Err(err) => return Err(err),
        }
    }

    stream.shutdown().await?;

    Ok(())
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let config_path = env::args()
        .nth(1)
        .unwrap_or_else(|| DEFAULT_CONFIG_PATH.to_string());
    let config = load_config(&config_path)?;
    let (server_config, top_config) = config.into_server_and_top();
    let socket_path = server_config.socket_path;

    let top = Arc::new(Mutex::new(Top::new(top_config)));

    if Path::new(&socket_path).exists() {
        fs::remove_file(&socket_path)?;
    }

    let listener = match UnixListener::bind(&socket_path) {
        Ok(listener) => listener,
        Err(err) if err.kind() == io::ErrorKind::PermissionDenied => {
            eprintln!(
                "Permission denied while binding Unix socket at {socket_path}; skipping server startup."
            );
            return Ok(());
        }
        Err(err) => return Err(err.into()),
    };
    println!("Server listening on {socket_path}");

    let top_for_listener = Arc::clone(&top);
    let server_task = tokio::spawn(async move {
        loop {
            match listener.accept().await {
                Ok((stream, addr)) => {
                    let top_for_client = Arc::clone(&top_for_listener);
                    if let Err(err) = handle_client(stream, addr, top_for_client).await {
                        eprintln!("Error handling client: {err}");
                    }
                }
                Err(err) => {
                    eprintln!("Failed to accept connection: {err}");
                }
            }
        }
    });

    let top_for_tick = Arc::clone(&top);
    let tick_task = tokio::task::spawn_blocking(move || loop {
        let (sim_err, cycles) = {
            let mut top_guard = top_for_tick.blocking_lock();
            let state = top_guard.tick();
            let cycles = top_guard.cycles_elapsed();
            (state, cycles)
        };

        if sim_err.is_err() {
            break (sim_err, cycles);
        }
    });

    let (sim_err, cycles) = tick_task.await?;

    match sim_err {
        Err(gluon::common::base::SimErr::TIMEOUT) => print!("Timeout {cycles} cycles"),
        _ => sim_err.expect("Sim exit"),
    }

    server_task.abort();
    let _ = server_task.await;
    if Path::new(&socket_path).exists() {
        let _ = fs::remove_file(&socket_path);
    }

    Ok(())
}
