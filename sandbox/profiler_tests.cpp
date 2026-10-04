// The frame profiler as the engine and the script runtime feed it: engine
// phases on the Sim and Render rows, each Script's resumes with their cause, and
// the scopes scripts mark with debug.profilebegin.

#include "support.hpp"

#include "Engine.hpp"
#include "ModuleScript.hpp"
#include "profiler/Profiler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

profiler::History live_history() {
    profiler::History out;
    profiler::with_live([&](const profiler::History& history) { out = history; });
    return out;
}

// Every recorded scope with this name.
std::vector<profiler::ScopeRecord> scopes_named(const profiler::History& history, const std::string& name) {
    std::vector<profiler::ScopeRecord> out;
    for (const profiler::Frame& frame : history.frames) {
        for (const profiler::ScopeRecord& record : frame.scopes) {
            if (record.scope < history.scopes.size() && history.scopes[record.scope].name == name) {
                out.push_back(record);
            }
        }
    }
    return out;
}

bool has_row(const profiler::History& history, const char* row) {
    for (const std::string& name : history.rows) {
        if (name == row) {
            return true;
        }
    }
    return false;
}

// Recording on for the test, and off and empty after it.
struct Recording {
    Recording() {
        profiler::reset_for_testing();
        profiler::acquire();
    }
    ~Recording() { profiler::reset_for_testing(); }
};

}  // namespace

TEST_CASE("PF1 a running engine records frames with Sim and Render scopes", "[PF1]") {
    Recording recording;
    engine_core::Engine engine;
    engine.start();
    engine.resume();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        profiler::collect();
    }
    engine.stop();
    profiler::collect();
    const profiler::History history = live_history();
    REQUIRE(history.frames.size() >= 5);
    REQUIRE(has_row(history, "Sim"));
    REQUIRE(has_row(history, "Render"));
    REQUIRE_FALSE(scopes_named(history, "Simulation step").empty());
    REQUIRE_FALSE(scopes_named(history, "Heartbeat").empty());
    REQUIRE_FALSE(scopes_named(history, "Prepare").empty());
    REQUIRE_FALSE(scopes_named(history, "PostRender").empty());
    // Steps nest: Heartbeat is inside the simulation step.
    for (const profiler::ScopeRecord& record : scopes_named(history, "Heartbeat")) {
        REQUIRE(record.depth >= 1);
    }
}
