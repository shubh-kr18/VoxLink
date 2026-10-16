
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace aura {

struct FloorGrant {
    std::string user;
    std::uint64_t token;
    std::chrono::steady_clock::time_point expires_at;
};

class FloorManager {
public:
    std::optional<FloorGrant> acquire(
        const std::string& channel,
        const std::string& user,
        std::chrono::milliseconds lease =
            std::chrono::seconds(5));

    bool release(
        const std::string& channel,
        const std::string& user,
        std::uint64_t token);

    std::optional<std::string> owner(
        const std::string& channel);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, FloorGrant> owners_;
    std::uint64_t next_token_ = 1;
};

} // namespace aura
