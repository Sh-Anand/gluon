#include "rad.h"
#include "rad_rpc.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kRuntimeSocketPath = "./rad-driver.sock";

struct RuntimeState {
    int sock = -1;
};

RuntimeState& State() {
    static RuntimeState s;
    return s;
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
    uint32_t pid = static_cast<uint32_t>(::getpid());
    std::vector<uint8_t> resp;
    if (!Call(st.sock, OP_SET_HOST_PID, &pid, sizeof(pid), &resp)) {
        ::close(st.sock);
        st.sock = -1;
        return false;
    }
    return true;
}

bool rpc_call(uint32_t op, const void* payload, uint32_t payload_size, std::vector<uint8_t>* resp) {
    if (!connect_driver())
        return false;

    return Call(State().sock, op, payload, payload_size, resp);
}

} // namespace

void radKernelLaunch(const char *kernel_name,
                     radDim3 grid_dim,
                     radDim3 block_dim,
                     radParamBuf* params,
                     radStream_t stream) {
    uint32_t name_len = static_cast<uint32_t>(std::strlen(kernel_name));
    uint32_t params_len = params ? static_cast<uint32_t>(params->size()) : 0;

    KernelLaunchReq req{};
    req.stream = stream;
    req.grid_dim = grid_dim;
    req.block_dim = block_dim;
    req.params_size = params_len;
    req.name_len = name_len;

    std::vector<uint8_t> payload(sizeof(req) + name_len + params_len);
    std::memcpy(payload.data(), &req, sizeof(req));
    std::memcpy(payload.data() + sizeof(req), kernel_name, name_len);
    if (params_len > 0)
        std::memcpy(payload.data() + sizeof(req) + name_len, params->data(), params_len);

    std::vector<uint8_t> resp;
    (void)rpc_call(OP_KERNEL_LAUNCH, payload.data(), (uint32_t)(payload.size()), &resp);
}

void radMemCpy(void *dst, void *src, size_t bytes, radMemCpyDir dir, radStream_t stream) {
    if (dst == nullptr || src == nullptr)
        return;

    MemCpyReq req{};
    req.stream = stream;
    req.dst_addr = (uint64_t)(uintptr_t)(dst);
    req.src_addr = (uint64_t)(uintptr_t)(src);
    req.bytes = (uint32_t)(bytes);
    req.dir = dir;

    std::vector<uint8_t> payload(sizeof(req));
    std::memcpy(payload.data(), &req, sizeof(req));

    std::vector<uint8_t> resp;
    (void)rpc_call(OP_MEMCPY, payload.data(), (uint32_t)(payload.size()), &resp);
}

void radMalloc(void **ptr, size_t bytes) {
    if (ptr == nullptr)
        return;

    uint32_t req = (uint32_t)(bytes);
    std::vector<uint8_t> resp;
    if (!rpc_call(OP_MALLOC, &req, sizeof(req), &resp) || resp.size() != sizeof(uint32_t)) {
        *ptr = nullptr;
        return;
    }

    *ptr = (void *)(*(uint32_t *)resp.data());
}

void radCreateStream(radStream_t* stream) {
    if (stream == nullptr)
        return;
    std::vector<uint8_t> resp;
    if (!rpc_call(OP_CREATE_STREAM, nullptr, 0, &resp) || resp.size() != sizeof(uint64_t))
        return;

    *stream = *(uint64_t*)resp.data();
}

void radEventRecord(radEvent_t* event, radStream_t stream) {
    if (event == nullptr)
        return;

    std::vector<uint8_t> resp;
    if (!rpc_call(OP_EVENT_RECORD, &stream, sizeof(stream), &resp) || resp.size() != sizeof(uint64_t))
        return;

    event->stream = stream;
    event->cmd_id = *(uint64_t*)resp.data();
}

void radEventSynchronize(radEvent_t* event) {
    if (event == nullptr)
        return;

    SyncReq req{};
    req.stream = event->stream;
    req.cmd_id = event->cmd_id;
    std::vector<uint8_t> resp;
    (void)rpc_call(OP_SYNC, &req, sizeof(req), &resp);
}

void radStreamWaitEvent(radEvent_t* event, radStream_t stream) {
    if (event == nullptr)
        return;

    WaitEventReq req{};
    req.stream = stream;
    req.event = *event;
    std::vector<uint8_t> resp;
    (void)rpc_call(OP_WAIT_EVENT, &req, sizeof(req), &resp);
}

void radStreamSynchronize(radStream_t stream) {
    SyncReq req{};
    req.stream = stream;
    req.cmd_id = 0;
    std::vector<uint8_t> resp;
    (void)rpc_call(OP_SYNC, &req, sizeof(req), &resp);
}
