// Selection and run-state events, and the studio's Move tool built on them.

#include "support.hpp"

#include "Folder.hpp"
#include "SelectionService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

std::vector<std::string> lines_of(engine_core::ScriptRuntime& runtime) {
    std::vector<std::string> out;
    for (const auto& line : runtime.drain_output().lines) {
        out.push_back(line.text);
    }
    return out;
}

InstanceId add_part(engine_core::DataModel& game, const char* name) {
    engine_core::GameObject& part = game.create_game_object();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), game.scene_service("Workspace"));
    return part.id();
}

}  // namespace

TEST_CASE("EV1 SelectionChanged fires once per change, and Get sees the new list", "[EV1]") {
    ScriptRig rig;
    const InstanceId part = add_part(rig.game, "Part");
    add_script(rig.game, rig.game.core(), "Watch", R"(
        local Selection = game:GetService("Selection")
        Selection.SelectionChanged:Connect(function(...)
            print("changed", select("#", ...), #Selection:Get())
        end)
    )");
    rig.frames(1);
    lines_of(rig.runtime);
    rig.game.selection().set({part});
    rig.frames(1);
    rig.game.selection().set({part});
    rig.frames(1);
    rig.game.selection().set({});
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(lines_of(rig.runtime) == std::vector<std::string>{"changed\t0\t1\n", "changed\t0\t0\n"});
}

TEST_CASE("EV2 Started fires at Play and Stopped after Stop has restored the place", "[EV2]") {
    ScriptRig rig;
    const InstanceId part = add_part(rig.game, "Part");
    add_script(rig.game, rig.game.core(), "Watch", R"(
        local RunService = game:GetService("RunService")
        RunService.Started:Connect(function() print("started", RunService:IsRunning()) end)
        RunService.Stopped:Connect(function()
            print("stopped", RunService:IsRunning(), workspace:FindFirstChild("Part") ~= nil)
        end)
    )");
    rig.frames(1);
    lines_of(rig.runtime);
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    rig.game.set_name(part, "Renamed");
    rig.game.stop_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(lines_of(rig.runtime) == std::vector<std::string>{"started\ttrue\n", "stopped\tfalse\ttrue\n"});
    // A second session: the plugin's connections outlived the first.
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    rig.game.stop_simulation();
    rig.frames(1);
    REQUIRE(lines_of(rig.runtime) == std::vector<std::string>{"started\ttrue\n", "stopped\tfalse\ttrue\n"});
}
