MUON_CLANG ?= $(MUON_32)/bin/clang
HOST_CLANG ?= clang++
LLVM_CONFIG ?= $(MUON_32)/bin/llvm-config
OBJCOPY ?= $(MUON_32)/bin/llvm-objcopy

SRC := hello.cpp
BUILD_DIR := build
DEVICE_BC := $(BUILD_DIR)/hello_kernel.bc
DEVICE_OBJ := $(BUILD_DIR)/hello_kernel_bc.o
HOST_BIN := $(BUILD_DIR)/hello

LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS := $(shell $(LLVM_CONFIG) --libs core analysis bitreader passes target native object riscv --system-libs)
LLVM_INCLUDE_DIR := $(MUON_32)/include
.PHONY: all clean

all: $(HOST_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DEVICE_BC): $(SRC) | $(BUILD_DIR)
	$(MUON_CLANG) \
		--target=riscv32-unknown-elf \
		-march=rv32im_zfinx -mabi=ilp32 \
		--sysroot=$(MUON_32)/riscv32-unknown-elf \
		-I$(MUON_32)/include \
		-I../driver/include \
		-DRAD_TARGET \
		-O2 \
		-Xclang -target-feature -Xclang +vortex \
		-emit-llvm -c $< -o $@

$(DEVICE_OBJ): $(DEVICE_BC)
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 \
		--binary-architecture=x86-64 $< $@


$(HOST_BIN): $(SRC) $(DEVICE_OBJ) ../driver/src/rad.cpp ../driver/src/driver.cpp | $(BUILD_DIR)
	$(HOST_CLANG) -std=c++17 -DRAD_HOST -I../driver -I../driver/include \
		-idirafter $(LLVM_INCLUDE_DIR) \
		-D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
		$(SRC) ../driver/src/rad.cpp ../driver/src/driver.cpp $(DEVICE_OBJ) -o $@ \
		-fuse-ld=lld \
		$(LLVM_LDFLAGS) $(LLVM_LIBS) -l:liblldELF.so -l:liblldCommon.so -Wl,-rpath,$(MUON_32)/lib

clean:
	rm -rf $(BUILD_DIR)
