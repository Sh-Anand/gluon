#ifndef RAD_DEFS_H
#define RAD_DEFS_H

#include <cstddef>
#include <cstdint>

constexpr std::size_t RAD_GPU_DRAM_SIZE = static_cast<std::size_t>(512) * 1024 * 1024;
constexpr std::size_t RAD_KERNEL_HEADER_BYTES = 40;
constexpr std::uint8_t RAD_KERNEL_REGS_PER_THREAD = 1;
constexpr std::uint32_t RAD_KERNEL_SMEM_PER_BLOCK = 1;
constexpr std::uint8_t RAD_KERNEL_FLAGS = 0;
constexpr std::uint32_t RAD_KERNEL_PRINTF_HOST_ADDR = 0;
constexpr std::uint16_t RAD_KERNEL_RESERVED_U16 = 0;

#endif
