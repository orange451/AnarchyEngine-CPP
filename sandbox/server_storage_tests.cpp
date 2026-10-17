// ServerStorage: what never leaves the server
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Folder.hpp"
#include "Project.hpp"
#include "Script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

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

TEST_CASE("SC20 game holds ServerStorage right after Storage, and GetService finds it", "[server-client][server-storage]") {
    ScriptRig rig;
    const engine_core::InstanceId storage = rig.game.scene_service("ServerStorage");
    REQUIRE(storage != 0);
    REQUIRE(rig.game.guid(storage) == "serverstorage");
    REQUIRE(rig.game.next_sibling(rig.game.scene_service("Storage")) == storage);
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "_G.same = game:GetService('ServerStorage') == game.ServerStorage");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"same"});
}

TEST_CASE("SC21 a script in ServerStorage never runs, in any role", "[server-client][server-storage]") {
    for (const engine_core::WorldRole role :
         {engine_core::WorldRole::Standalone, engine_core::WorldRole::Server, engine_core::WorldRole::Client}) {
        ScriptRig rig;
        rig.game.set_world_role(role);
        engine_core::Script& script =
            add_script(rig.game, rig.game.scene_service("ServerStorage"), "Hidden", "_G.hidden_ran = true");
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Any)));
        rig.game.start_simulation();
        rig.frames(1);
        bool value = false;
        REQUIRE_FALSE(rig.runtime.global_boolean("hidden_ran", value));
    }
}

TEST_CASE("SC22 ServerStorage cannot be moved, renamed, or destroyed", "[server-client][server-storage]") {
    ScriptRig rig;
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "local s = game.ServerStorage\n"
               "_G.no_move = not pcall(function() s.Parent = workspace end)\n"
               "_G.no_rename = not pcall(function() s.Name = 'X' end)\n"
               "_G.no_destroy = not pcall(function() s:Destroy() end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"no_move", "no_rename", "no_destroy"});
}

TEST_CASE("SC23 ServerStorage and what it holds save and load", "[server-client][server-storage][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), "Secrets");
        game.set_parent(folder.id(), game.scene_service("ServerStorage"));
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    engine_core::DataModel& game = loaded.datamodel();
    REQUIRE(game.find_first_child(game.scene_service("ServerStorage"), "Secrets") != 0);
}

TEST_CASE("SC24 a project saved before ServerStorage existed loads and gains it", "[server-client][server-storage][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project made = engine_core::Project::create(root);
        made.save();
    }
    // Remove ServerStorage's file, as a project from before it would lack it.
    bool removed = false;
    for (const auto& entry : fs::recursive_directory_iterator(root / "src")) {
        if (entry.path().filename().string().find("ServerStorage") != std::string::npos) {
            fs::remove_all(entry.path());
            removed = true;
            break;
        }
    }
    REQUIRE(removed);
    engine_core::Project loaded = engine_core::Project::load(root);
    REQUIRE(loaded.datamodel().scene_service("ServerStorage") != 0);
}
