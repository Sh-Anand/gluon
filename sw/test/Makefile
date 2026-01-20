MUON_CLANG ?= $(MUON_32)/bin/clang
MUON_OBJCOPY ?= $(MUON_32)/bin/llvm-objcopy
MUON_OBJDUMP ?= $(MUON_32)/bin/llvm-objdump
HOST_CLANG ?= clang++

RISCV_TRIPLE := riscv32-unknown-elf
RISCV_SYSROOT := $(MUON_32)/$(RISCV_TRIPLE)
RISCV_FLAGS := --target=$(RISCV_TRIPLE) -march=rv32im_zfinx -mabi=ilp32 --sysroot=$(RISCV_SYSROOT)
CLANG_RESOURCE_DIR := $(shell $(MUON_CLANG) --target=$(RISCV_TRIPLE) -print-resource-dir)

TEST ?= hello
TEST_DIR := $(TEST)
TEST_NAME := $(notdir $(TEST_DIR))
SRC := $(wildcard $(TEST_DIR)/*.cpp)
HOST_SRCS := $(filter-out %_kernel.cpp,$(SRC))
KERNEL_SRC := $(firstword $(filter %_kernel.cpp,$(SRC)))
$(if $(KERNEL_SRC),,$(error No kernel source found for $(TEST_DIR)))
$(if $(HOST_SRCS),,$(error No host sources found for $(TEST_DIR)))

BUILD_DIR := build
KERNEL_OBJ := $(BUILD_DIR)/kernel.o
HOST_BIN := $(BUILD_DIR)/$(TEST_NAME)
HOST_BIN_ABS := $(abspath $(HOST_BIN))
REPO_ROOT := $(abspath ../..)

KERNEL_CFLAGS := $(RISCV_FLAGS) -I$(REPO_ROOT)/sw/driver/include -I$(REPO_ROOT)/sw/libmuon/gluon/include -O2 -ffunction-sections -fdata-sections -fPIC -Wall -Wextra
KERNEL_LDFLAGS := $(RISCV_FLAGS) -fuse-ld=lld -nostdlib -Wl,-melf32lriscv -Wl,--no-relax -Wl,-Ttext=0x10001000 -Wl,-N \
    -L$(MUON_32)/lib/$(RISCV_TRIPLE) -L$(CLANG_RESOURCE_DIR)/lib/$(RISCV_TRIPLE)
HOST_INCLUDES := -I../driver/utils/ -I../driver/include -I../libmuon/gluon/include
LIBMUON_DIR := $(REPO_ROOT)/sw/libmuon/gluon
LIBMUON_A := $(LIBMUON_DIR)/libmuon.a
KERNEL_LIBS := $(LIBMUON_A) -lc -lm $(CLANG_RT_LIB)

.PHONY: all run clean

all: $(HOST_BIN)

run: $(HOST_BIN)
	cd $(REPO_ROOT) && $(HOST_BIN_ABS)

$(BUILD_DIR):
	mkdir -p $@

$(KERNEL_OBJ): $(KERNEL_SRC) | $(BUILD_DIR)
	$(MUON_CLANG) $(KERNEL_CFLAGS) -c $< -o $@

$(HOST_BIN): $(HOST_SRCS) $(KERNEL_OBJ) ../driver/src/rad.cpp ../driver/gluon-sim/src/driver.cpp | $(BUILD_DIR)
	$(HOST_CLANG) -std=c++17 -Wall -Wextra $(HOST_INCLUDES) \
		$(HOST_SRCS) ../driver/src/rad.cpp ../driver/gluon-sim/src/driver.cpp -o $@

$(LIBMUON_A):
	$(MAKE) -C $(LIBMUON_DIR)

clean:
	rm -rf $(BUILD_ROOT)
	$(MAKE) -C $(LIBMUON_DIR) clean
