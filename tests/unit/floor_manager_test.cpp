
#include "aura/floor_manager.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    aura::FloorManager floor;

    auto alice = floor.acquire("ece-2", "alice", 5s);
    assert(alice.has_value());

    assert(!floor.acquire("ece-2", "bob").has_value());

    auto retry = floor.acquire("ece-2", "alice");
    assert(retry.has_value());
    assert(retry->token == alice->token);

    assert(!floor.release("ece-2", "bob", alice->token));
    assert(!floor.release("ece-2", "alice", alice->token + 1));
    assert(floor.owner("ece-2") == "alice");

    assert(floor.release("ece-2", "alice", alice->token));
    assert(!floor.owner("ece-2").has_value());

    auto bob = floor.acquire("ece-2", "bob");
    assert(bob.has_value());

    auto alice_other = floor.acquire("ece-3", "alice");
    assert(alice_other.has_value());

    assert(floor.owner("ece-2") == "bob");
    assert(floor.owner("ece-3") == "alice");

    assert(!floor.acquire("", "alice").has_value());
    assert(!floor.acquire("ece-2", "").has_value());
    assert(!floor.acquire("ece-2", "alice", 0ms).has_value());

    aura::FloorManager expiry_floor;

    auto old_grant = expiry_floor.acquire(
        "expiry-channel", "alice", 30ms);

    assert(old_grant.has_value());

    std::this_thread::sleep_for(60ms);

    auto new_grant = expiry_floor.acquire(
        "expiry-channel", "bob", 5s);

    assert(new_grant.has_value());
    assert(new_grant->user == "bob");

    assert(!expiry_floor.release(
        "expiry-channel", "alice", old_grant->token));

    assert(expiry_floor.owner("expiry-channel") == "bob");

    constexpr int thread_count = 100;

    aura::FloorManager contested_floor;

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> successful_grants{0};

    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&, i]() {
            ready.fetch_add(1);

            while (!start.load()) {
                std::this_thread::yield();
            }

            auto grant = contested_floor.acquire(
                "busy-channel",
                "user-" + std::to_string(i),
                5s);

            if (grant.has_value()) {
                successful_grants.fetch_add(1);
            }
        });
    }

    while (ready.load() != thread_count) {
        std::this_thread::yield();
    }

    start.store(true);

    for (auto& thread : threads) {
        thread.join();
    }

    assert(successful_grants.load() == 1);

    auto current_owner = contested_floor.owner("busy-channel");
    assert(current_owner.has_value());

    auto current_grant = contested_floor.acquire(
        "busy-channel", *current_owner);

    assert(current_grant.has_value());

    assert(contested_floor.release(
        "busy-channel",
        current_grant->user,
        current_grant->token));

    auto next_grant = contested_floor.acquire(
        "busy-channel", "next-user", 5s);

    assert(next_grant.has_value());
    assert(next_grant->user == "next-user");

    std::cout << "All FloorManager tests passed!\n";
    std::cout << "Concurrent acquisition winners: "
              << successful_grants.load()
              << " / " << thread_count << '\n';

    return 0;
}
