#ifndef CORE_HPP
#define CORE_HPP

#include "rad.h"
#include "rad_rpc.hpp"

#include <cstdint>

typedef uint8_t radStream;

#define CMD_STREAM_ID_OFFSET 0
#define CMD_CMD_TYPE_OFFSET 1

// A kernel command carries the launch entry, four words:
//   0: address of the params block
//   1: grid x | grid y << 16
//   2: grid z | kernel id << 16 | (block z - 1) << 24
//   3: (block x - 1) | (block y - 1) << 10 | smem per block in 16-byte units << 20
#define CMD_KERNEL_ENTRY_OFFSET CMD_CMD_TYPE_OFFSET + 1
#define CMD_KERNEL_HEADER_SIZE CMD_KERNEL_ENTRY_OFFSET + 16

#define CMD_MEM_CMD_TYPE_OFFSET CMD_CMD_TYPE_OFFSET + 1
#define CMD_MEM_SRC_ADDR_OFFSET CMD_MEM_CMD_TYPE_OFFSET + 1
#define CMD_MEM_DST_ADDR_OFFSET CMD_MEM_SRC_ADDR_OFFSET + 8
#define CMD_MEM_LEN_OFFSET CMD_MEM_DST_ADDR_OFFSET + 8
#define CMD_MEM_DIR_OFFSET CMD_MEM_LEN_OFFSET + 4
#define CMD_MEM_HEADER_SIZE CMD_MEM_DIR_OFFSET + 1 // max possible header size
#define CMD_MEM_FROM_DRIVER 2 // with the direction: the host side of the copy is the driver's memory

#define CMD_WAIT_EVENT_STREAM_ID_OFFSET CMD_CMD_TYPE_OFFSET + 1
#define CMD_WAIT_EVENT_CMD_ID_OFFSET CMD_WAIT_EVENT_STREAM_ID_OFFSET + 1
#define CMD_WAIT_HEADER_SIZE CMD_WAIT_EVENT_CMD_ID_OFFSET + 8

#define CMD_CSR_ID_OFFSET CMD_CMD_TYPE_OFFSET + 1
#define CMD_CSR_FIELD_OFFSET CMD_CSR_ID_OFFSET + 1
#define CMD_CSR_VALUE_OFFSET CMD_CSR_FIELD_OFFSET + 1
#define CMD_CSR_HEADER_SIZE CMD_CSR_VALUE_OFFSET + 4

#define CMD_HEADER_SIZE CMD_MEM_HEADER_SIZE // max

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

struct HWStream {
    uint64_t head_cmd_id;
    uint64_t tail_cmd_id;
};

void KernelLaunch(KernelLaunchReq* req, const uint8_t* params_data);

void MemCpy(MemCpyReq* req);

uint32_t GPUMalloc(uint32_t bytes);

uint64_t CreateStream();

uint64_t EventRecord(radStream_t stream);

void TableWrite(TableWriteReq* req);

void WaitEvent(WaitEventReq* req);

bool GetError();

void Synchronize(SyncReq* req);

#endif
