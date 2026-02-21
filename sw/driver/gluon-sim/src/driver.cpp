#include "driver.h"
#include "rad.h"
#include "command.hpp"

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <toml.hpp>

namespace rad {
namespace {
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

bool SendCommand(int sock, const std::array<std::uint8_t, 16>& data, int fd) {
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
    region.addr = MAP_FAILED;
#ifdef MAP_FIXED_NOREPLACE
    {
        constexpr std::uintptr_t kPreferredBases[] = {
            0x10000000u,
            0x20000000u,
            0x30000000u,
            0x40000000u,
        };
        for (std::uintptr_t base : kPreferredBases) {
            void* desired = reinterpret_cast<void*>(base);
            void* mapped = ::mmap(
                desired,
                region.size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED_NOREPLACE,
                region.fd,
                0);
            if (mapped != MAP_FAILED) {
                region.addr = mapped;
                break;
            }
        }
    }
#endif
#ifdef MAP_32BIT
    if (region.addr == MAP_FAILED) {
        void* mapped = ::mmap(
            nullptr,
            region.size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_32BIT,
            region.fd,
            0);
        if (mapped != MAP_FAILED) {
            region.addr = mapped;
        }
    }
#endif
    if (region.addr == MAP_FAILED) {
        region.addr = ::mmap(
            nullptr,
            region.size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            region.fd,
            0);
    }
    if (region.addr == MAP_FAILED) {
        region.Reset();
        return false;
    }
    return true;
}

std::optional<std::string> SubmitCommand(const std::array<std::uint8_t, 16>& header,
                                         const void* payload,
                                         std::size_t payload_size) {
    ConnectionState& state = GetState();
    if (!state.initialized) {
        if (!InitConnection()) {
            std::cerr << "Failed to initialize connection\n";
            return std::nullopt;
        }
    }
    std::array<std::uint8_t, 16> header_bytes = header;
    state.last_shared_addr = nullptr;
    SharedMemoryRegion region;
    bool use_region = false;
    bool retain_region = false;
    if (header_bytes[1] == radCmdType_KERNEL) {
        use_region = true;
    } else if (header_bytes[1] == radCmdType_MEM) {
        use_region = true;
        retain_region = (header_bytes[15] == radMemCpyDir_D2H);
    }
    if (use_region) {
        std::size_t region_size = payload_size;
        if (region_size == 0) {
            region_size = ReadU32LE(header_bytes.data() + 11);
        }
        if (!CreateRegion(region, region_size)) {
            return std::nullopt;
        }
        if (payload_size > 0) {
            std::memcpy(region.addr, payload, payload_size);
        }
        std::uintptr_t shared_base = reinterpret_cast<std::uintptr_t>(region.addr);
        std::uint32_t shared_base_u32 = static_cast<std::uint32_t>(shared_base);
        if (header_bytes[1] == radCmdType_MEM) {
            if (header_bytes[15] == radMemCpyDir_H2D) {
                std::memcpy(header_bytes.data() + 3, &shared_base_u32, sizeof(shared_base_u32));
            } else {
                std::memcpy(header_bytes.data() + 7, &shared_base_u32, sizeof(shared_base_u32));
            }
        } else {
            std::memcpy(header_bytes.data() + 2, &shared_base_u32, sizeof(shared_base_u32));
        }
    }
    std::cout << "Submitting command (id=" << static_cast<int>(header_bytes[0])
              << ", size=" << payload_size
              << ")\n";
    if (!SendCommand(state.sock, header_bytes, use_region ? region.fd : -1)) {
        return std::nullopt;
    }
    if (use_region) {
        void* addr = region.addr;
        state.retained.push_back(std::move(region));
        if (retain_region) {
            state.last_shared_addr = addr;
        }
    }
    return std::string("OK");
}

std::optional<std::string> ReceiveError() {
    ConnectionState& state = GetState();
    if (!state.initialized) {
        return std::nullopt;
    }
    char buffer[16] = {0};
    ssize_t received = ::recv(state.sock, buffer, sizeof(buffer), MSG_WAITALL);
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
    return GetState().last_shared_addr;
}

void ReleaseSharedMemoryBase(void* addr) {
    auto& retained = GetState().retained;
    for (std::size_t i = 0; i < retained.size(); ++i) {
        if (retained[i].addr == addr) {
            retained[i].Reset();
            retained.erase(retained.begin() + i);
            return;
        }
    }
}

}  // namespace rad
