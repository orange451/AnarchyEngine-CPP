// Selection and run-state events, and the studio's Move tool built on them.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Dragger.hpp"
#include "Folder.hpp"
#include "PhysicsObject.hpp"
#include "SnapshotPump.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "UserInputService.hpp"
#include "ide/PluginLoader.hpp"
#include "SelectionService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
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

// Where the Move tool's handles sit, or nullopt when it shows none.
std::optional<engine_core::Vec3> handles_at(engine_core::DataModel& game) {
    const engine_core::Dragger* dragger = move_dragger(game);
    if (dragger == nullptr) {
        return std::nullopt;
    }
    const engine_core::Matrix4 at = dragger->transform();
    return engine_core::Vec3{at.m[12], at.m[13], at.m[14]};
}

bool near(float a, float b) { return std::abs(a - b) <= 1e-3f; }

// The Move tool loaded, and a camera at the origin looking down -Z with a 90
// degree view, 200 x 200 points: handles at (0, 0, -10) put the X arrow along
// screen y = 100 from x = 100 to 200, and 20 points drag 2 studs.
struct MoveRig {
    ScriptRig rig;
    ide::PluginLoader loader;

    MoveRig() {
        engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
        rig.game.set_parent(camera.id(), rig.game.scene_service("Workspace"));
        camera.set_field_of_view(90);
        camera.set_viewport_size(engine_core::Vec2{200, 200});
        dynamic_cast<engine_core::Workspace*>(rig.game.instance(rig.game.scene_service("Workspace")))
            ->set_current_camera(camera.id());
        rig.game.history().end_gesture();
        rig.game.history().reset_waypoints();
        REQUIRE(loader.load(rig.game, rig.runtime, {move_tool_file()}) == 1);
        rig.frames(1);
    }
    InstanceId part_at(const char* name, float x, float y, float z) {
        const InstanceId id = add_part(rig.game, name);
        rig.game.game_object(id)->set_transform(engine_core::matrix4_translation(x, y, z));
        rig.game.history().end_gesture();
        rig.game.history().reset_waypoints();
        return id;
    }
    float x_of(InstanceId id) { return rig.game.game_object(id)->transform().m[12]; }
    void post(bool down, float x, float y) {
        rig.game.input().post_mouse_button(0, down, x, y);
        rig.frames(1);
    }
    void move(float x, float y) {
        rig.game.input().post_mouse_move(x, y);
        rig.frames(1);
    }
};

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

TEST_CASE("MT1 the Move tool puts its handles at the middle of the selected PVInstances", "[MT1]") {
    MoveRig move;
    const InstanceId a = move.part_at("A", -2, 0, -10);
    const InstanceId b = move.part_at("B", 2, 4, -10);
    REQUIRE_FALSE(handles_at(move.rig.game));
    move.rig.game.selection().set({a, b});
    move.rig.frames(1);
    INFO(move.rig.runtime.last_error());
    auto at = handles_at(move.rig.game);
    REQUIRE(at);
    REQUIRE((near(at->x, 0) && near(at->y, 2) && near(at->z, -10)));
    move.rig.game.selection().set({b});
    move.rig.frames(1);
    at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 2) && near(at->y, 4)));
    move.rig.game.selection().set({});
    move.rig.frames(1);
    REQUIRE_FALSE(handles_at(move.rig.game));
    move.rig.game.history().end_gesture();
    REQUIRE_FALSE(move.rig.game.history().can_undo().first);
}

TEST_CASE("MT2 a selection with no PVInstance gets no handles", "[MT2]") {
    MoveRig move;
    engine_core::Folder& folder = move.rig.game.create<engine_core::Folder>();
    move.rig.game.set_parent(folder.id(), move.rig.game.scene_service("Workspace"));
    move.rig.game.selection().set({folder.id()});
    move.rig.frames(1);
    REQUIRE_FALSE(handles_at(move.rig.game));
}

TEST_CASE("MT3 the Move tool lets go during play and takes the selection back after Stop", "[MT3]") {
    MoveRig move;
    const InstanceId a = move.part_at("A", 1, 0, -10);
    move.rig.game.selection().set({a});
    move.rig.frames(1);
    REQUIRE(handles_at(move.rig.game));
    move.rig.game.capture_place();
    move.rig.game.start_simulation();
    move.rig.frames(1);
    REQUIRE_FALSE(handles_at(move.rig.game));
    move.rig.game.stop_simulation();
    move.rig.frames(1);
    const auto at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 1)));
}

TEST_CASE("MT4 New and Open clear the selection, so the Move tool lets go", "[MT4][project]") {
    MoveRig move;
    TempDir dir;
    engine_core::Project project = engine_core::Project::create(dir.path, move.rig.game);
    const InstanceId a = move.part_at("A", 0, 0, -10);
    project.save();
    move.rig.game.selection().set({a});
    move.rig.frames(1);
    REQUIRE(handles_at(move.rig.game));
    engine_core::Project reopened = engine_core::Project::load(dir.path, move.rig.game);
    move.rig.frames(1);
    REQUIRE(move.rig.game.selection().get().empty());
    REQUIRE_FALSE(handles_at(move.rig.game));
}

