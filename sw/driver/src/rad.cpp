#include "rad.h"
#include "rad_rpc.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

constexpr const char* kRuntimeSocketPath = "./rad-driver.sock";

struct PendingD2H {
    void* dst;
    uint32_t bytes;
};

struct RuntimeState {
    int sock = -1;
    std::vector<std::deque<PendingD2H>> pending_d2h;
};

RuntimeState& State() {
    static RuntimeState s;
    return s;
}

void EnsurePendingSize(uint64_t stream) {
    auto& p = State().pending_d2h;
    if (p.size() <= stream)
        p.resize(stream + 1);
}

bool connect_driver() {
    auto& st = State();
    if (st.sock != -1)
        return true;

    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1)
        return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kRuntimeSocketPath, sizeof(addr.sun_path) - 1);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(sock);
        return false;
    }

    st.sock = sock;
    return true;
}

bool rpc_call(uint32_t op, const void* req, uint32_t req_size, std::vector<uint8_t>* resp) {
    if (!connect_driver())
        return false;

    return radrpc::Call(State().sock, op, req, req_size, resp);
}

} // namespace

void radKernelLaunch(const char *kernel_name,
                     radDim3 grid_dim,
                     radDim3 block_dim,
                     radParamBuf* params,
                     radStream_t stream) {
    uint32_t name_len = static_cast<uint32_t>(std::strlen(kernel_name));
    uint32_t params_len = params ? static_cast<uint32_t>(params->size()) : 0;

    radrpc::KernelLaunchReq req{};
    req.stream = stream;
    req.grid_x = grid_dim.x;
    req.grid_y = grid_dim.y;
    req.grid_z = grid_dim.z;
    req.block_x = block_dim.x;
    req.block_y = block_dim.y;
    req.block_z = block_dim.z;
    req.name_len = name_len;
    req.params_len = params_len;

    std::vector<uint8_t> payload(sizeof(req) + name_len + params_len);
    std::memcpy(payload.data(), &req, sizeof(req));
    std::memcpy(payload.data() + sizeof(req), kernel_name, name_len);
    if (params_len > 0)
        std::memcpy(payload.data() + sizeof(req) + name_len, params->data(), params_len);

    std::vector<uint8_t> resp;
    (void)rpc_call(radrpc::OP_KERNEL_LAUNCH, payload.data(), static_cast<uint32_t>(payload.size()), &resp);
}

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir, radStream_t stream) {
    if (dst == nullptr || src == nullptr)
        return;

    radrpc::MemcpyReq req{};
    req.stream = stream;
    req.dst = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(dst));
    req.src = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(src));
    req.bytes = static_cast<uint32_t>(bytes);
    req.dir = static_cast<uint32_t>(dir);

    std::vector<uint8_t> payload(sizeof(req) + (dir == radMemCpyDir_H2D ? bytes : 0));
    std::memcpy(payload.data(), &req, sizeof(req));
    if (dir == radMemCpyDir_H2D)
        std::memcpy(payload.data() + sizeof(req), src, bytes);
    else {
        EnsurePendingSize(stream);
        State().pending_d2h[stream].push_back(PendingD2H{dst, static_cast<uint32_t>(bytes)});
    }

    std::vector<uint8_t> resp;
    (void)rpc_call(radrpc::OP_MEMCPY, payload.data(), static_cast<uint32_t>(payload.size()), &resp);
}

void radMalloc(void **ptr, size_t bytes) {
    if (ptr == nullptr)
        return;

    radrpc::MallocReq req{static_cast<uint64_t>(bytes)};
    std::vector<uint8_t> resp;
    if (!rpc_call(radrpc::OP_MALLOC, &req, sizeof(req), &resp) || resp.size() != sizeof(radrpc::MallocResp)) {
        *ptr = nullptr;
        return;
    }

    auto* r = reinterpret_cast<const radrpc::MallocResp*>(resp.data());
    *ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(r->addr));
}

void radGetError(radError *err, radStream_t stream) {
    if (err == nullptr)
        return;

    radrpc::StreamReq req{stream};
    std::vector<uint8_t> resp;
    if (!rpc_call(radrpc::OP_GET_ERROR, &req, sizeof(req), &resp) || resp.size() < sizeof(radrpc::GetErrorResp))
        return;

    auto* r = reinterpret_cast<const radrpc::GetErrorResp*>(resp.data());
    err->err_code = static_cast<radErrorCode>(r->err_code);
    err->pc = r->pc;

    if (r->d2h_bytes > 0) {
        EnsurePendingSize(r->stream);
        auto& q = State().pending_d2h[r->stream];
        if (!q.empty()) {
            PendingD2H p = q.front();
            q.pop_front();
            uint32_t n = r->d2h_bytes;
            if (n > p.bytes)
                n = p.bytes;
            std::memcpy(p.dst, resp.data() + sizeof(radrpc::GetErrorResp), n);
        }
    }
}

void radCreateStream(radStream_t* stream) {
    if (stream == nullptr)
        return;
    std::vector<uint8_t> resp;
    if (!rpc_call(radrpc::OP_CREATE_STREAM, nullptr, 0, &resp) || resp.size() != sizeof(radrpc::CreateStreamResp))
        return;

    auto* r = reinterpret_cast<const radrpc::CreateStreamResp*>(resp.data());
    *stream = r->stream;
    EnsurePendingSize(*stream);
}

void radEventRecord(radEvent_t* event, radStream_t stream) {
    if (event == nullptr)
        return;

    radrpc::StreamReq req{stream};
    std::vector<uint8_t> resp;
    if (!rpc_call(radrpc::OP_EVENT_RECORD, &req, sizeof(req), &resp) || resp.size() != sizeof(radrpc::EventResp))
        return;

    auto* r = reinterpret_cast<const radrpc::EventResp*>(resp.data());
    event->hw_sid = r->hw_sid;
    event->cmd_id = r->cmd_id;
}

void radWaitEvent(radEvent_t* event, radStream_t stream) {
    if (event == nullptr)
        return;

    radrpc::WaitEventReq req{};
    req.stream = stream;
    req.hw_sid = event->hw_sid;
    req.cmd_id = event->cmd_id;
    std::vector<uint8_t> resp;
    (void)rpc_call(radrpc::OP_WAIT_EVENT, &req, sizeof(req), &resp);
}
