#include "rad.h"
#include "driver.h"
#include "loader.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>

void write_u32_le(std::uint8_t* dst, std::uint32_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFF);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

struct BufferWriter {
    std::uint8_t* cursor;
    std::uint8_t* end;

    bool write_u32(std::uint32_t value) {
        if (!remaining(4))
            return false;
        write_u32_le(cursor, value);
        cursor += 4;
        return true;
    }

    bool write_u8(std::uint8_t value) {
        if (!remaining(1))
            return false;
        *cursor++ = value;
        return true;
    }

    bool write_block(const void* data, std::size_t size) {
        if (size == 0)
            return true;
        if (!remaining(size))
            return false;
        std::memcpy(cursor, data, size);
        cursor += size;
        return true;
    }

    bool write_zero(std::size_t size) {
        if (size == 0)
            return true;
        if (!remaining(size))
            return false;
        std::memset(cursor, 0, size);
        cursor += size;
        return true;
    }

    bool finished() const { return cursor == end; }

private:
    bool remaining(std::size_t size) const { return cursor + size <= end; }
};

class Command {
public:
    uint8_t cmd_id;
    radCmdType cmd_type;
    Command(radCmdType cmd_type) : cmd_type(cmd_type) {}
};

class KernelCommand : public Command {
public:
    KernelCommand(KernelBinary kernel_binary, uint32_t gpu_kernel_base) : Command(radCmdType_KERNEL), kernel_binary(kernel_binary), gpu_kernel_base(gpu_kernel_base) {}
    KernelBinary kernel_binary;
    uint32_t gpu_kernel_base;
};

class CopyCommand : public Command {
public:
    CopyCommand(uint32_t src_addr, uint32_t dst_addr, uint32_t size, void *userspace_dst_addr, radMemCpyDir dir) :
        Command(radCmdType_MEM), src_addr(src_addr), dst_addr(dst_addr), size(size), userspace_dst_addr(userspace_dst_addr), dir(dir) {}
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t size;
    void *userspace_dst_addr;
    radMemCpyDir dir;
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

static CommandStream command_stream;

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
    
    // Allocate space for header + padding up to code base + kernel binary (skip ELF header)
    size_t header_to_code = KERNEL_LOAD_ADDR - KERNEL_HEADER_START_ADDR;
    size_t code_pad = header_to_code - KERNEL_HEADER_BYTES;
    if (params_size > code_pad) {
        fprintf(stderr, "radKernelLaunch: params too large for header padding\n");
        return;
    }

    uint32_t gpu_mem_kernel_bin_start = KERNEL_HEADER_START_ADDR;

    uint32_t gpu_mem_start_pc = KERNEL_LOAD_ADDR + kernel_binary->start_pc;
    uint32_t gpu_mem_kernel_pc = gpu_mem_start_pc;

    size_t payload_size = header_to_code + kernel_binary->size;
    
    if (payload_size == 0) {
        fprintf(stderr, "radKernelLaunch: empty payload size\n");
        return;
    }
    std::unique_ptr<std::uint8_t[]> payload(new (std::nothrow) std::uint8_t[payload_size]);

    // allocate stack space in GPU mem
    auto stack_base_addr_opt = allocateDeviceMemory(KERNEL_STACK_SIZE);
    if (!stack_base_addr_opt) {
        fprintf(stderr, "radKernelLaunch: failed to allocate stack space on gpu\n");
        return;
    }
    uint32_t stack_base_addr = *stack_base_addr_opt + KERNEL_STACK_SIZE - 4;

    // allocate tls space
    auto tls_base_addr_opt = allocateDeviceMemory(KERNEL_TLS_SIZE);
    if (!tls_base_addr_opt) {
        fprintf(stderr, "radKernelLaunch: failed to allocate tls space on gpu\n");
        return;
    }
    uint32_t tls_base_addr = *tls_base_addr_opt;

    uint32_t params_base_addr = gpu_mem_kernel_bin_start + KERNEL_HEADER_BYTES;

    // Write header, params, padding, and kernel binary into payload
    BufferWriter writer{payload.get(), payload.get() + payload_size};
    if (!writer.write_u32(gpu_mem_start_pc) ||
        !writer.write_u32(gpu_mem_kernel_pc) ||
        !writer.write_u32(static_cast<std::uint32_t>(params_size)) ||
        !writer.write_u32(static_cast<std::uint32_t>(kernel_binary->size)) ||
        !writer.write_u32(stack_base_addr) ||
        !writer.write_u32(tls_base_addr) ||
        !writer.write_u32(params_base_addr) ||
        !writer.write_u32(static_cast<std::uint32_t>(grid_dim.x)) ||
        !writer.write_u32(static_cast<std::uint32_t>(grid_dim.y)) ||
        !writer.write_u32(static_cast<std::uint32_t>(grid_dim.z)) ||
        !writer.write_u32(static_cast<std::uint32_t>(block_dim.x)) ||
        !writer.write_u32(static_cast<std::uint32_t>(block_dim.y)) ||
        !writer.write_u32(static_cast<std::uint32_t>(block_dim.z)) ||
        !writer.write_u32(KERNEL_PRINTF_HOST_ADDR) ||
        !writer.write_u8(KERNEL_REGS_PER_THREAD) ||
        !writer.write_u32(KERNEL_SMEM_PER_BLOCK) ||
        !writer.write_u8(KERNEL_FLAGS) ||
        !writer.write_zero(KERNEL_HEADER_MEM_PADDING) ||
        !writer.write_block(params_data, params_size) ||
        !writer.write_zero(code_pad - params_size) ||
        !writer.write_block(kernel_binary->data, kernel_binary->size) ||
        !writer.finished()) {
        fprintf(stderr, "radKernelLaunch: failed to populate payload\n");
        return;
    }
    
