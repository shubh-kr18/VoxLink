#include "aura/floor_manager.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr int PORT = 5050;
constexpr std::size_t MAX_LINE_SIZE = 1024;
constexpr std::size_t MAX_CLIENTS = 32;

struct ClientSlot {
    std::thread worker;
    bool in_use = false;
    bool finished = false;
};

std::array<ClientSlot, MAX_CLIENTS> clients;
std::mutex clients_mutex;
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
            if (result < 0 && errno == EINTR) {
                continue;
            }

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
        if (channel.empty() || !(input >> user)) {
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

        if (
            channel.empty() ||
            !(input >> user >> token)
        ) {
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

void handle_client(
    int client_fd,
    aura::FloorManager& floor,
    ClientSlot& slot
) {
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

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        slot.finished = true;
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

    if (
        inet_pton(
            AF_INET,
            "127.0.0.1",
            &address.sin_addr
        ) != 1
    ) {
        std::cerr << "Invalid server address\n";
        close(server_fd);
        return 1;
    }

    if (
        bind(
            server_fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0
    ) {
        std::cerr << "Bind failed. Is port "
                  << PORT << " already in use?\n";

        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 64) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return 1;
    }

    std::cout << "VoxLink server listening on "
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

        int slot_index = -1;
        std::thread completed_thread;

        {
            std::lock_guard<std::mutex> lock(clients_mutex);

            // Find a free slot or reclaim a completed worker.
            for (std::size_t i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].in_use && clients[i].finished) {
                    completed_thread =
                        std::move(clients[i].worker);

                    clients[i].in_use = false;
                    clients[i].finished = false;
                }

                if (!clients[i].in_use) {
                    slot_index = static_cast<int>(i);
                    clients[i].in_use = true;
                    break;
                }
            }
        }

        // Join outside the mutex so the worker can finish
        // without blocking on clients_mutex.
        if (completed_thread.joinable()) {
            completed_thread.join();
        }

        if (slot_index == -1) {
            int enabled = 1;

            setsockopt(
                client_fd,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)
            );

            send_all(client_fd, "ERR server_busy\n");
            close(client_fd);

            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout
                << "Client rejected: connection limit reached\n";

            continue;
        }

        try {
            clients[slot_index].worker = std::thread(
                handle_client,
                client_fd,
                std::ref(floor),
                std::ref(clients[slot_index])
            );
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                clients[slot_index].in_use = false;
                clients[slot_index].finished = false;
            }

            close(client_fd);

            std::cerr << "Failed to start client thread: "
                      << error.what() << '\n';
        }
    }

    close(server_fd);
    return 0;
}