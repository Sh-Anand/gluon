#include "rad.h"
#include "driver.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>

#include <elfio/elfio.hpp>
#include <iomanip>

namespace {

void write_u32_le(std::uint8_t* dst, std::uint32_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFF);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void write_u16_le(std::uint8_t* dst, std::uint16_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFF);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

struct BufferWriter {
    std::uint8_t* cursor;
    std::uint8_t* end;

    bool write_u32(std::uint32_t value) {
        if (!remaining(4))
            return false;
        write_u32_le(cursor, value);
        cursor += 4;
        return true;
    }

    bool write_u16(std::uint16_t value) {
        if (!remaining(2))
            return false;
        write_u16_le(cursor, value);
        cursor += 2;
        return true;
    }

    bool write_u8(std::uint8_t value) {
        if (!remaining(1))
            return false;
        *cursor++ = value;
        return true;
    }

    bool write_block(const void* data, std::size_t size) {
        if (size == 0)
            return true;
        if (!remaining(size))
            return false;
        std::memcpy(cursor, data, size);
        cursor += size;
        return true;
    }

    bool write_zero(std::size_t size) {
        if (size == 0)
            return true;
        if (!remaining(size))
            return false;
        std::memset(cursor, 0, size);
        cursor += size;
        return true;
    }

    bool finished() const { return cursor == end; }

private:
    bool remaining(std::size_t size) const { return cursor + size <= end; }
};

}  // namespace

using namespace ELFIO;

extern "C" const unsigned char __gluon_kernel_start[];
extern "C" const unsigned char __gluon_kernel_end[];

struct KernelBinary {
    uint32_t start_pc = 0;
    uint32_t kernel_pc = 0;
    const uint8_t* data;
    size_t size = 0;
};

std::optional<KernelBinary> loadKernelBinary(const std::string& kernel_name) {
    if (kernel_name.empty())
        return std::nullopt;
    const auto* start = reinterpret_cast<const uint8_t*>(__gluon_kernel_start);
    const auto* end = reinterpret_cast<const uint8_t*>(__gluon_kernel_end);
    if (!start || !end || end <= start) {
        fprintf(stderr, "radKernelLaunch: missing kernel image\n");
        return std::nullopt;
    }
    size_t size = static_cast<size_t>(end - start);

    std::string kernal_bin = std::string(start, end);
    std::istringstream elf_stream(kernal_bin, std::ios::binary);
    
    elfio elf;
    if (!elf.load(elf_stream)) {
        fprintf(stderr, "radKernelLaunch: failed to load kernel image\n");
        return std::nullopt;
    }

    uint32_t start_vaddr = 0;
    uint32_t kernel_vaddr = 0;
    for (const auto& sec : elf.sections) {
        if (sec->get_type() == SHT_SYMTAB || sec->get_type() == SHT_DYNSYM) {
            ELFIO::symbol_section_accessor syms(elf, sec.get());
            for (unsigned i = 0; i < syms.get_symbols_num(); ++i) {
                std::string name;
                ELFIO::Elf64_Addr value;
                ELFIO::Elf_Xword size;
                unsigned char bind, type, other;
                ELFIO::Elf_Half sec_idx;

                syms.get_symbol(i, name, value, size, bind, type, sec_idx, other);
                if (name == "_start") {
                    start_vaddr = value;
                    if (kernel_vaddr)
                        break;
                } else if (name == kernel_name) {
                    kernel_vaddr = value;
                    if (start_vaddr)
                        break;
                }
            }
            if (start_vaddr && kernel_vaddr)
                break;
        }
    }


    uint32_t start_offset = 0;
    uint32_t kernel_offset = 0;
    for (const auto& seg : elf.segments) {
        if (seg->get_type() != PT_LOAD)
            continue;
        auto vaddr = seg->get_virtual_address();
        auto filesz = seg->get_file_size();
        if (start_vaddr >= vaddr && start_vaddr < vaddr + filesz)
            start_offset = static_cast<uint32_t>(start_vaddr - vaddr + seg->get_offset());
        if (kernel_vaddr >= vaddr && kernel_vaddr < vaddr + filesz)
            kernel_offset = static_cast<uint32_t>(kernel_vaddr - vaddr + seg->get_offset());
        if (start_offset && kernel_offset)
            break;
    }

    if (!start_offset && !kernel_offset) {
        fprintf(stderr, "radKernelLaunch: failed to find start or kernel offset\n");
        return std::nullopt;
    }
    fprintf(stderr, "radKernelLaunch: found start offset: %u, kernel offset: %u\n", start_offset, kernel_offset);
    return KernelBinary{start_offset, kernel_offset, start, size};
}

std::optional<uint32_t> allocateDeviceMemory(size_t bytes) {
    static std::uint64_t used = 0;
    static const std::uint64_t capacity = static_cast<std::uint64_t>(GPU_DRAM_SIZE);
    if (used > capacity)
        return std::nullopt;
    if (bytes > capacity - used)
        return std::nullopt;
    uint32_t addr = static_cast<uint32_t>(used);
    used += bytes;
    return addr;
}

