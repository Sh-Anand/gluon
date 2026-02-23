#ifndef CORE_HPP
#define CORE_HPP

#include "rad.h"
#include "rad_rpc.hpp"

#include <cstddef>
#include <cstdint>

void KernelLaunch(KernelLaunchReq* req, const char* kernel_name, const uint8_t* params_data);

void MemCpy(MemCpyReq* req, const void* h2d_data);

uint32_t GPUMalloc(uint32_t bytes);

uint64_t CreateStream();

uint64_t EventRecord(radStream_t stream);

void WaitEvent(WaitEventReq* req);

bool GetError();

void Synchronize(SyncReq* req);

#endif
