#ifndef COMMAND_HPP
#define COMMAND_HPP

#include <cstddef>
#include <cstdint>

enum radCmdType {
    radCmdType_KERNEL,
    radCmdType_MEM,
    radCmdType_CSR,
    radCmdType_WAIT,
    radCmdType_UNDEFINED,
};

enum radMemCmdType {
    radMemCmdType_COPY,
    radMemCmdType_SET,
};

#endif // COMMAND_HPP