TEST_CASE("MT5 a selected PhysicsObject gets handles, and the snapshot carries them", "[MT5]") {
    MoveRig move;
    engine_core::PhysicsObject& body = move.rig.game.create<engine_core::PhysicsObject>();
    move.rig.game.set_parent(body.id(), move.rig.game.scene_service("Workspace"));
    REQUIRE_FALSE(body.set_transform(engine_core::matrix4_translation(3, 0, -10)).has_value());
    move.rig.game.selection().set({body.id()});
    move.rig.frames(1);
    const auto at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 3)));
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(move.rig.game);
    pump.publish();
    REQUIRE(pump.front().draggers.size() == 1);
}

TEST_CASE("MT6 one drag moves every selected PVInstance and the handles alike, and one undo puts them back",
          "[MT6]") {
    MoveRig move;
    const InstanceId a = move.part_at("A", -2, 0, -10);
    const InstanceId b = move.part_at("B", 2, 0, -10);
    move.rig.game.selection().set({a, b});
    move.rig.frames(1);
    move.post(true, 150, 100);
    move.move(160, 100);
    move.move(170, 100);
    move.post(false, 170, 100);
    move.rig.frames(1);
    INFO(move.rig.runtime.last_error());
    REQUIRE(near(move.x_of(a), 0));
    REQUIRE(near(move.x_of(b), 4));
    auto at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 2)));
    REQUIRE(move.rig.game.history().can_undo().second == "Move");
    move.rig.game.history().undo();
    move.rig.frames(1);
    REQUIRE(near(move.x_of(a), -2));
    REQUIRE(near(move.x_of(b), 2));
    // The handles follow the selection back.
    at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 0)));
    REQUIRE_FALSE(move.rig.game.history().can_undo().first);
}

TEST_CASE("MT7 selecting a Folder of parts puts the handles at their middle, and one drag moves them all", "[MT7]") {
    MoveRig move;
    engine_core::Folder& folder = move.rig.game.create<engine_core::Folder>();
    move.rig.game.set_parent(folder.id(), move.rig.game.scene_service("Workspace"));
    const InstanceId a = move.part_at("A", -2, 0, -10);
    const InstanceId b = move.part_at("B", 0, 0, -10);
    const InstanceId c = move.part_at("C", 2, 0, -10);
    for (InstanceId id : {a, b, c}) {
        move.rig.game.set_parent(id, folder.id());
    }
    move.rig.game.history().end_gesture();
    move.rig.game.history().reset_waypoints();
    move.rig.game.selection().set({folder.id()});
    move.rig.frames(1);
    INFO(move.rig.runtime.last_error());
    const auto at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 0) && near(at->z, -10)));
    move.post(true, 150, 100);
    move.move(170, 100);
    move.post(false, 170, 100);
    move.rig.frames(1);
    REQUIRE(near(move.x_of(a), 0));
    REQUIRE(near(move.x_of(b), 2));
    REQUIRE(near(move.x_of(c), 4));
    move.rig.game.history().undo();
    REQUIRE(near(move.x_of(a), -2));
    REQUIRE(near(move.x_of(c), 2));
}

TEST_CASE("MT8 selecting Workspace never moves the camera the view looks through", "[MT8]") {
    MoveRig move;
    const InstanceId a = move.part_at("A", -2, 0, -10);
    const InstanceId b = move.part_at("B", 2, 0, -10);
    const InstanceId camera =
        dynamic_cast<engine_core::Workspace*>(move.rig.game.instance(move.rig.game.scene_service("Workspace")))
            ->current_camera();
    move.rig.game.selection().set({move.rig.game.scene_service("Workspace")});
    move.rig.frames(1);
    INFO(move.rig.runtime.last_error());
    // The camera at the origin is left out of the middle too.
    const auto at = handles_at(move.rig.game);
    REQUIRE((at && near(at->x, 0) && near(at->z, -10)));
    move.post(true, 150, 100);
    move.move(170, 100);
    move.post(false, 170, 100);
    move.rig.frames(1);
    REQUIRE(near(move.x_of(a), 0));
    REQUIRE(near(move.x_of(b), 4));
    REQUIRE(near(move.rig.game.game_object(camera)->transform().m[12], 0));
}

TEST_CASE("MT9 a part both selected and inside a selected Folder moves once", "[MT9]") {
    MoveRig move;
    engine_core::Folder& folder = move.rig.game.create<engine_core::Folder>();
    move.rig.game.set_parent(folder.id(), move.rig.game.scene_service("Workspace"));
    const InstanceId a = move.part_at("A", 0, 0, -10);
    move.rig.game.set_parent(a, folder.id());
    move.rig.game.selection().set({a, folder.id()});
    move.rig.frames(1);
    move.post(true, 150, 100);
    move.move(170, 100);
    move.post(false, 170, 100);
    move.rig.frames(1);
    REQUIRE(near(move.x_of(a), 2));
}

TEST_CASE("MT10 deleting the selected part takes it out of the selection, so the handles go", "[MT10]") {
    MoveRig move;
    const InstanceId a = move.part_at("A", 1, 0, -10);
    move.rig.game.selection().set({a});
    move.rig.frames(1);
    REQUIRE(handles_at(move.rig.game));
    const std::uint64_t before = move.rig.game.selection().revision();
    move.rig.game.destroy_tree(a);
    move.rig.frames(1);
    INFO(move.rig.runtime.last_error());
    REQUIRE(move.rig.game.selection().get().empty());
    REQUIRE(move.rig.game.selection().revision() != before);
    REQUIRE_FALSE(handles_at(move.rig.game));
}
