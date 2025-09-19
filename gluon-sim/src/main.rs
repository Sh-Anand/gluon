use std::env;
use std::error::Error;
use std::fs;
use std::io;
use std::path::Path;
use std::sync::Arc;

use serde::Deserialize;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{unix::SocketAddr, UnixListener, UnixStream};
use tokio::sync::Mutex;

use gluon::common::base::{Clocked, Command, Configurable};
use gluon::glug::glug::{GLUGConfig, GLUG};

const DEFAULT_CONFIG_PATH: &str = "config.toml";

#[derive(Deserialize)]
struct Config {
    server: ServerConfig,
}

#[derive(Deserialize)]
struct ServerConfig {
    socket_path: String,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let config_path = env::args().nth(1).unwrap_or_else(|| DEFAULT_CONFIG_PATH.to_string());
    let config = load_config(&config_path)?;
    let socket_path = config.server.socket_path;

    let glug = Arc::new(Mutex::new(GLUG::instantiate(GLUGConfig::default())));

    if Path::new(&socket_path).exists() {
        fs::remove_file(&socket_path)?;
    }

    let listener = UnixListener::bind(&socket_path)?;
    println!("Server listening on {socket_path}");

    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let glug = Arc::clone(&glug);
                tokio::spawn(async move {
                    if let Err(err) = handle_client(stream, addr, glug).await {
                        eprintln!("Error handling client: {err}");
                    }
                });
            }
            Err(err) => {
                eprintln!("Failed to accept connection: {err}");
            }
        }
    }
}

fn load_config(path: &str) -> Result<Config, Box<dyn Error>> {
    let contents = fs::read_to_string(path)?;
    let config = toml::from_str(&contents)?;
    Ok(config)
}

async fn handle_client(
    mut stream: UnixStream,
    addr: SocketAddr,
    glug: Arc<Mutex<GLUG>>,
) -> tokio::io::Result<()> {
    println!("Client connected: {addr:?}");

    loop {
        let mut buffer = [0_u8; 16];
        match stream.read_exact(&mut buffer).await {
            Ok(_) => {
                let command = Command::from_bytes(buffer);
                println!("Received command from {addr:?}: {:?}", command);
                schedule_command(Arc::clone(&glug), command);
                stream.write_all(b"ack").await?;
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

fn schedule_command(glug: Arc<Mutex<GLUG>>, command: Command) {
    tokio::spawn(async move {
        {
            let mut glug_guard = glug.lock().await;
            glug_guard.submit_command(command);
        }

        let glug_for_tick = Arc::clone(&glug);
        schedule_tick(glug_for_tick);
    });
}

fn schedule_tick(glug: Arc<Mutex<GLUG>>) {
    tokio::spawn(async move {
        let mut glug_guard = glug.lock().await;
        glug_guard.tick();
        let needs_more = glug_guard.busy();
        drop(glug_guard);

        if needs_more {
            schedule_tick(glug);
        }
    });
}
