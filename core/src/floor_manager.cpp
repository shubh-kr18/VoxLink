
#include "aura/floor_manager.hpp"

namespace aura {

std::optional<FloorGrant> FloorManager::acquire(
    const std::string& channel,
    const std::string& user,
    std::chrono::milliseconds lease) {

    if (channel.empty() || user.empty() ||
        lease <= std::chrono::milliseconds::zero()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto it = owners_.find(channel);

    if (it != owners_.end()) {
        if (it->second.expires_at <= now) {
            owners_.erase(it);
        } else {
            if (it->second.user == user) {
                return it->second;
            }

            return std::nullopt;
        }
    }

    FloorGrant grant{
        user,
        next_token_++,
        now + lease
    };

    owners_.emplace(channel, grant);

    return grant;
}

bool FloorManager::release(
    const std::string& channel,
    const std::string& user,
    std::uint64_t token) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = owners_.find(channel);

    if (it == owners_.end()) {
        return false;
    }

    if (it->second.expires_at <=
        std::chrono::steady_clock::now()) {
        owners_.erase(it);
        return false;
    }

    if (it->second.user != user ||
        it->second.token != token) {
        return false;
    }

    owners_.erase(it);
    return true;
}

std::optional<std::string> FloorManager::owner(
    const std::string& channel) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = owners_.find(channel);

    if (it == owners_.end()) {
        return std::nullopt;
    }

    if (it->second.expires_at <=
        std::chrono::steady_clock::now()) {
        owners_.erase(it);
        return std::nullopt;
    }

    return it->second.user;
}

} // namespace aura
