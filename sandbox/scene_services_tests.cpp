// Scene services: Workspace, Lighting, Storage, and Scripts, the only children
// of game. They cannot be moved, renamed, or destroyed.

#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "SceneService.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace {

using engine_core::ContractViolation;
using engine_core::DataModel;
using engine_core::Folder;
using engine_core::Game;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

}  // namespace

TEST_CASE("SS1 a new Game holds the four scene services, in order", "[SS1]") {
    Game game;
    const std::vector<InstanceId> children = game.get_children(0);
    REQUIRE(children.size() == 5);
    const char* const classes[] = {"Workspace", "Lighting", "Storage", "Scripts"};
    const char* const guids[] = {"workspace", "lighting", "storage", "scripts"};
    for (std::size_t index = 0; index < 4; ++index) {
        INFO(classes[index]);
        const DataModel* service = game.instance(children[index]);
        REQUIRE(service != nullptr);
        REQUIRE(std::string(service->class_name()) == classes[index]);
        REQUIRE(service->is_scene_service());
        REQUIRE(game.name(children[index]) == classes[index]);
        REQUIRE(game.guid(children[index]) == guids[index]);
        REQUIRE(game.scene_service(classes[index]) == children[index]);
    }
    REQUIRE(std::string(game.instance(children[4])->class_name()) == "Assets");
    REQUIRE(game.scene_service("Folder") == 0);
    REQUIRE_FALSE(game.is_scene_service());
    // Nothing to undo: a new place starts with them.
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("SS2 a scene service cannot be moved, renamed, or destroyed", "[SS2]") {
    Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId storage = game.scene_service("Storage");

    REQUIRE(reason(game.parent_error(workspace, DataModel::kNoParent)) == "Workspace cannot be moved");
    REQUIRE(reason(game.parent_error(workspace, storage)) == "Workspace cannot be moved");
    // Its parent already: nothing moves.
    REQUIRE_FALSE(game.parent_error(workspace, 0));

    REQUIRE(reason(game.rename_error(workspace, "World")) == "Workspace cannot be renamed");
    REQUIRE_FALSE(game.rename_error(workspace, "Workspace"));
    // New Project names the root after its folder.
    REQUIRE_FALSE(game.rename_error(0, "MyPlace"));

    REQUIRE(reason(game.destroy_error(workspace)) == "Workspace cannot be destroyed");
    REQUIRE(reason(game.destroy_error(0)) == "game cannot be destroyed");

    // The mutators refuse what the queries refuse.
    REQUIRE_THROWS_AS(game.set_parent(workspace, DataModel::kNoParent), ContractViolation);
    REQUIRE_THROWS_AS(game.set_parent(workspace, storage), ContractViolation);
    REQUIRE_THROWS_AS(game.set_name(workspace, "World"), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy(workspace), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy_tree(workspace), ContractViolation);

    REQUIRE(game.alive(workspace));
    REQUIRE(game.parent(workspace) == 0);
    REQUIRE(game.name(workspace) == "Workspace");
    REQUIRE(game.get_children(0).size() == 5);
}

TEST_CASE("SS3 only scene services are children of game", "[SS3]") {
    Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    Folder& outer = game.create<Folder>();
    Folder& inner = game.create<Folder>();
    game.set_name(inner.id(), "Inner");

    REQUIRE(reason(game.parent_error(outer.id(), 0)) ==
            "Only scene services can be children of game; put Folder in Workspace");
    REQUIRE_THROWS_AS(game.set_parent(outer.id(), 0), ContractViolation);
    REQUIRE(game.parent(outer.id()) == DataModel::kNoParent);

    REQUIRE_FALSE(game.parent_error(outer.id(), workspace));
    game.set_parent(outer.id(), workspace);
    game.set_parent(inner.id(), outer.id());
    REQUIRE(reason(game.parent_error(outer.id(), inner.id())) == "Cannot parent Folder to itself or a descendant");
    REQUIRE(reason(game.parent_error(outer.id(), outer.id())) == "Cannot parent Folder to itself or a descendant");
    // Taking it out of the tree is fine.
    REQUIRE_FALSE(game.parent_error(inner.id(), DataModel::kNoParent));

    REQUIRE(reason(game.parent_error(0, workspace)) == "game cannot be moved");

    game.destroy(inner.id());
    REQUIRE(reason(game.parent_error(inner.id(), workspace)) == "That instance no longer exists");
    REQUIRE(reason(game.parent_error(outer.id(), inner.id())) == "That instance no longer exists");
    REQUIRE(reason(game.rename_error(inner.id(), "X")) == "That instance no longer exists");
}

TEST_CASE("SS4 undo and Stop keep the scene services", "[SS4]") {
    SimRole role;
    Game game;
    const std::vector<InstanceId> before = game.get_children(0);
    const InstanceId workspace = game.scene_service("Workspace");

    game.history().set_pending_gesture("Insert Folder");
    Folder& folder = game.create<Folder>();
    game.set_parent(folder.id(), workspace);
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().first);
    while (game.history().can_undo().first) {
        game.history().undo();
    }
    REQUIRE(game.get_children(0) == before);
    REQUIRE(game.get_children(workspace).empty());

    game.start_simulation();
    Folder& played = game.create<Folder>();
    game.set_parent(played.id(), workspace);
    game.stop_simulation();
    REQUIRE(game.get_children(0) == before);
    REQUIRE(game.get_children(workspace).empty());
}

