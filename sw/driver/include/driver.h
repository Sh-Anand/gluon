#ifndef DRIVER_H
#define DRIVER_H

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

std::optional<std::string> SubmitKernelLaunch(const KernelLaunchHeader& header,
                                              const std::vector<std::uint8_t>& payload);

}

#endif
