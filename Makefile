CARGO ?= cargo
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS ?= -I sw/driver -I sw/driver/include

DEFAULT_TEST := hello
TEST ?= $(DEFAULT_TEST)

RUST_MANIFEST := gluon-sim/Cargo.toml
DRIVER_BIN := sw/test/build/$(TEST)/$(TEST)

TEST_DIRS := $(wildcard sw/test/*/)
TESTS := $(filter-out sw/test/build/,$(TEST_DIRS))
TEST_NAMES := $(patsubst %/,%,$(patsubst sw/test/%,%,$(TESTS)))

.PHONY: run server driver clean list $(TEST_NAMES)

run: server driver
	@set -euo pipefail; \
	RUST_LOG=debug $(CARGO) run --manifest-path $(RUST_MANIFEST) & \
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
	@$(MAKE) -C sw/test TEST=$(TEST) CPPFLAGS=

clean:
	@$(CARGO) clean --manifest-path $(RUST_MANIFEST)
	@rm -rf build sw/driver/gluon-sim/build gluon-sim.sock
	@$(MAKE) -C sw/test clean

list:
	@printf "%s\n" $(TEST_NAMES)

$(TEST_NAMES):
	@set -euo pipefail; \
	$(MAKE) -C sw/test TEST=$@ CPPFLAGS=; \
	$(CARGO) build --manifest-path $(RUST_MANIFEST); \
	RUST_LOG=debug $(CARGO) run --manifest-path $(RUST_MANIFEST) & \
	SERVER_PID=$$!; \
	trap 'kill $$SERVER_PID 2>/dev/null || true' EXIT; \
	sleep 1; \
	if ! kill -0 $$SERVER_PID 2>/dev/null; then \
		wait $$SERVER_PID || true; \
		exit 0; \
	fi; \
	$(MAKE) -C sw/test TEST=$@ run