namespace {

engine_core::Lighting& lighting_of(Game& game) {
    return *dynamic_cast<engine_core::Lighting*>(game.instance(game.scene_service("Lighting")));
}

void require_globals(ScriptRig& rig, std::initializer_list<const char*> names) {
    INFO(rig.runtime.last_error());
    for (const char* name : names) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

}  // namespace

TEST_CASE("SS5 Lighting's properties save, undo, and come back at Stop", "[SS5]") {
    SimRole role;
    Game game;
    engine_core::Lighting& lighting = lighting_of(game);
    REQUIRE(lighting.brightness() == engine_core::Lighting::kDefaultBrightness);
    REQUIRE(lighting.clock_time() == 14.0);

    // A default place saves nothing.
    engine_core::PropertyBag saved;
    lighting.save_properties(saved);
    REQUIRE(saved.empty());

    game.history().set_pending_gesture("Set Brightness");
    REQUIRE_FALSE(lighting.set_brightness(3.5));
    game.history().end_gesture();
    REQUIRE(lighting.brightness() == 3.5);
    REQUIRE(game.history().can_undo().second == "Set Brightness");
    game.history().undo();
    REQUIRE(lighting.brightness() == engine_core::Lighting::kDefaultBrightness);
    game.history().redo();
    REQUIRE(lighting.brightness() == 3.5);

    // ClockTime wraps into a day; the others do not go below 0.
    REQUIRE_FALSE(lighting.set_clock_time(25.0));
    REQUIRE(lighting.clock_time() == 1.0);
    REQUIRE_FALSE(lighting.set_clock_time(-2.0));
    REQUIRE(lighting.clock_time() == 22.0);
    REQUIRE_FALSE(lighting.set_fog_end(-5.0));
    REQUIRE(lighting.fog_end() == 0.0);
    REQUIRE(*lighting.set_brightness(std::nan("")) == "Brightness must be a finite number");
    REQUIRE(lighting.brightness() == 3.5);

    engine_core::ColorRgb red;
    red.g = 0.f;
    red.b = 0.f;
    REQUIRE_FALSE(lighting.set_ambient(red));

    // What save writes, a load reads back.
    saved.clear();
    lighting.save_properties(saved);
    Game other;
    engine_core::Lighting& loaded = lighting_of(other);
    for (const auto& member : saved) {
        std::string error;
        INFO(member.first);
        REQUIRE(loaded.load_property(member.first, member.second, error));
        REQUIRE(error.empty());
    }
    REQUIRE(loaded.brightness() == 3.5);
    REQUIRE(loaded.clock_time() == 22.0);
    REQUIRE(loaded.fog_end() == 0.0);
    REQUIRE(engine_core::same_color(loaded.ambient(), red));
    std::string error;
    REQUIRE(loaded.load_property("Brightness", engine_core::JsonValue::string("bright"), error));
    REQUIRE(error == "Brightness must be a number");

    // Play changes are dropped at Stop, as any other instance's are.
    game.start_simulation();
    REQUIRE_FALSE(lighting.set_brightness(9.0));
    game.stop_simulation();
    REQUIRE(lighting.brightness() == 3.5);
}

TEST_CASE("SS6 scripts see the scene services and cannot move them", "[SS6]") {
    ScriptRig rig;
    add_script(rig.game, "Services", R"(
        _G.workspace = workspace == game.Workspace and workspace == game:GetService("Workspace")
        _G.classes = workspace.ClassName == "Workspace" and game.Lighting.ClassName == "Lighting"
            and game:GetService("Storage").ClassName == "Storage" and game:GetService("Scripts").ClassName == "Scripts"
        _G.isa = workspace:IsA("SceneService") and workspace:IsA("DataModel") and not workspace:IsA("Instance")
        _G.run_service = game:GetService("RunService") ~= nil
        _G.locked = getmetatable(workspace) == "The metatable is locked"
            and getmetatable(script) == "The metatable is locked"

        local function refuses(fn, expected)
            local ok, message = pcall(fn)
            return not ok and string.find(message, expected, 1, true) ~= nil
        end
        _G.no_move = refuses(function() workspace.Parent = nil end, "Workspace cannot be moved")
        _G.no_move_in = refuses(function() game.Lighting.Parent = workspace end, "Lighting cannot be moved")
        _G.no_rename = refuses(function() game.Storage.Name = "Assets" end, "Storage cannot be renamed")
        _G.same_name = pcall(function() game.Storage.Name = "Storage" end)
        _G.no_destroy = refuses(function() game.Scripts:Destroy() end, "Scripts cannot be destroyed")
        _G.no_game_destroy = refuses(function() game:Destroy() end, "game cannot be destroyed")
        _G.no_game_move = refuses(function() game.Parent = workspace end, "game cannot be moved")
        _G.no_new = refuses(function() Instance.new("Workspace") end, "unknown class Workspace")
        _G.no_setmetatable = not pcall(setmetatable, workspace, {})

        local folder = Instance.new("Folder")
        _G.no_root = refuses(function() folder.Parent = game end,
            "Only scene services can be children of game; put Folder in Workspace")
        _G.no_root_new = refuses(function() Instance.new("Folder", game) end,
            "Only scene services can be children of game; put Folder in Workspace")
        _G.no_cycle = refuses(function() script.Parent = script end, "Cannot parent Services to itself or a descendant")
        folder.Parent = workspace
        _G.in_workspace = folder.Parent == workspace

        game.Lighting.Brightness = 4
        _G.lighting = game.Lighting.Brightness == 4 and game.Lighting.ClockTime == 14
        _G.no_nan = refuses(function() game.Lighting.FogEnd = 0 / 0 end, "FogEnd must be a finite number")
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"workspace", "classes", "isa", "run_service", "locked", "no_move", "no_move_in", "no_rename",
                          "same_name", "no_destroy", "no_game_destroy", "no_game_move", "no_new", "no_setmetatable",
                          "no_root", "no_root_new", "no_cycle", "in_workspace", "lighting", "no_nan"});
    REQUIRE(rig.game.get_children(0).size() == 5);
}