void radKernelLaunch(const char *kernel_name,
                                 radDim3 grid_dim,
                                 radDim3 block_dim,
                                 radParamBuf* params) {
    if (!kernel_name)
        return;
    auto kernel_binary = loadKernelBinary(kernel_name);
    if (!kernel_binary)
        return;
    if (grid_dim.x > UINT16_MAX || grid_dim.y > UINT16_MAX || grid_dim.z > UINT16_MAX) {
        fprintf(stderr, "radKernelLaunch: grid dimension exceeds limit\n");
        return;
    }
    if (block_dim.x > UINT16_MAX || block_dim.y > UINT16_MAX || block_dim.z > UINT16_MAX) {
        fprintf(stderr, "radKernelLaunch: block dimension exceeds limit\n");
        return;
    }
    if (kernel_binary->size > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: binary too large\n");
        return;
    }
    std::size_t params_size = 0;
    const uint8_t* params_data = nullptr;
    if (params) {
        params_size = params->size();
        if (params_size > 0)
            params_data = params->data();
    }
    if (params_size > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: parameter payload too large\n");
        return;
    }
    if (params_size > 0 && !params_data) {
        fprintf(stderr, "radKernelLaunch: parameter buffer missing data pointer\n");
        return;
    }
    size_t payload_size = KERNEL_HEADER_BYTES + params_size;
    uint32_t param_padding = (payload_size) & (sizeof(uint32_t) - 1);
    payload_size += param_padding;
    payload_size += kernel_binary->size;
    uint32_t kernel_bin_padding = (payload_size) & (sizeof(uint32_t) - 1);
    payload_size += kernel_bin_padding;

    auto device_addr = allocateDeviceMemory(payload_size);
    if (!device_addr) {
        fprintf(stderr, "radKernelLaunch: failed to allocate device memory\n");
        return;
    }
    uint32_t gpu_addr = *device_addr;

    size_t gpu_mem_kernel_bin_start = gpu_addr + KERNEL_HEADER_BYTES + params_size + param_padding;
    uint32_t gpu_mem_start_pc = gpu_mem_kernel_bin_start + kernel_binary->start_pc;
    uint32_t gpu_mem_kernel_pc = gpu_mem_kernel_bin_start + kernel_binary->kernel_pc;
    if (payload_size == 0) {
        fprintf(stderr, "radKernelLaunch: empty payload size\n");
        return;
    }
    std::unique_ptr<std::uint8_t[]> payload(new (std::nothrow) std::uint8_t[payload_size]);
    if (!payload) {
        fprintf(stderr, "radKernelLaunch: failed to allocate payload buffer\n");
        return;
    }
    BufferWriter writer{payload.get(), payload.get() + payload_size};
    if (!writer.write_u32(gpu_mem_start_pc) ||
        !writer.write_u32(gpu_mem_kernel_pc) ||
        !writer.write_u32(static_cast<std::uint32_t>(params_size + param_padding)) ||
        !writer.write_u32(static_cast<std::uint32_t>(kernel_binary->size)) ||
        !writer.write_u16(static_cast<std::uint16_t>(grid_dim.x)) ||
        !writer.write_u16(static_cast<std::uint16_t>(grid_dim.y)) ||
        !writer.write_u16(static_cast<std::uint16_t>(grid_dim.z)) ||
        !writer.write_u16(static_cast<std::uint16_t>(block_dim.x)) ||
        !writer.write_u16(static_cast<std::uint16_t>(block_dim.y)) ||
        !writer.write_u16(static_cast<std::uint16_t>(block_dim.z)) ||
        !writer.write_u8(KERNEL_REGS_PER_THREAD) ||
        !writer.write_u32(KERNEL_SMEM_PER_BLOCK) ||
        !writer.write_u8(KERNEL_FLAGS) ||
        !writer.write_u32(KERNEL_PRINTF_HOST_ADDR) ||
        !writer.write_u16(KERNEL_RESERVED_U16) ||
        !writer.write_block(params_data, params_size) ||
        !writer.write_zero(param_padding) ||
        !writer.write_block(kernel_binary->data, kernel_binary->size) ||
        !writer.write_zero(kernel_bin_padding) ||
        !writer.finished()) {
        fprintf(stderr, "radKernelLaunch: failed to populate payload\n");
        return;
    }
    if (payload_size > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: payload too large\n");
        return;
    }
    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = 0;
    header_bytes[1] = 0;
    write_u32_le(header_bytes.data() + 2, 0);
    write_u32_le(header_bytes.data() + 6, static_cast<std::uint32_t>(payload_size));
    write_u32_le(header_bytes.data() + 10, gpu_addr);
    auto response = rad::SubmitCommand(header_bytes, payload.get(), payload_size);
    if (!response)
        fprintf(stderr, "radKernelLaunch: failed to submit kernel launch\n");
}

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir) {
    if (!dst || !src)
        return;
    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = 0;
    header_bytes[1] = 0;
    write_u32_le(header_bytes.data() + 2, 0);
    write_u32_le(header_bytes.data() + 6, static_cast<std::uint32_t>(bytes));
    write_u32_le(header_bytes.data() + 10,
                 static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(dst)));
    write_u32_le(header_bytes.data() + 14,
                 static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(src)));
    auto response = rad::SubmitCommand(header_bytes, nullptr, 0);
    if (!response)
        fprintf(stderr, "radMemCpy: failed to submit mem copy\n");
}

void radMalloc(void **ptr, size_t bytes) {
    if (!ptr)
        return;
    auto device_addr = allocateDeviceMemory(bytes);
    if (!device_addr) {
        *ptr = nullptr;
        return;
    }
    std::uintptr_t value = static_cast<std::uintptr_t>(*device_addr);
    *ptr = reinterpret_cast<void *>(value);
}
