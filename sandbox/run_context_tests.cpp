// Script.RunContext: where a script runs once worlds have roles
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Enum.hpp"
#include "Project.hpp"
#include "Script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using engine_core::RunContext;

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

TEST_CASE("SC8 RunContext is Server by default and scripts set it to Client or Any", "[server-client][run-context]") {
    ScriptRig rig;
    engine_core::Script& target = add_script(rig.game, "Target", "-- target");
    REQUIRE(target.run_context() == RunContext::Server);
    add_script(rig.game, "Setter",
               "local t = workspace:FindFirstChild('Target')\n"
               "_G.default_ok = t.RunContext == Enum.RunContext.Server\n"
               "t.RunContext = Enum.RunContext.Any\n"
               "_G.after = t.RunContext == Enum.RunContext.Any\n"
               "_G.bad = not pcall(function() t.RunContext = 3 end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"default_ok", "after", "bad"});
    REQUIRE(target.run_context() == RunContext::Any);
}

TEST_CASE("SC9 a project saves RunContext only when it is not Server and loads it back", "[server-client][run-context][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        engine_core::Script& client = add_script(game, "ClientOne", "-- c");
        REQUIRE_FALSE(client.set_run_context(static_cast<int>(RunContext::Client)));
        add_script(game, "ServerOne", "-- s");
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    engine_core::DataModel& game = loaded.datamodel();
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    auto* client = dynamic_cast<engine_core::Script*>(game.instance(game.find_first_child(workspace, "ClientOne")));
    auto* server = dynamic_cast<engine_core::Script*>(game.instance(game.find_first_child(workspace, "ServerOne")));
    REQUIRE(client != nullptr);
    REQUIRE(server != nullptr);
    REQUIRE(client->run_context() == RunContext::Client);
    REQUIRE(server->run_context() == RunContext::Server);
}

TEST_CASE("SC10 Stop puts back the RunContext a script had when Play started", "[server-client][run-context]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "S", "-- s");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    rig.game.start_simulation();
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
    rig.game.stop_simulation();
    auto* restored = dynamic_cast<engine_core::Script*>(
        rig.game.instance(rig.game.find_first_child(rig.game.scene_service("Workspace"), "S")));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->run_context() == RunContext::Client);
}

TEST_CASE("SC11 undoing a script's delete keeps its RunContext", "[server-client][run-context][history]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Script& script = add_script(game, "S", "-- s");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
    const std::string guid = game.guid(script.id());
    begin_step(game, "Delete");
    game.destroy_tree(script.id());
    end_step(game);
    game.history().undo();
    const std::optional<engine_core::InstanceId> found = game.find_guid(guid);
    REQUIRE(found.has_value());
    auto* back = dynamic_cast<engine_core::Script*>(game.instance(*found));
    REQUIRE(back != nullptr);
    REQUIRE(back->run_context() == RunContext::Any);
}
