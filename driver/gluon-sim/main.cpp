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

#include "utils/toml.hpp"

namespace {
constexpr const char* kConfigPath = "config.toml";

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
        std::cerr << "Failed to connect: " << std::strerror(errno) << '\n';
        ::close(sock);
        return 1;
    }

    std::array<std::uint8_t, 16> kernel_launch_cmd{};
    kernel_launch_cmd[0] = 0;   // CmdType::KERNEL
    kernel_launch_cmd[1] = 1;   // command id placeholder

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

    std::cout << "Submitting kernel launch command (id="
              << static_cast<int>(kernel_launch_cmd[1]) << ")\n";
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