// Luau would otherwise read a path of up to three names from a global once,
// when the script loads.
TEST_CASE("SS9 a path through game or workspace reads the live value", "[SS9]") {
    ScriptRig rig;
    add_script(rig.game, rig.game.scene_service("Scripts"), "Paths", R"(
        game.Lighting.Brightness = 3
        _G.game_path = game.Lighting.Brightness == 3
        local folder = Instance.new("Folder", workspace)
        folder.Name = "Box"
        _G.found = workspace.Box ~= nil
        workspace.Box.Name = "Crate"
        _G.workspace_path = workspace.Crate.Name == "Crate"
        _G.script_path = script.Parent.Name == "Scripts"
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"game_path", "found", "workspace_path", "script_path"});
}

TEST_CASE("SS7 a script runs only under Workspace or Scripts", "[SS7]") {
    ScriptRig rig;
    const InstanceId workspace = rig.game.scene_service("Workspace");
    const InstanceId storage = rig.game.scene_service("Storage");
    const InstanceId lighting = rig.game.scene_service("Lighting");
    add_script(rig.game, workspace, "InWorkspace", "_G.workspace_ran = true");
    add_script(rig.game, rig.game.scene_service("Scripts"), "InScripts", "_G.scripts_ran = true");
    add_script(rig.game, storage, "InStorage", "_G.storage_ran = true");
    add_script(rig.game, lighting, "InLighting", "_G.lighting_ran = true");
    Folder& nested = rig.game.create<Folder>();
    rig.game.set_parent(nested.id(), workspace);
    add_script(rig.game, nested.id(), "Nested", "_G.nested_ran = true");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"workspace_ran", "scripts_ran", "nested_ran"});
    bool ran = false;
    REQUIRE_FALSE(rig.runtime.global_boolean("storage_ran", ran));
    REQUIRE_FALSE(rig.runtime.global_boolean("lighting_ran", ran));
}

