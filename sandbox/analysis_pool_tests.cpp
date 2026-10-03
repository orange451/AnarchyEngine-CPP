#include "AnalysisPool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kStack = std::size_t{1} << 20;

bool wait_for(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

TEST_CASE("APL1 run_all runs every task once and returns after the last", "[APL1]") {
    engine_core::AnalysisPool pool(3, kStack);
    std::atomic<int> ran{0};
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back([&ran] { ran.fetch_add(1); });
    }
    pool.run_all(std::move(tasks));
    REQUIRE(ran.load() == 100);
}

TEST_CASE("APL2 tasks run on several threads at once", "[APL2]") {
    engine_core::AnalysisPool pool(4, kStack);
    REQUIRE(pool.size() == 4);
    std::atomic<int> arrived{0};
    std::atomic<int> saw_all{0};
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 4; ++i) {
        // Each waits for all four, which only happens if all four run together.
        tasks.push_back([&arrived, &saw_all] {
            arrived.fetch_add(1);
            if (wait_for([&arrived] { return arrived.load() == 4; })) {
                saw_all.fetch_add(1);
            }
        });
    }
    pool.run_all(std::move(tasks));
    REQUIRE(arrived.load() == 4);
    REQUIRE(saw_all.load() == 4);
}

TEST_CASE("APL3 post returns before its tasks finish", "[APL3]") {
    engine_core::AnalysisPool pool(1, kStack);
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    pool.post({[&] {
        wait_for([&release] { return release.load(); });
        finished.store(true);
    }});
    REQUIRE_FALSE(finished.load());
    release.store(true);
    REQUIRE(wait_for([&finished] { return finished.load(); }));
}

TEST_CASE("APL4 the default size leaves a core free and is at least one", "[APL4]") {
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned expected = hardware > 1 ? hardware - 1 : 1;
    REQUIRE(engine_core::AnalysisPool::default_size() == expected);
    engine_core::AnalysisPool pool(0, kStack);
    REQUIRE(pool.size() == 1);
}
