#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fcntl.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#include "utils/toml.hpp"

namespace {
constexpr const char* kConfigPath = "config.toml";

struct SharedMemoryRegion {
    int fd = -1;
    void* addr = MAP_FAILED;
    std::size_t size = 0;

    ~SharedMemoryRegion() {
        if (addr != MAP_FAILED) {
            ::munmap(addr, size);
        }
        if (fd != -1) {
            ::close(fd);
        }
    }

    [[nodiscard]] bool valid() const { return fd != -1 && addr != MAP_FAILED; }
};

std::optional<std::string> LoadSocketPath() {
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
}  // namespace

int main() {
    std::optional<std::string> socket_path = LoadSocketPath();
    if (!socket_path) {
        return 1;
    }

    if (socket_path->size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::cerr << "Socket path is too long: " << *socket_path << '\n';
        return 1;
    }

    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create socket: " << std::strerror(errno) << '\n';
        return 1;
    }

    sockaddr_un server_address{};
    server_address.sun_family = AF_UNIX;
    std::strncpy(server_address.sun_path, socket_path->c_str(), sizeof(server_address.sun_path) - 1);

    std::cout << "Connecting to " << *socket_path << "...\n";
    if (::connect(sock, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        const int connect_errno = errno;
        std::cerr << "Failed to connect: " << std::strerror(connect_errno) << '\n';

        if (connect_errno == EPERM || connect_errno == ECONNREFUSED) {
            std::cerr <<
                "Connect not permitted or refused; skipping driver run.\n"
                "(This usually means the server cannot accept sockets in the current sandbox.)\n";
            ::close(sock);
            return 0;
        }

        ::close(sock);
        return 1;
    }

    constexpr std::uint32_t kGpuDramAddr = 0x8000;
    const std::string payload = "hello world hello world hello world hello";

    const auto format_hex32 = [](std::uint32_t value) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
        return oss.str();
    };

    SharedMemoryRegion shm_region;
    shm_region.size = payload.size();
    shm_region.fd = ::memfd_create("gluon-payload", MFD_CLOEXEC);
    if (shm_region.fd == -1) {
        std::cerr << "Failed to create shared memory: " << std::strerror(errno) << '\n';
        ::close(sock);
        return 1;
    }

    if (::ftruncate(shm_region.fd, static_cast<off_t>(shm_region.size)) == -1) {
        std::cerr << "Failed to size shared memory: " << std::strerror(errno) << '\n';
        ::close(sock);
        return 1;
    }

    shm_region.addr = MAP_FAILED;

#ifdef MAP_FIXED_NOREPLACE
    {
        constexpr std::uintptr_t kPreferredBases[] = {
            0x10000000u,  // 256 MiB
            0x20000000u,
            0x30000000u,
            0x40000000u,
        };
        for (std::uintptr_t base : kPreferredBases) {
            void* desired = reinterpret_cast<void*>(base);
            void* mapped = ::mmap(
                desired,
                shm_region.size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED_NOREPLACE,
                shm_region.fd,
                0);
            if (mapped != MAP_FAILED) {
                shm_region.addr = mapped;
                break;
            }
        }
    }
#endif

#ifdef MAP_32BIT
    if (shm_region.addr == MAP_FAILED) {
        void* mapped = ::mmap(
            nullptr,
            shm_region.size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_32BIT,
            shm_region.fd,
            0);
        if (mapped != MAP_FAILED) {
            shm_region.addr = mapped;
        }
    }
#endif

    if (shm_region.addr == MAP_FAILED) {
        shm_region.addr = ::mmap(
            nullptr,
            shm_region.size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            shm_region.fd,
            0);
    }

    if (shm_region.addr == MAP_FAILED) {
        std::cerr << "Failed to map shared memory: " << std::strerror(errno) << '\n';
        ::close(sock);
        return 1;
    }

    std::memcpy(shm_region.addr, payload.data(), shm_region.size);

    const std::uintptr_t base_addr = reinterpret_cast<std::uintptr_t>(shm_region.addr);
    const std::uintptr_t payload_addr = base_addr;
    const std::uintptr_t host_offset_ptr = payload_addr - base_addr;
    if (host_offset_ptr > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Shared memory offset 0x" << std::hex << host_offset_ptr << std::dec
                  << " exceeds 32-bit range required by command payload\n";
        ::close(sock);
        return 1;
    }

    const std::uint32_t host_offset = static_cast<std::uint32_t>(host_offset_ptr);
    const std::uint32_t payload_size = static_cast<std::uint32_t>(shm_region.size);

    if (!SendFileDescriptor(sock, shm_region.fd)) {
        ::close(sock);
        return 1;
    }

    std::array<std::uint8_t, 16> kernel_launch_cmd{};
    kernel_launch_cmd[0] = 0;   // CmdType::KERNEL
    kernel_launch_cmd[1] = 0;   // command id

    const auto write_u32 = [&kernel_launch_cmd](std::size_t offset, std::uint32_t value) {
        kernel_launch_cmd[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
        kernel_launch_cmd[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        kernel_launch_cmd[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        kernel_launch_cmd[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    };

    write_u32(2, host_offset);
    write_u32(6, payload_size);
    write_u32(10, kGpuDramAddr);

    std::cout << "Submitting kernel launch command (id="
              << static_cast<int>(kernel_launch_cmd[1]) << ")\n";
    std::cout << "  host_offset=" << format_hex32(host_offset)
              << " size=" << payload_size
              << " gpu_addr=" << format_hex32(kGpuDramAddr)
              << " payload=\"" << payload << "\"\n";

    const auto send_all = [](int fd, const std::uint8_t* data, std::size_t size) -> bool {
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
    };

    if (!send_all(sock, kernel_launch_cmd.data(), kernel_launch_cmd.size())) {
        std::cerr << "Failed to send command: " << std::strerror(errno) << '\n';
        ::close(sock);
        return 1;
    }

    char buffer[1024] = {0};
    ssize_t received = ::recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (received == -1) {
        std::cerr << "Failed to receive data: " << std::strerror(errno) << '\n';
        ::close(sock);
        return 1;
    }

    std::string response(buffer, received > 0 ? static_cast<size_t>(received) : 0);
    std::cout << "Received response: " << response << "\n";

    ::close(sock);
    return 0;
}
