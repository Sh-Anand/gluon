#ifndef DRIVER_H
#define DRIVER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rad {

std::optional<std::string> SubmitCommand(const std::vector<std::uint8_t>& header,
                                         const void* payload,
                                         std::size_t payload_size);

std::optional<std::string> ReceiveError();

void* GetSharedMemoryBase();
void* GetLastSharedMemoryBase();
void ReleaseSharedMemoryBase(void* addr);

}

#endif