TEST_CASE("SS8 a script that leaves Workspace and Scripts stops, and starts again when it comes back", "[SS8]") {
    ScriptRig rig;
    const InstanceId workspace = rig.game.scene_service("Workspace");
    const InstanceId storage = rig.game.scene_service("Storage");
    Folder& holder = rig.game.create<Folder>();
    rig.game.set_parent(holder.id(), workspace);
    add_script(rig.game, holder.id(), "Counter", R"(
        _G.starts = (_G.starts or 0) + 1
        while true do
            _G.ticks = (_G.ticks or 0) + 1
            task.wait()
        end
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    double starts = 0;
    double ticks = 0;
    REQUIRE(rig.runtime.global_number("starts", starts));
    REQUIRE(starts == 1);

    // Moving its folder to Storage stops it.
    rig.game.set_parent(holder.id(), storage);
    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    const double stopped_at = ticks;
    rig.frames(3, 0.05);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks == stopped_at);

    // Back into Scripts, it runs again from the top.
    rig.game.set_parent(holder.id(), rig.game.scene_service("Scripts"));
    rig.frames(2, 0.05);
    REQUIRE(rig.runtime.global_number("starts", starts));
    REQUIRE(starts == 2);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks > stopped_at);

    // Between Scripts and Workspace it keeps running, without a new start.
    rig.game.set_parent(holder.id(), workspace);
    rig.frames(2, 0.05);
    REQUIRE(rig.runtime.global_number("starts", starts));
    REQUIRE(starts == 2);

    // Out of the tree stops it too.
    rig.game.set_parent(holder.id(), DataModel::kNoParent);
    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    const double detached_at = ticks;
    rig.frames(3, 0.05);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks == detached_at);
}

