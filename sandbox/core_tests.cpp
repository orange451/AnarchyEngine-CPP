// Core: the studio's own service, outside the place.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "Folder.hpp"
#include "PhysicsObject.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "Script.hpp"
#include "SnapshotPump.hpp"
#include "ide/PluginLoader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using engine_core::ContractViolation;
using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const char* name, InstanceId parent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    if (parent != engine_core::DataModel::kNoParent) {
        game.set_parent(folder.id(), parent);
    }
    return folder.id();
}

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("CO9 a Game holds Core last, hidden from the explorer, and it cannot be moved, renamed, or destroyed",
          "[CO9]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    REQUIRE(core != 0);
    REQUIRE(game.parent(core) == 0);
    REQUIRE(game.get_children(0).back() == core);
    REQUIRE(std::string(game.instance(core)->class_name()) == "Core");
    REQUIRE(game.name(core) == "Core");
    REQUIRE(game.guid(core) == "core");
    REQUIRE(game.instance(core)->hidden_in_explorer());
    REQUIRE(game.instance(core)->is_service());
    REQUIRE_FALSE(game.instance(core)->is_scene_service());
    REQUIRE(game.scene_service("Core") == 0);
    REQUIRE_THROWS_AS(game.set_parent(core, game.scene_service("Workspace")), ContractViolation);
    REQUIRE_THROWS_AS(game.set_name(core, "Tools"), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy(core), ContractViolation);
    // What Workspace may hold, Core may hold.
    REQUIRE_NOTHROW(add_folder(game, "Tools", core));
}

TEST_CASE("CO2a Core's descendants are in Core, and Core holds itself and them", "[CO2a]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId inner = add_folder(game, "Inner", tools);
    const InstanceId place = add_folder(game, "Place", game.scene_service("Workspace"));
    REQUIRE(game.in_core(tools));
    REQUIRE(game.in_core(inner));
    REQUIRE_FALSE(game.in_core(core));
    REQUIRE(game.core_holds(core));
    REQUIRE(game.core_holds(inner));
    REQUIRE_FALSE(game.core_holds(place));
    REQUIRE_FALSE(game.core_holds(0));
    REQUIRE(game.in_game(inner));
    REQUIRE_FALSE(game.in_workspace(inner));
}

TEST_CASE("CO5 nothing moves across Core's edge, but an instance with no parent may go in", "[CO5]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId other = add_folder(game, "Other", core);
    const InstanceId part = add_folder(game, "Part", workspace);
    // Out of Core, to the place or to no parent.
    REQUIRE(game.parent_error(tools, workspace).has_value());
    REQUIRE(game.parent_error(tools, engine_core::DataModel::kNoParent).has_value());
    REQUIRE_THROWS_AS(game.set_parent(tools, workspace), ContractViolation);
    // From the place into Core.
    REQUIRE(game.parent_error(part, core).has_value());
    REQUIRE_THROWS_AS(game.set_parent(part, tools), ContractViolation);
    // Within Core.
    REQUIRE_NOTHROW(game.set_parent(tools, other));
    // An instance with no parent, including one that left the place.
    const InstanceId loose = add_folder(game, "Loose", engine_core::DataModel::kNoParent);
    REQUIRE_NOTHROW(game.set_parent(loose, core));
    game.set_parent(part, engine_core::DataModel::kNoParent);
    REQUIRE_NOTHROW(game.set_parent(part, core));
    REQUIRE(game.in_core(part));
}

TEST_CASE("CO1 New and Open leave Core's instances with their ids, names, and children", "[CO1][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId inner = add_folder(game, "Inner", tools);

    engine_core::Project::reset_place(game);
    REQUIRE(game.core() == core);
    REQUIRE(game.alive(tools));
    REQUIRE(game.parent(inner) == tools);
    REQUIRE(game.name(inner) == "Inner");

    engine_core::Project reopened = engine_core::Project::load(dir.path, game);
    REQUIRE(game.core() == core);
    REQUIRE(game.alive(inner));
    REQUIRE(game.parent(tools) == core);
    REQUIRE(game.get_children(0).back() == core);
}

TEST_CASE("CO2 what changes in Core during play is still there after Stop", "[CO2]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId place = add_folder(game, "Place", game.scene_service("Workspace"));
    game.start_simulation();
    game.set_name(tools, "Renamed");
    const InstanceId made = add_folder(game, "Made", core);
    game.set_name(place, "PlayName");
    const InstanceId play_only = add_folder(game, "PlayOnly", game.scene_service("Workspace"));
    game.stop_simulation();
    REQUIRE(game.name(tools) == "Renamed");
    REQUIRE(game.alive(made));
    REQUIRE(game.parent(made) == core);
    REQUIRE(game.in_core(made));
    // The place outside Core is restored as before.
    REQUIRE(game.name(place) == "Place");
    REQUIRE_FALSE(game.alive(play_only));
    REQUIRE(game.get_children(0).back() == core);
}

