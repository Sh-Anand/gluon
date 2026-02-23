#ifndef RAD_RPC_HPP
#define RAD_RPC_HPP

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <vector>


enum Op : uint32_t {
    OP_CREATE_STREAM = 1,
    OP_MALLOC = 2,
    OP_KERNEL_LAUNCH = 3,
    OP_MEMCPY = 4,
    OP_EVENT_RECORD = 5,
    OP_WAIT_EVENT = 6,
    OP_SYNC = 7,
    OP_SET_HOST_PID = 8,
};

struct MsgHeader {
    uint32_t op;
    uint32_t size;
};

struct RespHeader {
    int32_t status;
    uint32_t size;
};

struct KernelLaunchReq {
    radStream_t stream;
    radDim3 grid_dim;
    radDim3 block_dim;
    uint32_t params_size;
    uint32_t name_len;
};

struct MemCpyReq {
    radStream_t stream;
    uint64_t dst_addr;
    uint64_t src_addr;
    uint32_t bytes;
    radMemcpyDir dir;
};

struct WaitEventReq {
    radStream_t stream;
    radEvent_t event;
};

struct SyncReq {
    uint64_t stream;
    uint64_t cmd_id;
};

inline bool WriteFull(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

inline bool ReadFull(int fd, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, MSG_WAITALL);
        if (n <= 0)
            return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

inline bool Call(int fd, uint32_t op, const void* req, uint32_t req_size, std::vector<uint8_t>* resp) {
    MsgHeader hdr{op, req_size};
    if (!WriteFull(fd, &hdr, sizeof(hdr)))
        return false;
    if (req_size > 0 && !WriteFull(fd, req, req_size))
        return false;

    RespHeader rh{};
    if (!ReadFull(fd, &rh, sizeof(rh)))
        return false;
    if (rh.status != 0)
        return false;

    if (!resp) {
        std::vector<uint8_t> discard(rh.size);
        return rh.size == 0 || ReadFull(fd, discard.data(), rh.size);
    }
    resp->resize(rh.size);
    return rh.size == 0 || ReadFull(fd, resp->data(), rh.size);
}

inline bool SendResp(int fd, int32_t status, const void* payload, uint32_t size) {
    RespHeader rh{status, size};
    if (!WriteFull(fd, &rh, sizeof(rh)))
        return false;
    return size == 0 || WriteFull(fd, payload, size);
}

#endif
