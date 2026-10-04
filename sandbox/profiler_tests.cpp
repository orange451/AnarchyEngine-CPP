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
    // Paced as the studio's Runner paces it. Unpaced, an empty place's step loop
    // spins, and a render loop with no window to wait on does too.
    engine.set_simulation_pace_hz(60.0);
    engine.set_render_pace_hz(60.0);
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

namespace {

// The test thread records as Sim, once.
void record_here() {
    static thread_local bool registered = false;
    if (!registered) {
        profiler::register_thread("Sim");
        registered = true;
    }
}

// Steps the rig a frame at a time, marking a frame boundary before each, then collects.
profiler::History run_frames(ScriptRig& rig, int count) {
    for (int index = 0; index < count; ++index) {
        profiler::frame_boundary();
        rig.frames(1);
    }
    profiler::frame_boundary();
    profiler::collect();
    return live_history();
}

std::set<std::string> causes_of(const profiler::History& history, const std::string& name) {
    std::set<std::string> out;
    for (const profiler::ScopeRecord& record : scopes_named(history, name)) {
        if (record.cause != profiler::kNoCause && record.cause < history.causes.size()) {
            out.insert(history.causes[record.cause]);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("PF2 a Script's resumes are scopes named after it, with what resumed them", "[PF2]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    add_script(rig.game, "Mover",
               "game:GetService('RunService').Heartbeat:Connect(function() end)\n"
               "while true do task.wait() end");
    rig.game.start_simulation();
    const profiler::History history = run_frames(rig, 4);
    const std::set<std::string> causes = causes_of(history, "Mover");
    REQUIRE(causes.count("start") == 1);
    REQUIRE(causes.count("Heartbeat") == 1);
    REQUIRE(causes.count("wait") == 1);
    for (const profiler::ScopeRecord& record : scopes_named(history, "Mover")) {
        REQUIRE(history.scopes[record.scope].group == profiler::Group::Script);
        REQUIRE(history.scopes[record.scope].key == rig.game.guid(
                                                        rig.game.find_first_child(rig.game.scene_service("Workspace"), "Mover")));
    }
}

TEST_CASE("PF3 a renamed Script keeps its scope, under the new name", "[PF3]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "Before", "while true do task.wait() end");
    rig.game.start_simulation();
    run_frames(rig, 2);
    rig.game.set_name(script.id(), "After");
    const profiler::History history = run_frames(rig, 2);
    const auto after = scopes_named(history, "After");
    REQUIRE_FALSE(after.empty());
    REQUIRE(scopes_named(history, "Before").empty());
}

TEST_CASE("PF4 require records the ModuleScript inside the Script that asked", "[PF4]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Lib");
    module.set_source("local t = 0 for i = 1, 1000 do t = t + i end return t");
    rig.game.set_parent(module.id(), rig.game.scene_service("Workspace"));
    add_script(rig.game, "User", "local lib = require(workspace.Lib)");
    rig.game.start_simulation();
    const profiler::History history = run_frames(rig, 2);
    const auto users = scopes_named(history, "User");
    const auto libs = scopes_named(history, "Lib");
    REQUIRE(users.size() == 1);
    REQUIRE(libs.size() == 1);
    REQUIRE(libs[0].depth == users[0].depth + 1);
    REQUIRE(causes_of(history, "Lib") == std::set<std::string>{"require"});
}

namespace {

// How many printed lines contain text.
int printed(ScriptRig& rig, const std::string& text) {
    int count = 0;
    for (const auto& line : rig.runtime.drain_output().lines) {
        count += line.text.find(text) != std::string::npos ? 1 : 0;
    }
    return count;
}

}  // namespace

TEST_CASE("PF5 debug.profilebegin marks a scope inside the Script's own", "[PF5]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    add_script(rig.game, "Enemy",
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "  debug.profilebegin('pathfind')\n"
               "  debug.profilebegin('inner')\n"
               "  debug.profileend()\n"
               "  debug.profileend()\n"
               "end)");
    rig.game.start_simulation();
    const profiler::History history = run_frames(rig, 3);
    const auto enemy = scopes_named(history, "Enemy");
    const auto pathfind = scopes_named(history, "pathfind");
    const auto inner = scopes_named(history, "inner");
    REQUIRE_FALSE(pathfind.empty());
    REQUIRE(inner.size() == pathfind.size());
    REQUIRE(history.scopes[pathfind[0].scope].group == profiler::Group::User);
    std::uint8_t heartbeat_depth = 255;
    for (const profiler::ScopeRecord& record : enemy) {
        if (record.cause != profiler::kNoCause && history.causes[record.cause] == "Heartbeat") {
            heartbeat_depth = record.depth;
        }
    }
    REQUIRE(pathfind[0].depth == heartbeat_depth + 1);
    REQUIRE(inner[0].depth == heartbeat_depth + 2);
    REQUIRE(printed(rig, "Warning") == 0);
}

TEST_CASE("PF6 a scope open at a yield closes there and warns once", "[PF6]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    add_script(rig.game, "Looper",
               "while true do\n"
               "  debug.profilebegin('frame work')\n"
               "  task.wait()\n"
               "  debug.profileend()\n"
               "end");
    rig.game.start_simulation();
    rig.runtime.drain_output();
    const profiler::History history = run_frames(rig, 6);
    const auto work = scopes_named(history, "frame work");
    REQUIRE(work.size() >= 3);
    // Each is closed when the Script yields, inside the Script's scope.
    const auto looper = scopes_named(history, "Looper");
    REQUIRE_FALSE(looper.empty());
    for (const profiler::ScopeRecord& record : work) {
        REQUIRE(record.depth == looper[0].depth + 1);
    }
    // The yield closed it, so the profileend after each wait has nothing to end: one
    // warning for each kind, however many frames repeat it.
    REQUIRE(printed(rig, "Warning: Looper") == 2);
}

