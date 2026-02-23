use std::env;
use std::error::Error;
use std::fs;
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::Path;
use std::sync::Arc;
use std::collections::VecDeque;

use gluon::common::base::Configurable;
use gluon::common::base::{Clocked, Command};
use serde::Deserialize;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::unix::OwnedReadHalf;
use tokio::net::unix::OwnedWriteHalf;
use tokio::net::{unix::SocketAddr, UnixListener};
use tokio::sync::Mutex;
use gluon::glug::dispatch::DispatchConfig;
use gluon::glug::engine::EngineConfig;
use gluon::glug::engines::cs_engine::CSEngineConfig;
use gluon::glug::engines::kernel_engine::KernelEngineConfig;
use gluon::glug::engines::mem_engine::MemEngineConfig;
use gluon::glug::intake::IntakeConfig;
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
    intake: IntakeConfig,
    #[serde(default)]
    dispatch: DispatchConfig,
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

        glug_config.intake = self.intake;
        glug_config.dispatch = self.dispatch;
        glug_config.engine = engine_config;

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

fn recv_command(socket_fd: RawFd) -> io::Result<([u8; 24], Option<(OwnedFd, usize)>)> {
    const CMSG_BUFFER_LEN: usize =
        unsafe { libc::CMSG_SPACE(std::mem::size_of::<RawFd>() as u32) as usize };

    let mut data_buf = [0u8; 24];
    let mut cmsg_buffer = [0u8; CMSG_BUFFER_LEN];
    let mut iov = libc::iovec {
        iov_base: data_buf.as_mut_ptr().cast(),
        iov_len: data_buf.len(),
    };
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer.as_mut_ptr().cast();
    msg.msg_controllen = cmsg_buffer.len();

    let received = unsafe { libc::recvmsg(socket_fd, &mut msg, libc::MSG_WAITALL) };
    if received < 0 {
        return Err(io::Error::last_os_error());
    }
    if received == 0 {
        return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "socket closed"));
    }

    let mut fd_base = None;
    let mut cmsg = unsafe { libc::CMSG_FIRSTHDR(&msg) };
    while !cmsg.is_null() {
        let hdr = unsafe { &*cmsg };
        if hdr.cmsg_level == libc::SOL_SOCKET && hdr.cmsg_type == libc::SCM_RIGHTS {
            let data = unsafe { libc::CMSG_DATA(cmsg) as *const RawFd };
            if !data.is_null() {
                let fd = unsafe { *data };
                let base_u64 = match data_buf[1] {
                    0 => u64::from_le_bytes(data_buf[2..10].try_into().unwrap()),
                    1 => {
                        if data_buf[23] == 0 {
                            u64::from_le_bytes(data_buf[3..11].try_into().unwrap())
                        } else {
                            u64::from_le_bytes(data_buf[11..19].try_into().unwrap())
                        }
                    }
                    _ => 0,
                };
                fd_base = Some((unsafe { OwnedFd::from_raw_fd(fd) }, base_u64 as usize));
            }
        }
        cmsg = unsafe { libc::CMSG_NXTHDR(&msg, cmsg) };
    }

    Ok((data_buf, fd_base))
}

async fn enqueue_command(
    stream: OwnedReadHalf,
    addr: SocketAddr,
    top: Arc<Mutex<Top>>,
    active_regions: Arc<Mutex<Vec<VecDeque<SharedMemoryRegion>>>>,
) -> tokio::io::Result<()> {
    loop {
        stream.readable().await?;
        let (buffer, fd_base) = match recv_command(stream.as_ref().as_raw_fd()) {
            Ok(v) => v,
            Err(err) if err.kind() == io::ErrorKind::WouldBlock || err.kind() == io::ErrorKind::Interrupted => {
                continue;
            }
            Err(_) => {
                println!("Client closed connection: {addr:?}");
                return Ok(());
            }
        };
        if let Some((fd, base)) = fd_base {
            let region = SharedMemoryRegion::from_owned_fd(fd, base)?;
            let sid = buffer[0] as usize;
            let mut guard = active_regions.lock().await;
            if sid < guard.len() {
                guard[sid].push_back(region);
            }
        }
        let command = Command::from_bytes(buffer);
        {
            let mut top_guard = top.lock().await;
            top_guard.submit_command(command);
            top_guard.tick().unwrap();
        }
        tokio::task::yield_now().await;
    }
}

async fn tick_sim(top: Arc<Mutex<Top>>) -> tokio::io::Result<()> {
    loop {
        let mut top_guard = top.lock().await;
        top_guard.tick().unwrap();
    }
}

async fn dequeue_completion(
    mut stream: OwnedWriteHalf,
    top: Arc<Mutex<Top>>,
    active_regions: Arc<Mutex<Vec<VecDeque<SharedMemoryRegion>>>>,
) -> tokio::io::Result<()> {
    loop {
        if let Some(event) = {
            let mut top_guard = top.lock().await;
            top_guard.get_completion()
        } {
            stream.write_all(event.bytes.as_slice()).await?;
            let sid = event.sid() as usize;
            let mut guard = active_regions.lock().await;
            if sid < guard.len() {
                let _ = guard[sid].pop_front();
            }
            println!("Sent completion: {:?}", event);
        }
        tokio::task::yield_now().await;
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let config_path = env::args()
        .nth(1)
        .unwrap_or_else(|| DEFAULT_CONFIG_PATH.to_string());
    let config = load_config(&config_path)?;
    let (server_config, mut top_config) = config.into_server_and_top();
    let socket_path = server_config.socket_path;

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

    match listener.accept().await {
        Ok((mut stream, addr)) => {
            println!("Client connected: {addr:?}");

            let mut host_pid_bytes = [0u8; 4];
            stream.read_exact(&mut host_pid_bytes).await?;
            top_config.glug.host_pid = i32::from_le_bytes(host_pid_bytes);

            let top = Arc::new(Mutex::new(Top::new(&top_config)));
            let active_regions = Arc::new(Mutex::new((0..256).map(|_| VecDeque::new()).collect::<Vec<_>>()));

            let (read_half, write_half) = stream.into_split();

            env_logger::init();

            let h1 = tokio::task::spawn(enqueue_command(
                read_half,
                addr,
                Arc::clone(&top),
                Arc::clone(&active_regions),
            ));
            let h2 = tokio::task::spawn(tick_sim(Arc::clone(&top)));
            let h3 = tokio::task::spawn(dequeue_completion(
                write_half,
                Arc::clone(&top),
                Arc::clone(&active_regions),
            ));
            let _ = tokio::join!(h1, h2, h3);
        }
        Err(err) => {
            eprintln!("Failed to accept connection: {err}");
        }
    }

    if Path::new(&socket_path).exists() {
        let _ = fs::remove_file(&socket_path);
    }

    Ok(())
}