TEST_CASE("CO2b a Core instance made after a captured one is destroyed in play survives Stop", "[CO2b]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId doomed = add_folder(game, "Doomed", game.scene_service("Workspace"));
    game.start_simulation();
    game.destroy(doomed);
    const InstanceId made = add_folder(game, "Made", core);
    game.stop_simulation();
    REQUIRE(game.alive(made));
    REQUIRE(game.parent(made) == core);
    REQUIRE(game.alive(doomed));
    REQUIRE(game.name(doomed) == "Doomed");
}

TEST_CASE("CO3 Core is never saved and never makes the place unsaved", "[CO3][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    REQUIRE_FALSE(project.unsaved());
    const std::uint64_t before = engine_core::Project::place_fingerprint(game);
    add_folder(game, "Tools", game.core());
    REQUIRE_FALSE(project.unsaved());
    REQUIRE(engine_core::Project::place_fingerprint(game) == before);
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    REQUIRE(other.get_children(other.core()).empty());
}

TEST_CASE("CO4 changes under Core record no history", "[CO4]") {
    SimRole role;
    engine_core::Game game;
    game.history().reset_waypoints();
    const InstanceId tools = add_folder(game, "Tools", game.core());
    game.history().end_gesture();
    game.set_name(tools, "Renamed");
    game.history().end_gesture();
    const InstanceId other = add_folder(game, "Other", game.core());
    game.set_parent(tools, other);
    game.history().end_gesture();
    game.destroy(tools);
    game.history().end_gesture();
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE_FALSE(game.history().is_recording_in_progress());
}

TEST_CASE("CO4b making an instance and putting it in Core leaves no undo step, and the next edit stands alone",
          "[CO4b]") {
    SimRole role;
    engine_core::Game game;
    game.history().reset_waypoints();
    // As a plugin does: made with no parent, named, then put in Core, with no gesture between.
    const InstanceId tool = add_folder(game, "Tool", engine_core::DataModel::kNoParent);
    game.set_name(tool, "Dragger");
    game.set_parent(tool, game.core());
    REQUIRE_FALSE(game.history().is_recording_in_progress());
    // The user's next edit is its own step, and undoing it leaves the tool alone.
    const InstanceId part = add_folder(game, "Part", game.scene_service("Workspace"));
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().first);
    game.history().undo();
    REQUIRE_FALSE(game.alive(part));
    REQUIRE(game.alive(tool));
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("CO4c undoing a step recorded before an instance went into Core does not touch it", "[CO4c]") {
    SimRole role;
    engine_core::Game game;
    game.history().reset_waypoints();
    const InstanceId part = add_folder(game, "Part", game.scene_service("Workspace"));
    game.history().end_gesture();
    game.set_parent(part, engine_core::DataModel::kNoParent);
    game.history().end_gesture();
    game.set_parent(part, game.core());
    game.history().end_gesture();
    while (game.history().can_undo().first) {
        game.history().undo();
    }
    REQUIRE(game.alive(part));
    REQUIRE(game.parent(part) == game.core());
}

TEST_CASE("CO6 a game script cannot reach Core, while a plugin and the command line can", "[CO6]") {
    ScriptRig rig;
    add_script(rig.game, "Probe", R"(
        print("service", pcall(function() return game:GetService("Core") end))
        print("index", pcall(function() return game.Core end))
        print("find", game:FindFirstChild("Core"))
        local listed = false
        for _, child in game:GetChildren() do
            if child.Name == "Core" then listed = true end
        end
        print("listed", listed)
        print("wait", game:WaitForChild("Core", 0.05))
    )");
    rig.game.start_simulation();
    rig.frames(10, 0.02);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "find\tnil\n"));
    REQUIRE(has_line(output, "listed\tfalse\n"));
    REQUIRE(has_line(output, "wait\tnil\n"));
    bool service_refused = false;
    bool index_refused = false;
    for (const auto& line : output.lines) {
        service_refused = service_refused || line.text.rfind("service\tfalse", 0) == 0;
        index_refused = index_refused || line.text.rfind("index\tfalse", 0) == 0;
    }
    REQUIRE(service_refused);
    REQUIRE(index_refused);
    rig.game.stop_simulation();

    rig.runtime.run_chunk(R"(print("console", game:GetService("Core").Name, game:FindFirstChild("Core") ~= nil))");
    rig.frames(1);
    REQUIRE(has_line(rig.runtime.drain_output(), "console\tCore\ttrue\n"));
}

