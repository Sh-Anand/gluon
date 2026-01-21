#ifndef MEM_HPP
#define MEM_HPP

#include "rad_defs.h"

#include <cstdint>
#include <optional>

static std::uint64_t g_device_mem_used = GPU_MEM_START_ADDR;

std::optional<uint32_t> allocateDeviceMemory(size_t bytes) {
    static const std::uint64_t capacity = static_cast<std::uint64_t>(GPU_DRAM_SIZE);
    size_t aligned_bytes = bytes + (bytes % sizeof(uint32_t));
    if (g_device_mem_used > capacity)
        return std::nullopt;
    if (bytes > capacity - g_device_mem_used)
        return std::nullopt;
    uint32_t addr = static_cast<uint32_t>(g_device_mem_used);
    g_device_mem_used += aligned_bytes;
    return addr;
}

#endif