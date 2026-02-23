#include "driver.h"
#include "rad.h"
#include "core.h"
#include "rad_rpc.hpp"

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <toml.hpp>
struct SharedMemoryRegion {
    int fd = -1;
    void* addr = MAP_FAILED;
    std::size_t size = 0;

    SharedMemoryRegion() = default;
    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion(SharedMemoryRegion&& other) noexcept
        : fd(other.fd), addr(other.addr), size(other.size) {
        other.fd = -1;
        other.addr = MAP_FAILED;
        other.size = 0;
    }
    SharedMemoryRegion& operator=(SharedMemoryRegion&& other) noexcept {
        if (this != &other) {
            Reset();
            fd = other.fd;
            addr = other.addr;
            size = other.size;
            other.fd = -1;
            other.addr = MAP_FAILED;
            other.size = 0;
        }
        return *this;
    }

    void Reset() {
        if (addr != MAP_FAILED) {
            ::munmap(addr, size);
            addr = MAP_FAILED;
        }
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
        size = 0;
    }

    ~SharedMemoryRegion() { Reset(); }
};

struct ConnectionState {
    bool initialized = false;
    int sock = -1;
    std::vector<SharedMemoryRegion> retained;
    void* last_shared_addr = nullptr;
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

bool SendCommand(int sock, const std::array<std::uint8_t, CMD_HEADER_SIZE>& data, int fd) {
    struct ::msghdr msg = {};
    struct ::iovec iov;
    iov.iov_base = const_cast<std::uint8_t*>(data.data());
    iov.iov_len = data.size();
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(::cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))] = {};
    if (fd != -1) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct ::cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    if (::sendmsg(sock, &msg, 0) == -1) {
        std::cerr << "Failed to send command: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

void ShutdownConnection() {
    ConnectionState& state = GetState();
    if (state.sock != -1) {
        ::close(state.sock);
        state.sock = -1;
    }
    state.retained.clear();
    state.last_shared_addr = nullptr;
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
    state.initialized = true;
    static bool registered = false;
    if (!registered) {
        std::atexit([] { ShutdownConnection(); });
        registered = true;
    }
    return true;
}

static std::uint32_t ReadU32LE(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

static bool CreateRegion(SharedMemoryRegion& region, std::size_t size) {
    region.size = size;
    region.fd = ::memfd_create("gluon-cmd", MFD_CLOEXEC);
    if (region.fd == -1) {
        return false;
    }
    if (::ftruncate(region.fd, static_cast<off_t>(region.size)) == -1) {
        region.Reset();
        return false;
    }
    region.addr = ::mmap(
        nullptr,
        region.size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        region.fd,
        0);
    if (region.addr == MAP_FAILED) {
        region.Reset();
        return false;
    }
    return true;
}

std::optional<std::string> driver::SubmitCommand(const std::vector<std::uint8_t>& header,
                                         const void* payload,
                                         std::size_t payload_size) {
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
    state.last_shared_addr = nullptr;
    int sock = state.sock;
    lock.unlock();
    std::array<std::uint8_t, CMD_HEADER_SIZE> header_bytes{};
    std::memcpy(header_bytes.data(), header.data(), header.size());
    SharedMemoryRegion region;
    bool use_region = false;
    bool retain_region = false;
    if (header_bytes[CMD_CMD_TYPE_OFFSET] == radCmdType_KERNEL) {
        use_region = true;
    } else if (header_bytes[CMD_CMD_TYPE_OFFSET] == radCmdType_MEM) {
        use_region = true;
        retain_region = (header_bytes[CMD_MEM_DIR_OFFSET] == radMemCpyDir_D2H);
    }
    if (use_region) {
        std::size_t region_size = payload_size;
        if (region_size == 0) {
            region_size = ReadU32LE(header_bytes.data() + CMD_MEM_LEN_OFFSET);
        }
        if (!CreateRegion(region, region_size)) {
            return std::nullopt;
        }
        if (payload_size > 0) {
            std::memcpy(region.addr, payload, payload_size);
        }
        std::uintptr_t shared_base = reinterpret_cast<std::uintptr_t>(region.addr);
        std::uint64_t shared_base_u64 = static_cast<std::uint64_t>(shared_base);
        if (header_bytes[CMD_CMD_TYPE_OFFSET] == radCmdType_MEM) {
            if (header_bytes[CMD_MEM_DIR_OFFSET] == radMemCpyDir_H2D) {
                std::memcpy(header_bytes.data() + CMD_MEM_SRC_ADDR_OFFSET, &shared_base_u64, sizeof(shared_base_u64));
            }
        } else {
            std::memcpy(header_bytes.data() + CMD_KERNEL_HOST_ADDR_OFFSET, &shared_base_u64, sizeof(shared_base_u64));
        }
    }
    std::cout << "Submitting command (id=" << static_cast<int>(header_bytes[0])
              << ", size=" << payload_size
              << ")\n";
    if (!SendCommand(sock, header_bytes, use_region ? region.fd : -1)) {
        return std::nullopt;
    }
    if (use_region) {
        void* addr = region.addr;
        lock.lock();
        state.retained.push_back(std::move(region));
        if (retain_region) {
            state.last_shared_addr = addr;
        }
        lock.unlock();
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

void* GetSharedMemoryBase() {
    return nullptr;
}

void* GetLastSharedMemoryBase() {
    std::lock_guard<std::mutex> lock(GetStateMutex());
    return GetState().last_shared_addr;
}

void ReleaseSharedMemoryBase(void* addr) {
    std::lock_guard<std::mutex> lock(GetStateMutex());
    auto& retained = GetState().retained;
    for (std::size_t i = 0; i < retained.size(); ++i) {
        if (retained[i].addr == addr) {
            retained[i].Reset();
            retained.erase(retained.begin() + i);
            return;
        }
    }
}

constexpr const char* kDriverSocketPath = "./rad-driver.sock";
constexpr const char* kHostPidPath = "./.gluon_host_pid";

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
            const char* kernel_name = reinterpret_cast<const char*>(req.data() + sizeof(*k));
            const uint8_t* params_data = req.data() + sizeof(*k) + k->name_len;
            std::string kernel_name_str(kernel_name, kernel_name + k->name_len);
            KernelLaunch(k, kernel_name_str.c_str(), params_data);
            if (!SendResp(cli, 0, nullptr, 0))
                break;
        } else if (h.op == OP_MEMCPY) {
            void* h2d_data = (void*)(uintptr_t)(req.data() + sizeof(MemCpyReq));
            MemCpy((MemCpyReq*)req.data(), h2d_data);
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
            FILE* f = std::fopen(kHostPidPath, "w");
            if (!f) {
                if (!SendResp(cli, -1, nullptr, 0))
                    break;
                continue;
            }
            std::fprintf(f, "%u\n", pid);
            std::fclose(f);
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
