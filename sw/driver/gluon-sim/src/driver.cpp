#include "driver.h"

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
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "utils/toml.hpp"

namespace rad {
namespace {
struct SharedMemoryRegion {
    int fd = -1;
    void* addr = MAP_FAILED;
    std::size_t size = 0;

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
    SharedMemoryRegion shared;
std::uint32_t gpu_addr = 0;
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

bool SendFileDescriptor(int sock, int fd) {
    struct ::msghdr msg = {};
    unsigned char buffer = 0;
    struct ::iovec iov;
    iov.iov_base = &buffer;
    iov.iov_len = sizeof(buffer);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(::cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof(control));
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct ::cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    if (::sendmsg(sock, &msg, 0) == -1) {
        std::cerr << "Failed to send shared memory fd: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

bool SendAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t total_sent = 0;
    while (total_sent < size) {
        ssize_t sent = ::send(fd, data + total_sent, size - total_sent, 0);
        if (sent == -1) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string FormatHex32(std::uint32_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return oss.str();
}
}

void ShutdownConnection() {
    ConnectionState& state = GetState();
    if (state.sock != -1) {
        ::close(state.sock);
        state.sock = -1;
    }
    state.shared.Reset();
    state.initialized = false;
    state.gpu_addr = 0;
}

bool InitConnection(std::size_t shared_mem_bytes, std::uint32_t gpu_addr) {
    ConnectionState& state = GetState();
    if (state.initialized) {
        if (shared_mem_bytes <= state.shared.size) {
            state.gpu_addr = gpu_addr;
            return true;
        }
        ShutdownConnection();
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
    SharedMemoryRegion region;
    region.size = shared_mem_bytes;
    region.fd = ::memfd_create("gluon-payload", MFD_CLOEXEC);
    if (region.fd == -1) {
        std::cerr << "Failed to create shared memory: " << std::strerror(errno) << '\n';
        ::close(sock);
        return false;
    }
    if (::ftruncate(region.fd, static_cast<off_t>(region.size)) == -1) {
        std::cerr << "Failed to size shared memory: " << std::strerror(errno) << '\n';
        region.Reset();
        ::close(sock);
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
        std::cerr << "Failed to map shared memory: " << std::strerror(errno) << '\n';
        region.Reset();
        ::close(sock);
        return false;
    }
    if (!SendFileDescriptor(sock, region.fd)) {
        region.Reset();
        ::close(sock);
        return false;
    }
    state.shared.Reset();
    state.shared.fd = region.fd;
    state.shared.addr = region.addr;
    state.shared.size = region.size;
    region.fd = -1;
    region.addr = MAP_FAILED;
    region.size = 0;
    state.sock = sock;
    state.gpu_addr = gpu_addr;
    state.initialized = true;
    static bool registered = false;
    if (!registered) {
        std::atexit([] { ShutdownConnection(); });
        registered = true;
    }
    return true;
}

bool IsConnectionReady() {
    return GetState().initialized;
}

std::uint32_t GetGpuAddress() {
    return GetState().gpu_addr;
}

std::optional<std::string> SubmitKernelLaunch(const KernelLaunchHeader& header,
                                              const std::vector<std::uint8_t>& payload) {
    ConnectionState& state = GetState();
    if (!state.initialized) {
        std::cerr << "Kernel launch attempted before initializing connection\n";
        return std::nullopt;
    }
    if (header.payload_size != payload.size()) {
        std::cerr << "Kernel payload size mismatch\n";
        return std::nullopt;
    }
    if (header.host_offset > state.shared.size ||
        header.host_offset + payload.size() > state.shared.size) {
        std::cerr << "Kernel payload offset outside shared memory\n";
        return std::nullopt;
    }
    std::memcpy(static_cast<std::uint8_t*>(state.shared.addr) + header.host_offset,
                payload.data(),
                payload.size());
    std::array<std::uint8_t, 16> kernel_launch_cmd{};
    kernel_launch_cmd[0] = 0;
    kernel_launch_cmd[1] = header.command_id;
    const auto write_u32 = [&kernel_launch_cmd](std::size_t offset, std::uint32_t value) {
        kernel_launch_cmd[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
        kernel_launch_cmd[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        kernel_launch_cmd[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        kernel_launch_cmd[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    };
    write_u32(2, header.host_offset);
    write_u32(6, header.payload_size);
    write_u32(10, header.gpu_addr);
    std::cout << "Submitting kernel launch command (id="
              << static_cast<int>(kernel_launch_cmd[1]) << ")\n";
    std::cout << "  host_offset=" << FormatHex32(header.host_offset)
              << " size=" << header.payload_size
              << " gpu_addr=" << FormatHex32(header.gpu_addr) << '\n';
    if (!SendAll(state.sock, kernel_launch_cmd.data(), kernel_launch_cmd.size())) {
        std::cerr << "Failed to send command: " << std::strerror(errno) << '\n';
        return std::nullopt;
    }
    char buffer[1024] = {0};
    ssize_t received = ::recv(state.sock, buffer, sizeof(buffer) - 1, 0);
    if (received == -1) {
        std::cerr << "Failed to receive data: " << std::strerror(errno) << '\n';
        return std::nullopt;
    }
    std::string response(buffer, received > 0 ? static_cast<size_t>(received) : 0);
    std::cout << "Received response: " << response << "\n";
    return response;
}

}  // namespace rad
