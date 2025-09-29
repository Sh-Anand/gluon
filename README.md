# Gluon

Gluon is the command processor module of the radiance GPU. This repository contains the functional simulator, driver and runtime for the Gluon command processor. It is meant to work in tandem with Cyclotron, the SIMT core functional sim. For RTL and design spec see `ucb-bar/radiance` repo.

# Requirements

- Cyclotron: Clone the cyclotron repo

# Usage

Set the path to your cyclotron folder under `Cargo.toml` dependencies.

`make` from the root automatically builds and runs both the driver and the gluon rust server

`cargo build` and `cargo run` from `gluon-sim/` to run just the rust server.

# Components

- `gluon-sim/`: the rust functional sim that listens over a unix socket handled async with tokio.

- `sw/driver/`: the software driver that JIT compiles the GPU kernels and sends commands over the same unix socket configured for the rust server.

- `sw/driver/test/`: example test programs

# Config

See `config.toml`, same config is reused by the driver using `toml.hpp` to parse. 

- `socket`: unix socket file