TEST_CASE("CO7 a Script in Core runs in the plugin VM, in edit mode and through Play, Stop, and New", "[CO7]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, rig.game.core(), "Tool", R"(
        print("tool started")
        game:GetService("RunService").Heartbeat:Connect(function()
            _G.ticks = (_G.ticks or 0) + 1
        end)
    )");
    rig.frames(1);
    REQUIRE(has_line(rig.runtime.drain_output(), "tool started\n"));
    REQUIRE(rig.runtime.is_plugin(script.id()));

    rig.game.start_simulation();
    rig.frames(2);
    rig.game.stop_simulation();
    engine_core::Project::reset_place(rig.game);
    rig.frames(2);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    REQUIRE_FALSE(has_line(rig.runtime.drain_output(), "tool started\n"));
}

TEST_CASE("CO7b disabling or destroying a Script in Core stops it, and enabling it starts it again", "[CO7b]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, rig.game.core(), "Tool", R"(print("tool started"))");
    rig.frames(1);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    script.set_enabled(false);
    rig.frames(1);
    REQUIRE_FALSE(rig.runtime.is_plugin(script.id()));
    rig.runtime.drain_output();
    script.set_enabled(true);
    rig.frames(1);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    REQUIRE(has_line(rig.runtime.drain_output(), "tool started\n"));
    const InstanceId id = script.id();
    rig.game.destroy(id);
    rig.frames(1);
    REQUIRE_FALSE(rig.runtime.is_plugin(id));
}

TEST_CASE("CO7c a plugin that puts a Script into Core starts it on the next step", "[CO7c]") {
    ScriptRig rig;
    add_script(rig.game, rig.game.core(), "Maker", R"lua(
        local made = Instance.new("Script")
        made.Name = "Made"
        made.Source = "print('made ran')"
        made.Parent = script.Parent
        print("maker done")
    )lua");
    rig.frames(1);
    const auto first = rig.runtime.drain_output();
    REQUIRE(has_line(first, "maker done\n"));
    rig.frames(1);
    const auto second = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE((has_line(first, "made ran\n") || has_line(second, "made ran\n")));
}

TEST_CASE("CO7d a Script orphaned when its Folder in Core is destroyed stops", "[CO7d]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Kit", rig.game.core());
    engine_core::Script& script = add_script(rig.game, folder, "Tool", R"(print("tool started"))");
    rig.frames(1);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    // As Destroy from Lua does: the Folder goes, and its children are left with no parent.
    rig.game.destroy(folder);
    rig.frames(1);
    REQUIRE(rig.game.alive(script.id()));
    REQUIRE_FALSE(rig.runtime.is_plugin(script.id()));
}

TEST_CASE("CO8 a GameObject in Core has a snapshot row, and a PhysicsObject in Core never simulates", "[CO8]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& shown = game.create_game_object();
    game.set_name(shown.id(), "Shown");
    game.set_parent(shown.id(), game.core());
    std::vector<InstanceId> rendered;
    game.for_each_rendered([&](const engine_core::GameObject& object) { rendered.push_back(object.id()); });
    REQUIRE(std::find(rendered.begin(), rendered.end(), shown.id()) != rendered.end());

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(shown.id()) != nullptr);

    engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
    game.set_parent(body.id(), game.core());
    std::vector<InstanceId> bodies;
    game.physics_bodies(bodies);
    REQUIRE(bodies.empty());
}

TEST_CASE("CO10 the built-in plugins load into Core, and New and Open keep them", "[CO10][project]") {
    ScriptRig rig;
    TempDir dir;
    engine_core::Project project = engine_core::Project::create(dir.path, rig.game);
    rig.game.history().reset_waypoints();
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {ide::PluginFile{"Hello", "print('hello')"}}) == 1);
    const InstanceId plugin = loader.loaded()[0];
    REQUIRE(rig.game.parent(plugin) == rig.game.core());
    REQUIRE(rig.runtime.is_plugin(plugin));
    REQUIRE_FALSE(rig.game.history().can_undo().first);

    engine_core::Project::reset_place(rig.game);
    engine_core::Project reopened = engine_core::Project::load(dir.path, rig.game);
    rig.frames(1);
    REQUIRE(rig.game.alive(plugin));
    REQUIRE(rig.runtime.is_plugin(plugin));
}
