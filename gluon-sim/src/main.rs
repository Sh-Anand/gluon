use std::env;
use std::error::Error;
use std::fs;
use std::io;
use std::path::Path;
use std::sync::Arc;

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

async fn enqueue_command(
    mut stream: OwnedReadHalf,
    addr: SocketAddr,
    top: Arc<Mutex<Top>>,
) -> tokio::io::Result<()> {
    loop {
        let mut buffer = [0u8; 24];
        match stream.read_exact(&mut buffer).await {
            Ok(_) => {}
            Err(_) => {
                println!("Client closed connection: {addr:?}");
                return Ok(());
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
) -> tokio::io::Result<()> {
    loop {
        if let Some(event) = {
            let mut top_guard = top.lock().await;
            top_guard.get_completion()
        } {
            stream.write_all(event.bytes.as_slice()).await?;
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

            let mut pid_bytes = [0u8; 8];
            stream.read_exact(&mut pid_bytes).await?;
            top_config.glug.host_pid = i32::from_le_bytes(pid_bytes[0..4].try_into().unwrap());
            top_config.glug.driver_pid = i32::from_le_bytes(pid_bytes[4..8].try_into().unwrap());

            let top = Arc::new(Mutex::new(Top::new(&top_config)));

            let (read_half, write_half) = stream.into_split();

            env_logger::init();

            let h1 = tokio::task::spawn(enqueue_command(
                read_half,
                addr,
                Arc::clone(&top),
            ));
            let h2 = tokio::task::spawn(tick_sim(Arc::clone(&top)));
            let h3 = tokio::task::spawn(dequeue_completion(
                write_half,
                Arc::clone(&top),
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
