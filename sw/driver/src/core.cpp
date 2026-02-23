#include "core.hpp"

#include "command.hpp"
#include "driver.h"
#include "loader.hpp"
#include "mem.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <thread>

static std::vector<radStream> streams = {0};
static std::vector<HWStream> hw_streams = std::vector<HWStream>(HW_STREAM_COUNT, HWStream{0, 0});
static uint8_t curr_hw_stream = 1;
static std::mutex core_mu;

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

void KernelLaunch(KernelLaunchReq* req, const char* kernel_name, const uint8_t* params_data) {
    std::lock_guard<std::mutex> lock(core_mu);
    ELFLoader *loader = new ELFLoader("sw/test/build/kernel.elf");

    size_t payload_size = KERNEL_HEADER_MEM_END + req->params_size + loader->size;
    auto kernel_payload_addr_opt = allocateDeviceMemory(payload_size);
    if (!kernel_payload_addr_opt)
        return;
    uint32_t kernel_payload_addr = *kernel_payload_addr_opt;
    uint32_t kernel_reloc_addr = kernel_payload_addr + KERNEL_HEADER_MEM_END + req->params_size;

    loader->applyRelocations(kernel_reloc_addr);
    uint32_t start_pc = loader->getSymbolAddress("_start", kernel_reloc_addr);
    uint32_t kernel_pc = loader->getSymbolAddress(kernel_name, kernel_reloc_addr);

    auto stack_base_addr_opt = allocateDeviceMemory(KERNEL_STACK_SIZE);
    if (!stack_base_addr_opt)
        return;
    uint32_t stack_base_addr = *stack_base_addr_opt + KERNEL_STACK_SIZE - 4;

    auto tls_base_addr_opt = allocateDeviceMemory(KERNEL_TLS_SIZE);
    if (!tls_base_addr_opt)
        return;
    uint32_t tls_base_addr = *tls_base_addr_opt;

    std::unique_ptr<std::uint8_t[]> payload(new (std::nothrow) std::uint8_t[payload_size]);
    if (!payload)
        return;

    BufferWriter writer{payload.get(), payload.get() + payload_size};
    if (!writer.write_u32(start_pc) ||
        !writer.write_u32(kernel_pc) ||
        !writer.write_u32(req->params_size) ||
        !writer.write_u32(static_cast<std::uint32_t>(loader->size)) ||
        !writer.write_u32(stack_base_addr) ||
        !writer.write_u32(tls_base_addr) ||
        !writer.write_u32(static_cast<std::uint32_t>(req->grid_dim.x)) ||
        !writer.write_u32(static_cast<std::uint32_t>(req->grid_dim.y)) ||
        !writer.write_u32(static_cast<std::uint32_t>(req->grid_dim.z)) ||
        !writer.write_u32(static_cast<std::uint32_t>(req->block_dim.x)) ||
        !writer.write_u32(static_cast<std::uint32_t>(req->block_dim.y)) ||
        !writer.write_u32(static_cast<std::uint32_t>(req->block_dim.z)) ||
        !writer.write_u32(KERNEL_PRINTF_HOST_ADDR) ||
        !writer.write_u8(KERNEL_REGS_PER_THREAD) ||
        !writer.write_u32(KERNEL_SMEM_PER_BLOCK) ||
        !writer.write_u8(KERNEL_FLAGS) ||
        !writer.write_zero(KERNEL_HEADER_MEM_PADDING) ||
        !writer.write_block(params_data, req->params_size) ||
        !writer.write_block(loader->binary_data, loader->size) ||
        !writer.finished()) {
        return;
    }

    hw_streams[streams[req->stream]].tail_cmd_id++;

    std::vector<uint8_t> header_bytes(CMD_HEADER_SIZE, 0);
    header_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    header_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_KERNEL;
    write_u32_le(header_bytes.data() + 2, 0);
    write_u32_le(header_bytes.data() + 6, static_cast<std::uint32_t>(payload_size));
    write_u32_le(header_bytes.data() + 10, kernel_payload_addr);
    (void)rad::SubmitCommand(header_bytes, payload.get(), payload_size);
}

// TODO: memcpy way way too hacky
void MemCpy(MemCpyReq* req, void* h2d_data) {
    std::lock_guard<std::mutex> lock(core_mu);
    uint32_t src_addr = (uint32_t)(uintptr_t)(req->src_addr);
    uint32_t dst_addr = (uint32_t)(uintptr_t)(req->dst_addr);
    void* aux_ptr = (void*)(uintptr_t)(req->dst_addr);
    size_t aux_size = 0;
    if (req->dir == radMemCpyDir_H2D) {
        aux_ptr = h2d_data;
        aux_size = req->bytes;
    }
    hw_streams[streams[req->stream]].tail_cmd_id++;

    // hack to keep interface reusable
    std::vector<uint8_t> header_bytes(CMD_HEADER_SIZE, 0);
    header_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    header_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_MEM;
    header_bytes[CMD_MEM_CMD_TYPE_OFFSET] = radMemCmdType_COPY;
    write_u32_le(header_bytes.data() + 3, src_addr); // 4 bytes
    write_u32_le(header_bytes.data() + 3 + sizeof(uint32_t), dst_addr); // 4 bytes
    write_u32_le(header_bytes.data() + 3 + sizeof(uint32_t) + sizeof(uint32_t), req->bytes); // 4 bytes
    header_bytes[3 + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t)] = req->dir; // 1 byte
    (void)rad::SubmitCommand(header_bytes, aux_ptr, aux_size);
}

uint32_t GPUMalloc(uint32_t bytes) {
    std::lock_guard<std::mutex> lock(core_mu);
    auto device_addr = allocateDeviceMemory(bytes);
    return device_addr ? *device_addr : 0;
}

uint64_t CreateStream() {
    std::lock_guard<std::mutex> lock(core_mu);
    streams.emplace_back(curr_hw_stream++);
    uint64_t stream = streams.size() - 1;
    curr_hw_stream %= HW_STREAM_COUNT;
    return stream;
}

uint64_t EventRecord(radStream_t stream) {
    std::lock_guard<std::mutex> lock(core_mu);
    return hw_streams[streams[stream]].tail_cmd_id;
}

void WaitEvent(WaitEventReq* req) {
    std::lock_guard<std::mutex> lock(core_mu);
    std::vector<uint8_t> header_bytes(CMD_HEADER_SIZE, 0);
    header_bytes[0] = streams[req->stream];
    header_bytes[1] = radCmdType_WAIT;
    header_bytes[2] = streams[req->event.stream];
    write_u64_le(header_bytes.data() + 3, req->event.cmd_id);
    (void)rad::SubmitCommand(header_bytes, nullptr, 0);
}

bool GetError() {
    auto response = rad::ReceiveError();
    if (!response)
        return false;

    std::lock_guard<std::mutex> lock(core_mu);
    hw_streams[response->at(0)].head_cmd_id++;
    return true;
}

void Synchronize(SyncReq* req) {
    uint64_t wait_cmd;
    {
        std::lock_guard<std::mutex> lock(core_mu);
        if (req->cmd_id == 0) wait_cmd = hw_streams[streams[req->stream]].tail_cmd_id;
        else wait_cmd = req->cmd_id;
    }

    while (true) {
        {
            std::lock_guard<std::mutex> lock(core_mu);
            if (hw_streams[streams[req->stream]].head_cmd_id >= wait_cmd) break;
        }
        std::this_thread::yield();
    }
}