namespace {

void write_text(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string instance_file(const char* klass, const char* guid, const char* name) {
    return std::string("{\n  \"class\": \"") + klass + "\",\n  \"id\": \"" + guid + "\",\n  \"Name\": \"" + name +
           "\"\n}\n";
}

// A place whose files hold Workspace but no other scene service: a part, a
// script, and a folder with a child, all in Workspace.
void write_partial_place(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    write_text(root / "project.json",
               "{\"format\": 1, \"name\": \"Partial\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, "
               "\"resources\": {\"root\": \"resources\"}}\n");
    write_text(root / "src" / "init.json", instance_file("Game", "root0", "Partial"));
    const fs::path workspace = root / "src" / "Workspace.workspace";
    write_text(workspace / "init.json", instance_file("Workspace", "workspace", "Workspace"));
    write_text(workspace / "Part.aaaa.json", instance_file("GameObject", "aaaa", "Part"));
    write_text(workspace / "Main.bbbb.meta.json", instance_file("Script", "bbbb", "Main"));
    write_text(workspace / "Main.bbbb.luau", "print('main')\n");
    write_text(workspace / "Box.cccc" / "init.json", instance_file("Folder", "cccc", "Box"));
    write_text(workspace / "Box.cccc" / "Inner.dddd.json", instance_file("Folder", "dddd", "Inner"));
}

std::vector<std::string> child_names(const DataModel& game, InstanceId parent) {
    std::vector<std::string> out;
    for (InstanceId child : game.get_children(parent)) {
        out.push_back(game.name(child));
    }
    return out;
}

}  // namespace

TEST_CASE("SS10 a place missing scene services loads with them made", "[SS10][project]") {
    SimRole role;
    TempDir dir;
    write_partial_place(dir.path);
    namespace fs = std::filesystem;
    {
        engine_core::Project project = engine_core::Project::load(dir.path);
        DataModel& game = project.datamodel();
        REQUIRE(child_names(game, 0) ==
                std::vector<std::string>{"Workspace", "Lighting", "Storage", "Scripts", "Assets"});
        const InstanceId workspace = game.scene_service("Workspace");
        REQUIRE(child_names(game, workspace) == std::vector<std::string>{"Part", "Main", "Box"});
        REQUIRE(child_names(game, *game.find_guid("cccc")) == std::vector<std::string>{"Inner"});
        // Reading the disk again gives the same tree: nothing differs but the made services.
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE_FALSE(scan.has_disk_changes);
        REQUIRE(project.unsaved());
        // Nothing is written until a save.
        REQUIRE_FALSE(fs::exists(dir.path / "src" / "Lighting.lighting.json"));

        project.save();
        REQUIRE(fs::exists(dir.path / "src" / "Lighting.lighting.json"));
        REQUIRE(fs::exists(dir.path / "src" / "Storage.storage.json"));
        REQUIRE(fs::exists(dir.path / "src" / "Scripts.scripts.json"));
        REQUIRE(fs::exists(dir.path / "src" / "Assets.assets" / "init.json"));
        REQUIRE(fs::exists(dir.path / "src" / "Workspace.workspace" / "Box.cccc" / "Inner.dddd.json"));
        REQUIRE_FALSE(project.unsaved());
    }
    engine_core::Project again = engine_core::Project::load(dir.path);
    DataModel& game = again.datamodel();
    REQUIRE(child_names(game, game.scene_service("Workspace")) == std::vector<std::string>{"Part", "Main", "Box"});
    again.save();
    REQUIRE(again.last_save().written.empty());
    REQUIRE(again.last_save().moved.empty());
    REQUIRE(again.last_save().removed.empty());
}

// A service the load made has no file until a save writes one, so loading
// other changes from disk must not give it one.
TEST_CASE("SS14 changes from disk in a place missing scene services still save", "[SS14][project]") {
    SimRole role;
    TempDir dir;
    write_partial_place(dir.path);
    engine_core::Project project = engine_core::Project::load(dir.path);
    write_text(dir.path / "src" / "Workspace.workspace" / "Main.bbbb.luau", "print('edited outside')\n");
    const engine_core::DiskScan applied = project.apply_disk();
    REQUIRE(applied.conflicts.empty());
    REQUIRE(applied.loaded == std::vector<std::string>{"Main"});
    REQUIRE_NOTHROW(project.save());
    REQUIRE(std::filesystem::exists(dir.path / "src" / "Lighting.lighting.json"));
    REQUIRE(std::filesystem::exists(dir.path / "src" / "Workspace.workspace" / "Main.bbbb.luau"));
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("SS11 a file may not take a scene service's GUID, put one elsewhere, or sit under game",
          "[SS11][project]") {
    SimRole role;
    const std::filesystem::path box = std::filesystem::path("src") / "Workspace.workspace" / "Box.cccc";
    {
        TempDir dir;
        write_partial_place(dir.path);
        write_text(dir.path / box / "Fake.storage.json", instance_file("Folder", "storage", "Fake"));
        try {
            engine_core::Project::load(dir.path);
            FAIL("the load should refuse the GUID");
        } catch (const engine_core::ProjectError& error) {
            REQUIRE(std::string(error.what()).find("GUID storage is reserved for Storage") != std::string::npos);
        }
    }
    {
        TempDir dir;
        write_partial_place(dir.path);
        write_text(dir.path / box / "Lighting.lighting.json", instance_file("Lighting", "lighting", "L"));
        try {
            engine_core::Project::load(dir.path);
            FAIL("the load should refuse a service below game");
        } catch (const engine_core::ProjectError& error) {
            REQUIRE(std::string(error.what()).find("Lighting must be a child of game with GUID lighting") !=
                    std::string::npos);
        }
    }
    {
        TempDir dir;
        write_partial_place(dir.path);
        write_text(dir.path / "src" / "Loose.eeee.json", instance_file("Folder", "eeee", "Loose"));
        try {
            engine_core::Project::load(dir.path);
            FAIL("the load should refuse an instance directly under game");
        } catch (const engine_core::ProjectError& error) {
            REQUIRE(std::string(error.what()).find("only a scene service can be a child of game") !=
                    std::string::npos);
        }
    }
}

TEST_CASE("SS12 a loaded place keeps its Lighting, and a new place resets it", "[SS12][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        lighting_of(static_cast<Game&>(project.datamodel())).set_brightness(5.0);
        project.save();
    }
    Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const InstanceId lighting = game.scene_service("Lighting");
    REQUIRE(lighting_of(game).brightness() == 5.0);
    engine_core::Project::reset_place(game);
    // The same service, back at its defaults.
    REQUIRE(game.scene_service("Lighting") == lighting);
    REQUIRE(lighting_of(game).brightness() == engine_core::Lighting::kDefaultBrightness);
    REQUIRE(game.get_children(0).size() == 5);
}

// A saved registry property gets Changed with its own name, and its registered
// default is what a new instance has, so a new place saves none of them.
TEST_CASE("SS13 registry properties fire Changed by name and default to a new instance", "[SS13]") {
    ScriptRig rig;
    add_script(rig.game, "Watch", R"(
        local seen = {}
        game.Lighting.Changed:Connect(function(property)
            table.insert(seen, property)
            _G.names = table.concat(seen, ",") == "Brightness,FogColor"
        end)
        game.Lighting.Brightness = 3
        game.Lighting.FogColor = Color3.new(1, 0, 0)
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    require_globals(rig, {"names"});

    std::vector<std::string> classes;
    engine_core::lua_class_names(classes);
    Game fresh;
    int checked = 0;
    for (const std::string& name : classes) {
        // A base class only for IsA, as Light, inherits saved fields but no file can hold one.
        if (engine_core::lua_saved_fields(name.c_str()).empty() || !engine_core::project_class_known(name)) {
            continue;
        }
        DataModel* object = fresh.instance(fresh.scene_service(name));
        if (object == nullptr) {
            object = engine_core::lua_create_instance(fresh, name.c_str());
        }
        INFO(name);
        REQUIRE(object != nullptr);
        engine_core::PropertyBag saved;
        object->save_properties(saved);
        REQUIRE(saved.empty());
        ++checked;
    }
    REQUIRE(checked >= 1);
}

// Destroy leaves the children alive and out of the tree, so a script among
// them stops, as moving it out would.
TEST_CASE("SS15 destroying a script's parent stops the script", "[SS15]") {
    ScriptRig rig;
    Folder& holder = rig.game.create<Folder>();
    rig.game.set_name(holder.id(), "Holder");
    rig.game.set_parent(holder.id(), rig.game.scene_service("Workspace"));
    add_script(rig.game, holder.id(), "Ticker", R"(
        while true do
            _G.ticks = (_G.ticks or 0) + 1
            task.wait()
        end
    )");
    add_script(rig.game, rig.game.scene_service("Scripts"), "Breaker", R"(
        task.wait(0.1)
        workspace.Holder:Destroy()
    )");
    rig.game.start_simulation();
    rig.frames(6, 0.05);
    double ticks = 0;
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    const double stopped_at = ticks;
    rig.frames(4, 0.05);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks == stopped_at);
}
