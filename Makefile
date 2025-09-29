CARGO ?= cargo
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS ?= -I sw/driver -I sw/driver/include

RUST_MANIFEST := gluon-sim/Cargo.toml
DRIVER_BIN := sw/test/build/hello

.PHONY: run server driver clean

run: server driver
	@set -euo pipefail; \
	$(CARGO) run --manifest-path $(RUST_MANIFEST) & \
	SERVER_PID=$$!; \
	trap 'kill $$SERVER_PID 2>/dev/null || true' EXIT; \
	sleep 1; \
	if ! kill -0 $$SERVER_PID 2>/dev/null; then \
		wait $$SERVER_PID || true; \
		exit 0; \
	fi; \
	$(DRIVER_BIN)

server:
	@$(CARGO) build --manifest-path $(RUST_MANIFEST)

driver:
	@$(MAKE) -C sw/test

clean:
	@$(CARGO) clean --manifest-path $(RUST_MANIFEST)
	@$(MAKE) -C sw/test clean
