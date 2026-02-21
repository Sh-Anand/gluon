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

class Command {
public:
    radCmdType cmd_type;
    Command(radCmdType cmd_type) : cmd_type(cmd_type) {}
};

class KernelCommand : public Command {
public:
    KernelCommand(uint8_t* binary_data, size_t size, uint32_t gpu_kernel_base) : Command(radCmdType_KERNEL), binary_data(binary_data), size(size), gpu_kernel_base(gpu_kernel_base) {}
    uint8_t* binary_data;
    size_t size;
    uint32_t gpu_kernel_base;
};

class CopyCommand : public Command {
public:
    CopyCommand(uint32_t src_addr, uint32_t dst_addr, uint32_t size, void *userspace_dst_addr, bool d2h) :
        Command(radCmdType_MEM), src_addr(src_addr), dst_addr(dst_addr), size(size), userspace_dst_addr(userspace_dst_addr), d2h(d2h), shared_addr(nullptr) {}
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t size;
    void *userspace_dst_addr;
    bool d2h;
    void *shared_addr;
};

#endif // COMMAND_HPP
