#ifndef RAD_RPC_HPP
#define RAD_RPC_HPP

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace radrpc {

enum Op : uint32_t {
    OP_CREATE_STREAM = 1,
    OP_MALLOC = 2,
    OP_KERNEL_LAUNCH = 3,
    OP_MEMCPY = 4,
    OP_EVENT_RECORD = 5,
    OP_WAIT_EVENT = 6,
    OP_GET_ERROR = 7,
};

struct MsgHeader {
    uint32_t op;
    uint32_t size;
};

struct RespHeader {
    int32_t status;
    uint32_t size;
};

struct CreateStreamResp {
    uint64_t stream;
};

struct MallocReq {
    uint64_t bytes;
};

struct MallocResp {
    uint32_t addr;
};

struct KernelLaunchReq {
    uint64_t stream;
    uint32_t grid_x;
    uint32_t grid_y;
    uint32_t grid_z;
    uint32_t block_x;
    uint32_t block_y;
    uint32_t block_z;
    uint32_t name_len;
    uint32_t params_len;
};

struct MemcpyReq {
    uint64_t stream;
    uint32_t dst;
    uint32_t src;
    uint32_t bytes;
    uint32_t dir;
};

struct StreamReq {
    uint64_t stream;
};

struct EventResp {
    uint8_t hw_sid;
    uint8_t _pad[7];
    uint64_t cmd_id;
};

struct WaitEventReq {
    uint64_t stream;
    uint8_t hw_sid;
    uint8_t _pad[7];
    uint64_t cmd_id;
};

struct GetErrorResp {
    uint64_t stream;
    uint32_t err_code;
    uint32_t pc;
    uint32_t d2h_bytes;
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

} // namespace radrpc

#endif
