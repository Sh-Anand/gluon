use std::error::Error;
use std::fs;
use std::path::Path;

use serde::Deserialize;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{unix::SocketAddr, UnixListener, UnixStream};

const CONFIG_PATH: &str = "config.toml";

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
    let config = load_config()?;
    let socket_path = config.server.socket_path;

    if Path::new(&socket_path).exists() {
        fs::remove_file(&socket_path)?;
    }

    let listener = UnixListener::bind(&socket_path)?;
    println!("Server listening on {socket_path}");

    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                tokio::spawn(async move {
                    if let Err(err) = handle_client(stream, addr).await {
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

fn load_config() -> Result<Config, Box<dyn Error>> {
    let contents = fs::read_to_string(CONFIG_PATH)?;
    let config = toml::from_str(&contents)?;
    Ok(config)
}

async fn handle_client(mut stream: UnixStream, addr: SocketAddr) -> tokio::io::Result<()> {
    println!("Client connected: {addr:?}");

    let mut buffer = [0_u8; 1024];
    let bytes_read = stream.read(&mut buffer).await?;
    if bytes_read == 0 {
        println!("Client closed connection without sending data");
        return Ok(());
    }

    let request = String::from_utf8_lossy(&buffer[..bytes_read]);
    println!("Received request: {}", request.trim_end());

    let response = b"dummy-response";
    stream.write_all(response).await?;
    stream.shutdown().await?;
    println!("Sent response: dummy-response");

    Ok(())
}
