// Selection and run-state events, and the studio's Move tool built on them.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "Dragger.hpp"
#include "Folder.hpp"
#include "Project.hpp"
#include "ide/PluginLoader.hpp"
#include "SelectionService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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

ide::PluginFile move_tool_file() {
    ide::PluginFile file;
    std::string error;
    REQUIRE(ide::read_plugin_file(std::filesystem::path(ANARCHY_SOURCE_DIR) / "resources/plugins/MoveTool.luau", file,
                                  error));
    return file;
}

// The Move tool's Dragger in Core, or null when it has made none.
engine_core::Dragger* move_dragger(engine_core::DataModel& game) {
    for (InstanceId child : game.get_children(game.core())) {
        if (auto* dragger = dynamic_cast<engine_core::Dragger*>(game.instance(child))) {
            return dragger;
        }
    }
    return nullptr;
}

InstanceId move_target(engine_core::DataModel& game) {
    const engine_core::Dragger* dragger = move_dragger(game);
    return dragger != nullptr ? dragger->target() : 0;
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

TEST_CASE("MT1 the Move tool puts handles on the first selected PVInstance, and follows the selection", "[MT1]") {
    ScriptRig rig;
    const InstanceId a = add_part(rig.game, "A");
    const InstanceId b = add_part(rig.game, "B");
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {move_tool_file()}) == 1);
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(move_target(rig.game) == 0);
    rig.game.selection().set({a, b});
    rig.frames(1);
    REQUIRE(move_target(rig.game) == a);
    rig.game.selection().set({b});
    rig.frames(1);
    REQUIRE(move_target(rig.game) == b);
    rig.game.selection().set({});
    rig.frames(1);
    REQUIRE(move_target(rig.game) == 0);
    // One Dragger the whole time, and none of it is an undo step.
    int draggers = 0;
    for (InstanceId child : rig.game.get_children(rig.game.core())) {
        draggers += dynamic_cast<engine_core::Dragger*>(rig.game.instance(child)) != nullptr ? 1 : 0;
    }
    REQUIRE(draggers == 1);
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
}

TEST_CASE("MT2 a selection that is not a PVInstance gets no handles", "[MT2]") {
    ScriptRig rig;
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(folder.id(), rig.game.scene_service("Workspace"));
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {move_tool_file()}) == 1);
    rig.game.selection().set({folder.id()});
    rig.frames(1);
    REQUIRE(move_target(rig.game) == 0);
}

TEST_CASE("MT3 the Move tool lets go during play and takes the selection back after Stop", "[MT3]") {
    ScriptRig rig;
    const InstanceId a = add_part(rig.game, "A");
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {move_tool_file()}) == 1);
    rig.game.selection().set({a});
    rig.frames(1);
    REQUIRE(move_target(rig.game) == a);
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(move_target(rig.game) == 0);
    rig.game.stop_simulation();
    rig.frames(1);
    REQUIRE(move_target(rig.game) == a);
}

TEST_CASE("MT4 New and Open clear the selection, so the Move tool lets go", "[MT4][project]") {
    ScriptRig rig;
    TempDir dir;
    engine_core::Project project = engine_core::Project::create(dir.path, rig.game);
    const InstanceId a = add_part(rig.game, "A");
    project.save();
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {move_tool_file()}) == 1);
    rig.game.selection().set({a});
    rig.frames(1);
    REQUIRE(move_target(rig.game) == a);
    // Reopening the same project brings back an instance with A's GUID.
    engine_core::Project reopened = engine_core::Project::load(dir.path, rig.game);
    rig.frames(1);
    REQUIRE(rig.game.selection().get().empty());
    REQUIRE(move_target(rig.game) == 0);
    rig.game.selection().set({rig.game.find_first_child(rig.game.scene_service("Workspace"), "A")});
    rig.frames(1);
    REQUIRE(move_target(rig.game) != 0);
    engine_core::Project::reset_place(rig.game);
    rig.frames(1);
    REQUIRE(rig.game.selection().get().empty());
    REQUIRE(move_target(rig.game) == 0);
}