    if (payload_size > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: payload too large\n");
        return;
    }

    uint8_t cmd_id = command_stream.add_command(std::make_unique<KernelCommand>(*kernel_binary, gpu_mem_kernel_bin_start));

    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = cmd_id;
    header_bytes[1] = radCmdType_KERNEL;
    write_u32_le(header_bytes.data() + 2, 0);
    write_u32_le(header_bytes.data() + 6, static_cast<std::uint32_t>(payload_size));
    write_u32_le(header_bytes.data() + 10, gpu_mem_kernel_bin_start);
    auto response = rad::SubmitCommand(header_bytes, payload.get(), payload_size);
    if (!response)
        fprintf(stderr, "radKernelLaunch: failed to submit kernel launch\n");
}

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir) {
    fprintf(stderr, "radMemCpy: dst=%p, src=%p, bytes=%zu, dir=%d\n", dst, src, bytes, dir);
    if (dst == nullptr || src == nullptr)
        return;
    
    uint32_t src_addr_u32 = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(src));
    uint32_t dst_addr_u32 = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(dst));
    uint32_t size_u32 = static_cast<uint32_t>(bytes);

    void *src_addr, *dst_addr, *payload_addr;
    void *userspace_dst_addr;
    size_t payload_size;
    if (dir == radMemCpyDir_H2D) {
        src_addr = 0;
        dst_addr = dst;
        payload_addr = src;
        payload_size = bytes;
        userspace_dst_addr = 0;
    } else {
        src_addr = src;
        dst_addr = 0;
        payload_addr = nullptr;
        payload_size = 0;
        userspace_dst_addr = dst;
    }

    uint8_t cmd_id = command_stream.add_command(std::make_unique<CopyCommand>(src_addr_u32, dst_addr_u32, size_u32, userspace_dst_addr, dir));
    
    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = cmd_id;
    header_bytes[1] = radCmdType_MEM;
    header_bytes[2] = radMemCmdType_COPY;
    write_u32_le(header_bytes.data() + 3, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(src_addr)));
    write_u32_le(header_bytes.data() + 7, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(dst_addr)));
    write_u32_le(header_bytes.data() + 11, static_cast<std::uint32_t>(bytes));
    header_bytes[15] = dir;
    auto response = rad::SubmitCommand(header_bytes, payload_addr, payload_size);
    if (!response)
        fprintf(stderr, "radMemCpy: failed to submit mem copy\n");
}

void radMalloc(void **ptr, size_t bytes) {
    if (ptr == nullptr)
        return;
    auto device_addr = allocateDeviceMemory(bytes);
    if (!device_addr) {
        fprintf(stderr, "radMalloc: failed to allocate device memory\n");
        *ptr = nullptr;
        return;
    }
    std::uintptr_t value = static_cast<std::uintptr_t>(*device_addr);
    *ptr = reinterpret_cast<void *>(value);
}

// Massive hack, memcpy to userspace destination is handled here
void radGetError(radError *err) {
    if (err == nullptr)
        return;
    auto response = rad::ReceiveError();
    if (!response)
        fprintf(stderr, "radGetError: failed to receive error\n");
    if (response) {
        uint8_t response_cmd_id = response->at(0);
        Command* command = command_stream.ack_command(response_cmd_id);
        if (command) {
            err->cmd_id = command->cmd_id;
        } else {
            fprintf(stderr, "radGetError: command not found in stream\n");
        }
        err->err_code = static_cast<radErrorCode>(response->at(1));

        uint32_t pc = static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(2))) |
            (static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(3))) << 8) |
            (static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(4))) << 16) |
            (static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(5))) << 24);

        if (command->cmd_type == radCmdType_KERNEL) {
            KernelCommand* kernel_command = static_cast<KernelCommand*>(command);
            uint32_t translated_pc = 1; // TODO: implement this
            pc = translated_pc;
        }

        if (command->cmd_type == radCmdType_MEM) {
            CopyCommand* copy_command = static_cast<CopyCommand*>(command);
            if (copy_command->dir == radMemCpyDir_D2H) {
                void *shared_mem_base = rad::GetSharedMemoryBase();
                if (!shared_mem_base) {
                    fprintf(stderr, "radGetError: shared memory not initialized\n");
                    return;
                }
                void *src_addr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shared_mem_base));
                memcpy(copy_command->userspace_dst_addr, src_addr, copy_command->size);
            }
        }

        err->pc = pc;

        return;
    }
}
