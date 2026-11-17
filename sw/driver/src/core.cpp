#include "core.h"

#include "driver.h"
#include "mem.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <thread>
#include <stdexcept>

static std::vector<radStream> streams = {0};
static std::vector<HWStream> hw_streams = std::vector<HWStream>(HW_STREAM_COUNT, HWStream{0, 0});
static std::vector<std::vector<void*>> host_ptrs = std::vector<std::vector<void*>>(HW_STREAM_COUNT);
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

void KernelLaunch(KernelLaunchReq* req, const uint8_t* params_data) {
    // the params block: the params' size, then the params
    size_t block_size = PARAMS_START + req->params_size;
    auto block_addr_opt = allocateDeviceMemory(block_size);
    if (!block_addr_opt)
        throw std::runtime_error("Failed to allocate device memory for kernel params");
    std::uint8_t* block = new std::uint8_t[block_size];
    write_u32_le(block + PARAMS_SIZE, req->params_size);
    std::memcpy(block + PARAMS_START, params_data, req->params_size);

    {
        std::lock_guard<std::mutex> lock(core_mu);
        hw_streams[streams[req->stream]].tail_cmd_id += 2;
    }

    // copy the block in from this process, then launch
    std::vector<uint8_t> copy_bytes(CMD_MEM_HEADER_SIZE, 0);
    copy_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    copy_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_MEM;
    copy_bytes[CMD_MEM_CMD_TYPE_OFFSET] = radMemCmdType_COPY;
    write_u64_le(copy_bytes.data() + CMD_MEM_SRC_ADDR_OFFSET, (uint64_t)(uintptr_t)(block));
    write_u64_le(copy_bytes.data() + CMD_MEM_DST_ADDR_OFFSET, *block_addr_opt);
    write_u32_le(copy_bytes.data() + CMD_MEM_LEN_OFFSET, (uint32_t)(block_size));
    copy_bytes[CMD_MEM_DIR_OFFSET] = radMemcpyDir_H2D | CMD_MEM_FROM_DRIVER;
    (void)driver::SubmitCommand(copy_bytes, block, block_size);
    host_ptrs[streams[req->stream]].push_back(block);

    std::vector<uint8_t> header_bytes(CMD_KERNEL_HEADER_SIZE, 0);
    header_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    header_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_KERNEL;
    std::uint8_t* entry = header_bytes.data() + CMD_KERNEL_ENTRY_OFFSET;
    write_u32_le(entry, *block_addr_opt);
    write_u32_le(entry + 4, req->grid_dim.x | req->grid_dim.y << 16);
    write_u32_le(entry + 8, req->grid_dim.z | (uint32_t)req->kernel_id << 16 | (req->block_dim.z - 1) << 24);
    write_u32_le(entry + 12, (req->block_dim.x - 1) | (req->block_dim.y - 1) << 10 | (req->smem_bytes + 15) / 16 << 20);
    (void)driver::SubmitCommand(header_bytes, nullptr, 0);
    host_ptrs[streams[req->stream]].push_back(nullptr);
}

// TODO: memcpy way way too hacky
void MemCpy(MemCpyReq* req) {
    uint64_t src_addr = (uint64_t)(uintptr_t)(req->src_addr);
    uint64_t dst_addr = (uint64_t)(uintptr_t)(req->dst_addr);
    void* aux_ptr = (void*)(uintptr_t)(req->dst_addr);
    size_t aux_size = 0;

    {
        std::lock_guard<std::mutex> lock(core_mu);
        hw_streams[streams[req->stream]].tail_cmd_id++;
    }

    // hack to keep interface reusable
    std::vector<uint8_t> header_bytes(CMD_MEM_HEADER_SIZE, 0);
    header_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    header_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_MEM;
    header_bytes[CMD_MEM_CMD_TYPE_OFFSET] = radMemCmdType_COPY;
    write_u64_le(header_bytes.data() + CMD_MEM_SRC_ADDR_OFFSET, src_addr); // 8 bytes
    write_u64_le(header_bytes.data() + CMD_MEM_DST_ADDR_OFFSET, dst_addr); // 8 bytes
    write_u32_le(header_bytes.data() + CMD_MEM_LEN_OFFSET, req->bytes); // 4 bytes
    header_bytes[CMD_MEM_DIR_OFFSET] = req->dir; // 1 byte
    (void)driver::SubmitCommand(header_bytes, aux_ptr, aux_size);
    host_ptrs[streams[req->stream]].push_back(nullptr);
}

// set one field of a kernel table entry
void TableWrite(TableWriteReq* req) {
    {
        std::lock_guard<std::mutex> lock(core_mu);
        hw_streams[streams[req->stream]].tail_cmd_id++;
    }

    std::vector<uint8_t> header_bytes(CMD_CSR_HEADER_SIZE, 0);
    header_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    header_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_CSR;
    header_bytes[CMD_CSR_ID_OFFSET] = req->id;
    header_bytes[CMD_CSR_FIELD_OFFSET] = req->field;
    write_u32_le(header_bytes.data() + CMD_CSR_VALUE_OFFSET, req->value);
    (void)driver::SubmitCommand(header_bytes, nullptr, 0);
    host_ptrs[streams[req->stream]].push_back(nullptr);
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
    std::vector<uint8_t> header_bytes(CMD_WAIT_HEADER_SIZE, 0);
    header_bytes[CMD_STREAM_ID_OFFSET] = streams[req->stream];
    header_bytes[CMD_CMD_TYPE_OFFSET] = radCmdType_WAIT;
    header_bytes[CMD_WAIT_EVENT_STREAM_ID_OFFSET] = streams[req->event.stream];
    write_u64_le(header_bytes.data() + CMD_WAIT_EVENT_CMD_ID_OFFSET, req->event.cmd_id);
    (void)driver::SubmitCommand(header_bytes, nullptr, 0);
}

bool GetError() {
    auto response = driver::ReceiveError();
    if (!response)
        return false;

    std::lock_guard<std::mutex> lock(core_mu);
    uint8_t sid = response->at(0);
    hw_streams[sid].head_cmd_id++;
    if (!host_ptrs[sid].empty()) {
        void* host_ptr = host_ptrs[sid].front();
        host_ptrs[sid].erase(host_ptrs[sid].begin());
        delete[] (uint8_t*)host_ptr;
    }
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
