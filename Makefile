CARGO ?= cargo
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS ?= -I sw/driver -I sw/driver/include

DEFAULT_TEST := hello
TEST ?= $(DEFAULT_TEST)

RUST_MANIFEST := gluon-sim/Cargo.toml
DRIVER_BIN := sw/test/build/$(TEST)/$(TEST)
RAD_DRIVER_BIN := sw/driver/gluon-sim/build/rad-driver-daemon

RUST_LOG ?=

TEST_DIRS := $(wildcard sw/test/*/)
TESTS := $(filter-out sw/test/build/,$(TEST_DIRS))
TEST_NAMES := $(patsubst %/,%,$(patsubst sw/test/%,%,$(TESTS)))

.PHONY: run server driver rad_driver clean list pathtracer $(TEST_NAMES)

run: server driver rad_driver
	@set -euo pipefail; \
	RUST_LOG=$(RUST_LOG) $(CARGO) run --manifest-path $(RUST_MANIFEST) & \
	SERVER_PID=$$!; \
	$(RAD_DRIVER_BIN) & \
	DRIVER_PID=$$!; \
	trap 'kill $$SERVER_PID $$DRIVER_PID 2>/dev/null || true' EXIT; \
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

rad_driver:
	@mkdir -p sw/driver/gluon-sim/build
	@$(CXX) -std=c++17 -Wall -Wextra -I sw/driver/include -I sw/driver/utils -I sw/libmuon/gluon/include \
		sw/driver/src/core.cpp \
		sw/driver/gluon-sim/src/driver.cpp \
		-o $(RAD_DRIVER_BIN)

clean:
	@$(CARGO) clean --manifest-path $(RUST_MANIFEST)
	@rm -rf build sw/driver/gluon-sim/build gluon-sim.sock rad-driver.sock
	@$(MAKE) -C sw/test clean

list:
	@printf "%s\n" $(TEST_NAMES)

# start the sim and the driver daemon in the background, killed on exit
START_SIM = \
		$(MAKE) rad_driver; \
		$(CARGO) build --manifest-path $(RUST_MANIFEST); \
		RUST_LOG=$(RUST_LOG) $(CARGO) run --manifest-path $(RUST_MANIFEST) & \
		SERVER_PID=$$!; \
		$(RAD_DRIVER_BIN) & \
		DRIVER_PID=$$!; \
		trap 'kill $$SERVER_PID $$DRIVER_PID 2>/dev/null || true' EXIT; \
		sleep 1; \
		if ! kill -0 $$SERVER_PID 2>/dev/null; then \
			wait $$SERVER_PID || true; \
			exit 0; \
		fi

$(TEST_NAMES):
	@set -euo pipefail; \
		$(MAKE) -C sw/test TEST=$@ CPPFLAGS=; \
		$(START_SIM); \
	$(MAKE) -C sw/test TEST=$@ run

# pathtracer workload: built by its gluon target, renders to PT_OUT
PATHTRACER := sw/workloads/pathtracer
PT_SCENE ?= $(PATHTRACER)/gltf/bunny.glb
PT_ARGS ?= -r 16 16
PT_OUT ?= pathtracer.pfm

pathtracer:
	@set -euo pipefail; \
		$(MAKE) -C $(PATHTRACER) target=gluon; \
		$(START_SIM); \
	RAD_KERNEL_ELF=$(PATHTRACER)/build/gluon/kernel.elf \
		$(PATHTRACER)/build/gluon/host/pathtracer_gluon $(PT_ARGS) -f $(PT_OUT) $(PT_SCENE)
