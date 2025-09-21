CARGO ?= cargo
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS ?= -I driver

RUST_MANIFEST := gluon-sim/Cargo.toml
DRIVER_SRC := driver/gluon-sim/main.cpp
DRIVER_BIN := driver/gluon-sim/build/gluon-driver

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

$(DRIVER_BIN): $(DRIVER_SRC)
	@mkdir -p $(dir $(DRIVER_BIN))
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

driver: $(DRIVER_BIN)

clean:
	@$(CARGO) clean --manifest-path $(RUST_MANIFEST)
	@rm -rf $(dir $(DRIVER_BIN))