TEST_CASE("PF7 profileend with nothing open warns once", "[PF7]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    add_script(rig.game, "Stray", "while true do debug.profileend() task.wait() end");
    rig.game.start_simulation();
    rig.runtime.drain_output();
    run_frames(rig, 4);
    REQUIRE(printed(rig, "no debug.profilebegin") == 1);
}

TEST_CASE("PF8 scope names: text only, cut to 64 bytes, 256 a Script", "[PF8]") {
    record_here();
    Recording recording;
    ScriptRig rig;
    add_script(rig.game, "Names",
               "local ok = pcall(debug.profilebegin, 5)\n"
               "print('number ok', ok)\n"
               "debug.profilebegin(string.rep('n', 100)) debug.profileend()\n"
               "for i = 1, 257 do debug.profilebegin('s' .. i) debug.profileend() end");
    rig.game.start_simulation();
    profiler::frame_boundary();
    rig.frames(1);
    profiler::frame_boundary();
    profiler::collect();
    const profiler::History history = live_history();
    REQUIRE(printed(rig, "number ok\tfalse") == 1);
    REQUIRE(scopes_named(history, std::string(64, 'n')).size() == 1);
    // The long name and s1 to s255 are the 256; s256 and s257 are past it.
    REQUIRE(scopes_named(history, "s255").size() == 1);
    REQUIRE(scopes_named(history, "s256").empty());
    REQUIRE(scopes_named(history, "s257").empty());
    REQUIRE(scopes_named(history, "(too many scopes)").size() == 2);
}

TEST_CASE("PF9 debug holds only the profiler's two functions", "[PF9]") {
    ScriptRig rig;
    add_script(rig.game, "Probe",
               "print('debug', type(debug), type(debug.profilebegin), type(debug.profileend), "
               "debug.traceback == nil, debug.getinfo == nil, debug.info == nil)");
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(printed(rig, "debug\ttable\tfunction\tfunction\ttrue\ttrue\ttrue") == 1);
}
