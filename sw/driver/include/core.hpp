#ifndef CORE_HPP
#define CORE_HPP

#include "rad.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace core {

struct KernelLaunchReq {
    radStream_t stream;
    radDim3 grid_dim;
    radDim3 block_dim;
    const char* kernel_name;
    const uint8_t* params_data;
    uint32_t params_size;
};

struct MemCpyReq {
    radStream_t stream;
    uint32_t dst_addr;
    uint32_t src_addr;
    uint32_t bytes;
    radMemCpyDir dir;
    const uint8_t* h2d_data;
};

struct WaitEventReq {
    radStream_t stream;
    uint8_t hw_sid;
    uint64_t cmd_id;
};

struct CompletionResult {
    uint64_t stream;
    radErrorCode err_code;
    uint32_t pc;
    std::vector<uint8_t> d2h_bytes;
};

struct SyncReq {
    uint64_t stream;
    uint64_t cmd_id;
};

void KernelLaunch(const KernelLaunchReq& req);

void MemCpy(const MemCpyReq& req);

uint32_t Malloc(uint64_t bytes);

uint64_t CreateStream();

radEvent_t EventRecord(radStream_t stream);

void WaitEvent(const WaitEventReq& req);

bool GetError(CompletionResult* out);

void Synchronize(const SyncReq& req);

} // namespace core

#endif
