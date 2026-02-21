#include "rad.h"
#include "driver.h"
#include "loader.hpp"
#include "mem.hpp"
#include "command.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

struct radStream {
    uint8_t hw_sid;
    std::vector<std::unique_ptr<Command>> commands;
    radStream(uint8_t hw_sid) : hw_sid(hw_sid), commands() {}
};

// massive hack until we move this all to driver
static std::vector<radStream> streams = [] {
    std::vector<radStream> v;
    v.emplace_back(0);
    return v;
}();
static std::vector<uint64_t> hw_stream_cmd_ids = std::vector<uint64_t>(HW_STREAM_COUNT, 0);
static uint8_t curr_hw_stream = 1;

void write_u32_le(std::uint8_t* dst, std::uint32_t value) {
    for (int i = 0; i < 4; i++) {
        dst[i] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
}

void write_u64_le(std::uint8_t* dst, std::uint64_t value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
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

void radKernelLaunch(const char *kernel_name,
                                 radDim3 grid_dim,
                                 radDim3 block_dim,
                                 radParamBuf* params,
                                 radStream_t stream) {
    ELFLoader *loader = new ELFLoader("sw/test/build/kernel.elf");
 
    std::size_t params_size = 0;
    const uint8_t* params_data = nullptr;
    if (params) {
        params_size = params->size();
        if (params_size > 0)
            params_data = params->data();
    }

    size_t payload_size = KERNEL_HEADER_MEM_END + params_size + loader->size;
    auto kernel_payload_addr_opt = allocateDeviceMemory(payload_size);
    assert(kernel_payload_addr_opt);
    uint32_t kernel_payload_addr = *kernel_payload_addr_opt;
    uint32_t kernel_reloc_addr = kernel_payload_addr + KERNEL_HEADER_MEM_END + params_size;
    
    // apply relocations
    loader->applyRelocations(kernel_reloc_addr);
    uint32_t start_pc = loader->getSymbolAddress("_start", kernel_reloc_addr);
    uint32_t kernel_pc = loader->getSymbolAddress(kernel_name, kernel_reloc_addr);

    // allocate stack space in GPU mem
    auto stack_base_addr_opt = allocateDeviceMemory(KERNEL_STACK_SIZE);
    assert(stack_base_addr_opt);
    uint32_t stack_base_addr = *stack_base_addr_opt + KERNEL_STACK_SIZE - 4;

    // allocate tls space
    auto tls_base_addr_opt = allocateDeviceMemory(KERNEL_TLS_SIZE);
    assert(tls_base_addr_opt);
    uint32_t tls_base_addr = *tls_base_addr_opt;

    std::unique_ptr<std::uint8_t[]> payload(new (std::nothrow) std::uint8_t[payload_size]);
    BufferWriter writer{payload.get(), payload.get() + payload_size};
    if (!writer.write_u32(start_pc) ||
        !writer.write_u32(kernel_pc) ||
        !writer.write_u32(static_cast<std::uint32_t>(params_size)) ||
        !writer.write_u32(static_cast<std::uint32_t>(loader->size)) ||
        !writer.write_u32(stack_base_addr) ||
        !writer.write_u32(tls_base_addr) ||
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
        !writer.write_block(loader->binary_data, loader->size) ||
        !writer.finished()) {
        fprintf(stderr, "radKernelLaunch: failed to populate payload\n");
        return;
    }
    
    if (payload_size > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: payload too large\n");
        return;
    }

    streams[stream].commands.push_back(std::make_unique<KernelCommand>(loader->binary_data, loader->size, kernel_reloc_addr));
    hw_stream_cmd_ids[streams[stream].hw_sid]++;

    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = streams[stream].hw_sid;
    header_bytes[1] = radCmdType_KERNEL;
    write_u32_le(header_bytes.data() + 2, 0);
    write_u32_le(header_bytes.data() + 6, static_cast<std::uint32_t>(payload_size));
    write_u32_le(header_bytes.data() + 10, kernel_payload_addr);
    auto response = rad::SubmitCommand(header_bytes, payload.get(), payload_size);
    if (!response)
        fprintf(stderr, "radKernelLaunch: failed to submit kernel launch\n");
}

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir, radStream_t stream) {
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

    streams[stream].commands.push_back(std::make_unique<CopyCommand>(src_addr_u32, dst_addr_u32, size_u32, userspace_dst_addr, dir));
    hw_stream_cmd_ids[streams[stream].hw_sid]++;

    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = streams[stream].hw_sid;
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
void radGetError(radError *err, radStream_t stream) {
    if (err == nullptr)
        return;

    auto response = rad::ReceiveError();
    if (!response)
        fprintf(stderr, "radGetError: failed to receive error\n");
    if (response) {
        Command* command = streams[stream].commands.front().get();
        if (!command) {
            fprintf(stderr, "radGetError: command not found in stream\n");
            return;
        }
        err->err_code = static_cast<radErrorCode>(response->at(1));

        uint32_t pc = static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(2))) |
            (static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(3))) << 8) |
            (static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(4))) << 16) |
            (static_cast<uint32_t>(static_cast<std::uint8_t>(response->at(5))) << 24);

        if (command->cmd_type == radCmdType_KERNEL) {
            uint32_t translated_pc = 1; // TODO: implement this
            pc = translated_pc;
        }

        if (command->cmd_type == radCmdType_MEM) {
            CopyCommand* copy_command = static_cast<CopyCommand*>(command);
            if (copy_command->d2h) {
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

        streams[stream].commands.erase(streams[stream].commands.begin());
        return;
    }
}

void radCreateStream(radStream_t* stream) {
    streams.emplace_back(curr_hw_stream++);
    *stream = streams.size() - 1;
    curr_hw_stream %= HW_STREAM_COUNT;
}

void radEventRecord(radEvent_t* event, radStream_t stream) {
    event->hw_sid = streams[stream].hw_sid;
    event->cmd_id = hw_stream_cmd_ids[streams[stream].hw_sid];
}

void radWaitEvent(radEvent_t* event, radStream_t stream) {
    std::array<std::uint8_t, 16> header_bytes{};
    header_bytes[0] = streams[stream].hw_sid;
    header_bytes[1] = radCmdType_WAIT;
    header_bytes[2] = event->hw_sid;
    write_u64_le(header_bytes.data() + 3, event->cmd_id);
    auto response = rad::SubmitCommand(header_bytes, nullptr, 0);
    if (!response)
        fprintf(stderr, "radWaitEvent: failed to submit wait event\n");
}
