#ifndef RAD_DRIVER_H
#define RAD_DRIVER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rad {

struct KernelLaunchHeader {
    std::uint8_t command_id;
    std::uint32_t host_offset;
    std::uint32_t payload_size;
    std::uint32_t gpu_addr;
};

bool InitConnection(std::size_t shared_mem_bytes = 1 << 20,
                    std::uint32_t gpu_addr = 0x8000);
void ShutdownConnection();
bool IsConnectionReady();
std::uint32_t GetGpuAddress();

std::optional<std::string> SubmitKernelLaunch(const KernelLaunchHeader& header,
                                              const std::vector<std::uint8_t>& payload);

}

#endif
