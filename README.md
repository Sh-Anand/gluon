# Gluon

Gluon is the command processor module of the radiance GPU. This repository contains the functional simulator, driver and runtime for the Gluon command processor. It is meant to work in tandem with Cyclotron, the SIMT core functional sim. For RTL and design spec see `ucb-bar/radiance` repo.

# Requirements

- Cyclotron: Clone the cyclotron repo

# Usage

Set the path to your cyclotron folder under `Cargo.toml` dependencies.

`make <test_name>` from the root automatically builds and runs the test + driver and the gluon rust server

`make` runs `hello` by default. `make list` prints all available tests.

`cargo build` and `cargo run` from `gluon-sim/` to run just the rust server.

# Adding tests

Let's say you want to add a test `hello`.
Create a dir with the name of your test under `sw/test/`. In this case it would be `sw/test/hello`.
In this dir create `<test_name>_host.cpp` and `<test_name>_kernel.cpp`. Example `hello_host.cpp` and `hello_kernel.cpp`.
Run `make list` to verify that make can see your test. If you added `hello` correctly, it should show print `hello` in the list.
See existing tests for the programming model. A detailed documentation of the programming model is under construction.

# Components

- `gluon-sim/`: the rust functional sim that listens over a unix socket handled async with tokio.

- `sw/driver/`: the software driver that JIT compiles the GPU kernels and sends commands over the same unix socket configured for the rust server.

- `sw/driver/test/`: example test programs

# Config

See `config.toml`, same config is reused by the driver using `toml.hpp` to parse. 

- `socket`: unix socket file
