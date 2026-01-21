#ifndef COMMAND_HPP
#define COMMAND_HPP

#include <cstdint>

#include "rad_defs.h"
#include "loader.hpp"

enum radCmdType {
    radCmdType_KERNEL,
    radCmdType_MEM,
    radCmdType_CSR,
    radCmdType_FENCE,
    radCmdType_UNDEFINED,
};

enum radMemCmdType {
    radMemCmdType_COPY,
    radMemCmdType_SET,
};

class Command {
public:
    uint8_t cmd_id;
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
        Command(radCmdType_MEM), src_addr(src_addr), dst_addr(dst_addr), size(size), userspace_dst_addr(userspace_dst_addr), d2h(d2h) {}
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t size;
    void *userspace_dst_addr;
    bool d2h;
};

class CommandStream {
public:
    uint8_t next_cmd_id;
    std::vector<std::unique_ptr<Command>> commands;

    CommandStream() : next_cmd_id(0) {
        fprintf(stderr, "CommandStream: initialized\n");
    }

    uint8_t add_command(std::unique_ptr<Command> command) {
        command->cmd_id = next_cmd_id++;
        uint8_t cmd_id = command->cmd_id;
        commands.push_back(std::move(command));
        return cmd_id;
    }

    Command* ack_command(uint8_t cmd_id) {
        for (auto& command : commands) {
            if (command->cmd_id == cmd_id)
                return command.get();
        }
        return nullptr;
    }
    
};

#endif // COMMAND_HPP