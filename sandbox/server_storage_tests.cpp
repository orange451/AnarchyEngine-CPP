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

TEST_CASE("SC20 Storage holds ServerStorage then ClientStorage, and GetService finds both", "[server-client][server-storage]") {
    ScriptRig rig;
    const engine_core::InstanceId storage = rig.game.scene_service("Storage");
    const engine_core::InstanceId server = rig.game.service("ServerStorage");
    const engine_core::InstanceId client = rig.game.service("ClientStorage");
    REQUIRE(server != 0);
    REQUIRE(client != 0);
    REQUIRE(rig.game.guid(server) == "serverstorage");
    REQUIRE(rig.game.guid(client) == "clientstorage");
    REQUIRE(rig.game.parent(server) == storage);
    REQUIRE(rig.game.parent(client) == storage);
    REQUIRE(rig.game.first_child(storage) == server);
    REQUIRE(rig.game.next_sibling(server) == client);
    REQUIRE_FALSE(rig.game.instance(server)->is_scene_service());
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "_G.same = game:GetService('ServerStorage') == game.Storage.ServerStorage\n"
               "_G.same_client = game:GetService('ClientStorage') == game.Storage.ClientStorage");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"same", "same_client"});
}

TEST_CASE("SC20b a script in ClientStorage never runs, in any role", "[server-client][server-storage]") {
    for (const engine_core::WorldRole role :
         {engine_core::WorldRole::Standalone, engine_core::WorldRole::Server, engine_core::WorldRole::Client}) {
        ScriptRig rig;
        rig.game.set_world_role(role);
        engine_core::Script& script =
            add_script(rig.game, rig.game.service("ClientStorage"), "Kept", "_G.kept_ran = true");
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Any)));
        rig.game.start_simulation();
        rig.frames(1);
        bool value = false;
        REQUIRE_FALSE(rig.runtime.global_boolean("kept_ran", value));
    }
}

TEST_CASE("SC21 a script in ServerStorage never runs, in any role", "[server-client][server-storage]") {
    for (const engine_core::WorldRole role :
         {engine_core::WorldRole::Standalone, engine_core::WorldRole::Server, engine_core::WorldRole::Client}) {
        ScriptRig rig;
        rig.game.set_world_role(role);
        engine_core::Script& script =
            add_script(rig.game, rig.game.service("ServerStorage"), "Hidden", "_G.hidden_ran = true");
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Any)));
        rig.game.start_simulation();
        rig.frames(1);
        bool value = false;
        REQUIRE_FALSE(rig.runtime.global_boolean("hidden_ran", value));
    }
}

TEST_CASE("SC22 ServerStorage and ClientStorage cannot be moved, renamed, or destroyed", "[server-client][server-storage]") {
    ScriptRig rig;
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "local s = game.Storage.ServerStorage\n"
               "local c = game.Storage.ClientStorage\n"
               "_G.no_move = not pcall(function() s.Parent = workspace end)\n"
               "_G.no_rename = not pcall(function() s.Name = 'X' end)\n"
               "_G.no_destroy = not pcall(function() s:Destroy() end)\n"
               "_G.no_move_client = not pcall(function() c.Parent = workspace end)\n"
               "_G.no_destroy_client = not pcall(function() c:Destroy() end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"no_move", "no_rename", "no_destroy", "no_move_client", "no_destroy_client"});
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
        game.set_parent(folder.id(), game.service("ServerStorage"));
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    engine_core::DataModel& game = loaded.datamodel();
    REQUIRE(game.find_first_child(game.service("ServerStorage"), "Secrets") != 0);
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
    REQUIRE(loaded.datamodel().service("ServerStorage") != 0);
}

TEST_CASE("SC25 a project that saved ServerStorage as a child of game loads it inside Storage, with what it held", "[server-client][server-storage][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), "Secrets");
        game.set_parent(folder.id(), game.service("ServerStorage"));
        project.save();
    }
    // Move ServerStorage's file or folder up to src/, where builds before it
    // went inside Storage wrote it.
    fs::path from;
    for (const auto& entry : fs::recursive_directory_iterator(root / "src")) {
        if (entry.path().filename().string().rfind("ServerStorage", 0) == 0) {
            from = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(from.empty());
    REQUIRE(from.parent_path() != root / "src");
    fs::rename(from, root / "src" / from.filename());
    engine_core::Project loaded = engine_core::Project::load(root);
    engine_core::DataModel& game = loaded.datamodel();
    const engine_core::InstanceId server = game.service("ServerStorage");
    REQUIRE(server != 0);
    REQUIRE(game.parent(server) == game.scene_service("Storage"));
    REQUIRE(game.find_first_child(server, "Secrets") != 0);
    REQUIRE(game.get_children(0).size() == 8);
}
