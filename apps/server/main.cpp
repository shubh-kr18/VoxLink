#include "aura/floor_manager.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr int PORT = 5050;
constexpr std::size_t MAX_LINE_SIZE = 1024;

std::mutex log_mutex;

bool send_all(int fd, const std::string& message) {
    std::size_t sent = 0;

    while (sent < message.size()) {
        ssize_t result = send(
            fd,
            message.data() + sent,
            message.size() - sent,
            0
        );

        if (result <= 0) {
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

std::string process_command(
    aura::FloorManager& floor,
    const std::string& line
) {
    std::istringstream input(line);

    std::string command;
    std::string channel;
    std::string user;

    input >> command >> channel;

    if (command == "ACQUIRE") {
        if (!(input >> user)) {
            return "ERR invalid_command\n";
        }

        std::string extra;

        if (input >> extra) {
            return "ERR invalid_command\n";
        }

        auto grant = floor.acquire(
            channel,
            user,
            std::chrono::seconds(60)
        );

        if (!grant.has_value()) {
            return "DENIED\n";
        }

        return "GRANTED " +
            std::to_string(grant->token) + "\n";
    }

    if (command == "OWNER") {
        std::string extra;

        if (channel.empty() || input >> extra) {
            return "ERR invalid_command\n";
        }

        auto current = floor.owner(channel);

        if (!current.has_value()) {
            return "OWNER NONE\n";
        }

        return "OWNER " + *current + "\n";
    }

    if (command == "RELEASE") {
        std::uint64_t token;

        if (!(input >> user >> token)) {
            return "ERR invalid_command\n";
        }

        std::string extra;

        if (input >> extra) {
            return "ERR invalid_command\n";
        }

        bool released = floor.release(
            channel,
            user,
            token
        );

        return released
            ? "RELEASED\n"
            : "RELEASE_DENIED\n";
    }

    return "ERR unknown_command\n";
}

void handle_client(int client_fd, aura::FloorManager& floor) {
    // Prevent a disconnected client from terminating the server
    // through SIGPIPE when send() is called.
    int enabled = 1;

    setsockopt(
        client_fd,
        SOL_SOCKET,
        SO_NOSIGPIPE,
        &enabled,
        sizeof(enabled)
    );

    {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "Client connected\n";
    }

    std::string line;
    char buffer[512];
    bool connected = true;

    while (connected) {
        ssize_t received = recv(
            client_fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received <= 0) {
            break;
        }

        for (ssize_t i = 0; i < received; ++i) {
            char c = buffer[i];

            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                std::string response =
                    process_command(floor, line);

                if (!send_all(client_fd, response)) {
                    connected = false;
                    break;
                }

                line.clear();
            } else {
                line += c;

                if (line.size() > MAX_LINE_SIZE) {
                    send_all(
                        client_fd,
                        "ERR line_too_long\n"
                    );

                    connected = false;
                    break;
                }
            }
        }
    }

    close(client_fd);

    {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "Client disconnected\n";
    }
}

} // namespace

int main() {
    aura::FloorManager floor;

    int server_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int reuse = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);

    inet_pton(
        AF_INET,
        "127.0.0.1",
        &address.sin_addr
    );

    if (bind(
        server_fd,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    ) < 0) {
        std::cerr << "Bind failed. Is port "
                  << PORT << " already in use?\n";

        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }

    std::cout << "AURA server listening on "
              << "127.0.0.1:" << PORT << '\n';

    while (true) {
        int client_fd = accept(
            server_fd,
            nullptr,
            nullptr
        );

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "Accept failed\n";
            continue;
        }

        std::thread(
            handle_client,
            client_fd,
            std::ref(floor)
        ).detach();
    }

    close(server_fd);
    return 0;
}