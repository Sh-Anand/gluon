#ifndef DRIVER_H
#define DRIVER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rad {

std::optional<std::string> SubmitCommand(const std::array<std::uint8_t, 16>& header,
                                         const std::vector<std::uint8_t>& payload);

}

#endif
