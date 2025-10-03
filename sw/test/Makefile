MUON_CLANG ?= $(MUON_32)/bin/clang
HOST_CLANG ?= clang++
LLVM_CONFIG ?= $(MUON_32)/bin/llvm-config
OBJCOPY ?= $(MUON_32)/bin/llvm-objcopy

TEST ?= hello
TEST_DIR := $(TEST)
TEST_NAME := $(notdir $(TEST_DIR))
SRC := $(wildcard $(TEST_DIR)/*.cpp)
HOST_SRCS := $(filter-out %_kernel.cpp,$(SRC))
KERNEL_SRC := $(firstword $(filter %_kernel.cpp,$(SRC)))
$(if $(KERNEL_SRC),,$(error No kernel source found for $(TEST_DIR)))
$(if $(HOST_SRCS),,$(error No host sources found for $(TEST_DIR)))
BUILD_ROOT := build
BUILD_DIR := $(BUILD_ROOT)/$(TEST_NAME)
DEVICE_BC := $(BUILD_ROOT)/$(TEST_NAME)_kernel.bc
DEVICE_OBJ := $(BUILD_ROOT)/$(TEST_NAME)_kernel_bc.o
HOST_BIN := $(BUILD_DIR)/$(TEST_NAME)
HOST_BIN_ABS := $(abspath $(HOST_BIN))
REPO_ROOT := $(abspath ../..)

LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS := $(shell $(LLVM_CONFIG) --libs core analysis bitreader passes target native object riscv --system-libs)
LLVM_INCLUDE_DIR := $(MUON_32)/include
.PHONY: all run clean

all: $(HOST_BIN)

run: $(HOST_BIN)
	cd $(REPO_ROOT) && $(HOST_BIN_ABS)

$(BUILD_ROOT):
	@if [ -e $(BUILD_ROOT) ] && [ ! -d $(BUILD_ROOT) ]; then \
		rm -f $(BUILD_ROOT); \
	fi
	mkdir -p $(BUILD_ROOT)

$(BUILD_DIR): | $(BUILD_ROOT)
	@if [ -e $(BUILD_DIR) ] && [ ! -d $(BUILD_DIR) ]; then \
		rm -f $(BUILD_DIR); \
	fi
	mkdir -p $(BUILD_DIR)

$(DEVICE_BC): $(KERNEL_SRC) | $(BUILD_ROOT)
	$(MUON_CLANG) \
		--target=riscv32-unknown-elf \
		-march=rv32im_zfinx -mabi=ilp32 \
		--sysroot=$(MUON_32)/riscv32-unknown-elf \
		-I$(MUON_32)/include \
		-I../driver/include \
		-fPIC \
		-O2 \
		-Xclang -target-feature -Xclang +vortex \
		-emit-llvm -c $< -o $@

$(DEVICE_OBJ): $(DEVICE_BC)
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 \
		--binary-architecture=x86-64 \
		--redefine-sym _binary_build_$(TEST_NAME)_kernel_bc_start=__gluon_kernel_start \
		--redefine-sym _binary_build_$(TEST_NAME)_kernel_bc_end=__gluon_kernel_end \
		--redefine-sym _binary_build_$(TEST_NAME)_kernel_bc_size=__gluon_kernel_size \
		$< $@

$(HOST_BIN): $(HOST_SRCS) $(DEVICE_OBJ) ../driver/src/rad.cpp ../driver/gluon-sim/src/driver.cpp | $(BUILD_DIR)
	$(HOST_CLANG) -std=c++17 -I../driver -I../driver/include -I../driver/gluon-sim/include \
		-idirafter $(LLVM_INCLUDE_DIR) \
		-D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
		$(HOST_SRCS) ../driver/src/rad.cpp ../driver/gluon-sim/src/driver.cpp $(DEVICE_OBJ) -o $@ \
		-fuse-ld=lld \
		$(LLVM_LDFLAGS) $(LLVM_LIBS) -l:liblldELF.so -l:liblldCommon.so -Wl,-rpath,$(MUON_32)/lib

clean:
	rm -rf $(BUILD_ROOT)
