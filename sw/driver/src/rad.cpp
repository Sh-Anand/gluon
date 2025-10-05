#include "rad.h"
#include "driver.h"

#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <sstream>

#include <elfio/elfio.hpp>
#include <iomanip>

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

    rad::KernelLaunchHeader header{};
    header.command_id = 0;
    header.host_offset = 0;
    header.payload_size = static_cast<uint32_t>(payload_size);
    auto device_addr = allocateDeviceMemory(payload_size);
    if (!device_addr) {
        fprintf(stderr, "radKernelLaunch: failed to allocate device memory\n");
        return;
    }
    header.gpu_addr = *device_addr;

    std::vector<std::uint8_t> payload;
    payload.reserve(payload_size);
    const auto push_u32 = [&payload](std::uint32_t value) {
        payload.push_back(static_cast<std::uint8_t>(value & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto push_u16 = [&payload](std::uint16_t value) {
        payload.push_back(static_cast<std::uint8_t>(value & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };

    size_t gpu_mem_kernel_bin_start = header.gpu_addr + KERNEL_HEADER_BYTES + params_size;
    uint32_t gpu_mem_start_pc = gpu_mem_kernel_bin_start + kernel_binary->start_pc;
    uint32_t gpu_mem_kernel_pc = gpu_mem_kernel_bin_start + kernel_binary->kernel_pc;

    push_u32(gpu_mem_start_pc);
    push_u32(gpu_mem_kernel_pc);
    push_u32(static_cast<std::uint32_t>(params_size));
    push_u32(static_cast<std::uint32_t>(kernel_binary->size));
    push_u16(static_cast<std::uint16_t>(grid_dim.x));
    push_u16(static_cast<std::uint16_t>(grid_dim.y));
    push_u16(static_cast<std::uint16_t>(grid_dim.z));
    push_u16(static_cast<std::uint16_t>(block_dim.x));
    push_u16(static_cast<std::uint16_t>(block_dim.y));
    push_u16(static_cast<std::uint16_t>(block_dim.z));
    payload.push_back(KERNEL_REGS_PER_THREAD);
    push_u32(KERNEL_SMEM_PER_BLOCK);
    payload.push_back(KERNEL_FLAGS);
    push_u32(KERNEL_PRINTF_HOST_ADDR);
    push_u16(KERNEL_RESERVED_U16);
    
    if (params_size > 0)
        payload.insert(payload.end(), params_data, params_data + params_size);
    for (size_t i = 0; i < param_padding; ++i)
        payload.push_back(0);

    payload.insert(payload.end(), kernel_binary->data, kernel_binary->data + kernel_binary->size);
    for (size_t i = 0; i < kernel_bin_padding; ++i)
        payload.push_back(0);
    if (payload.size() > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: payload too large\n");
        return;
    }
    auto response = rad::SubmitKernelLaunch(header, payload);
    if (!response)
        fprintf(stderr, "radKernelLaunch: failed to submit kernel launch\n");
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
