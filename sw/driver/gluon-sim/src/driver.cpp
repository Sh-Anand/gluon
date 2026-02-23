#include "driver.h"
#include "rad.h"
#include "core.h"
#include "rad_rpc.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <toml.hpp>
struct ConnectionState {
    bool initialized = false;
    int sock = -1;
    uint32_t runtime_pid = 0;
};

ConnectionState& GetState() {
    static ConnectionState state;
    return state;
}

std::mutex& GetStateMutex() {
    static std::mutex mu;
    return mu;
}

std::optional<std::string> LoadSocketPath() {
    constexpr const char* kConfigPath = "config.toml";
    toml::table config;
    try {
        config = toml::parse_file(kConfigPath);
    } catch (const toml::parse_error& err) {
        std::cerr << "Failed to parse config file: " << err.description() << '\n';
        return std::nullopt;
    }
    toml::node_view<toml::node> server = config["server"];
    if (!server || !server.is_table()) {
        std::cerr << "[server] section missing from config file\n";
        return std::nullopt;
    }
    std::optional<std::string> socket_path = server["socket_path"].value<std::string>();
    if (!socket_path) {
        std::cerr << "socket_path missing from [server] section\n";
        return std::nullopt;
    }
    return socket_path;
}

bool SendCommand(int sock, const std::array<std::uint8_t, CMD_HEADER_SIZE>& data) {
    return WriteFull(sock, data.data(), data.size());
}

void ShutdownConnection() {
    ConnectionState& state = GetState();
    if (state.sock != -1) {
        ::close(state.sock);
        state.sock = -1;
    }
    state.initialized = false;
}

bool InitConnection() {
    ConnectionState& state = GetState();
    if (state.initialized) {
        return true;
    }
    auto socket_path = LoadSocketPath();
    if (!socket_path) {
        return false;
    }
    if (socket_path->size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::cerr << "Socket path is too long: " << *socket_path << '\n';
        return false;
    }
    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create socket: " << std::strerror(errno) << '\n';
        return false;
    }
    sockaddr_un server_address{};
    server_address.sun_family = AF_UNIX;
    std::strncpy(server_address.sun_path, socket_path->c_str(), sizeof(server_address.sun_path) - 1);
    std::cout << "Connecting to " << *socket_path << "...\n";
    if (::connect(sock, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        const int connect_errno = errno;
        std::cerr << "Failed to connect: " << std::strerror(connect_errno) << '\n';
        ::close(sock);
        return false;
    }
    state.sock = sock;
    uint32_t pids[2] = {state.runtime_pid, static_cast<uint32_t>(::getpid())};
    if (!WriteFull(state.sock, pids, sizeof(pids))) {
        ::close(state.sock);
        state.sock = -1;
        return false;
    }
    state.initialized = true;
    static bool registered = false;
    if (!registered) {
        std::atexit([] { ShutdownConnection(); });
        registered = true;
    }
    return true;
}

std::optional<std::string> driver::SubmitCommand(const std::vector<std::uint8_t>& header,
                                         const void* payload,
                                         std::size_t payload_size) {
    (void)payload;
    bool initialized = false;
    std::unique_lock<std::mutex> lock(GetStateMutex());
    initialized = GetState().initialized;
    lock.unlock();
    if (!initialized && !InitConnection()) {
        std::cerr << "Failed to initialize connection\n";
        return std::nullopt;
    }
    ConnectionState& state = GetState();
    lock.lock();
    int sock = state.sock;
    lock.unlock();
    std::array<std::uint8_t, CMD_HEADER_SIZE> header_bytes{};
    std::memcpy(header_bytes.data(), header.data(), header.size());
    std::cout << "Submitting command (id=" << static_cast<int>(header_bytes[0])
              << ", size=" << payload_size
              << ")\n";
    if (!SendCommand(sock, header_bytes)) {
        return std::nullopt;
    }
    return std::string("OK");
}

std::optional<std::string> driver::ReceiveError() {
    ConnectionState& state = GetState();
    if (!state.initialized) {
        return std::nullopt;
    }
    int sock = state.sock;
    char buffer[16] = {0};
    ssize_t received = ::recv(sock, buffer, sizeof(buffer), MSG_WAITALL);
    if (received == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        std::cerr << "Failed to receive data: " << std::strerror(errno) << '\n';
        return std::nullopt;
    }
    if (received != static_cast<ssize_t>(sizeof(buffer))) {
        return std::nullopt;
    }
    if (received == 0) {
        ShutdownConnection();
        return std::nullopt;
    }
    return std::string(buffer, static_cast<std::size_t>(received));
}

constexpr const char* kDriverSocketPath = "./rad-driver.sock";

int main() {
    ::unlink(kDriverSocketPath);

    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv == -1)
        return 1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kDriverSocketPath, sizeof(addr.sun_path) - 1);

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
        return 1;

    if (::listen(srv, 1) == -1)
        return 1;

    int cli = ::accept(srv, nullptr, nullptr);
    if (cli == -1)
        return 1;

    std::thread([] {
        for (;;) {
            if (!GetError())
                ::usleep(1000);
        }
    }).detach();

    for (;;) {
        MsgHeader h{};
        if (!ReadFull(cli, &h, sizeof(h)))
            break;

        std::vector<uint8_t> req(h.size);
        if (h.size > 0 && !ReadFull(cli, req.data(), h.size))
            break;

        if (h.op == OP_CREATE_STREAM) {
            uint64_t resp = CreateStream();
            if (!SendResp(cli, 0, &resp, sizeof(resp)))
                break;
        } else if (h.op == OP_MALLOC) {
            uint32_t resp = GPUMalloc(*(uint32_t*)req.data());
            if (!SendResp(cli, 0, &resp, sizeof(resp)))
                break;
        } else if (h.op == OP_KERNEL_LAUNCH) {
            KernelLaunchReq* k = (KernelLaunchReq*)req.data();
            const uint8_t* params_data = req.data() + sizeof(*k);
            KernelLaunch(k, params_data);
            if (!SendResp(cli, 0, nullptr, 0))
                break;
        } else if (h.op == OP_MEMCPY) {
            MemCpy((MemCpyReq*)req.data());
            if (!SendResp(cli, 0, nullptr, 0))
                break;
        } else if (h.op == OP_EVENT_RECORD) {
            uint64_t resp = EventRecord(*(radStream_t*)req.data());
            if (!SendResp(cli, 0, &resp, sizeof(resp)))
                break;
        } else if (h.op == OP_WAIT_EVENT) {
            WaitEvent((WaitEventReq*)req.data());
            if (!SendResp(cli, 0, nullptr, 0))
                break;
        } else if (h.op == OP_SYNC) {
            Synchronize((SyncReq*)req.data());
            if (!SendResp(cli, 0, nullptr, 0))
                break;
        } else if (h.op == OP_SET_HOST_PID) {
            uint32_t pid = *(uint32_t*)req.data();
            std::lock_guard<std::mutex> lock(GetStateMutex());
            GetState().runtime_pid = pid;
            if (!SendResp(cli, 0, nullptr, 0))
                break;
        } else {
            if (!SendResp(cli, -1, nullptr, 0))
                break;
        }
    }

    ::close(cli);
    ::close(srv);
    ::unlink(kDriverSocketPath);
    return 0;
}
