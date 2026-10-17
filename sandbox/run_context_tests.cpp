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

namespace {

void add_three(engine_core::DataModel& game) {
    engine_core::Script& server = add_script(game, "Server", "_G.server_ran = true");
    engine_core::Script& client = add_script(game, "Client", "_G.client_ran = true");
    engine_core::Script& any = add_script(game, "Any", "_G.any_ran = true");
    REQUIRE_FALSE(server.set_run_context(static_cast<int>(RunContext::Server)));
    REQUIRE_FALSE(client.set_run_context(static_cast<int>(RunContext::Client)));
    REQUIRE_FALSE(any.set_run_context(static_cast<int>(RunContext::Any)));
}

bool ran(ScriptRig& rig, const char* name) {
    bool value = false;
    return rig.runtime.global_boolean(name, value) && value;
}

}  // namespace

TEST_CASE("SC12 Standalone runs every script whatever its RunContext", "[server-client][run-context]") {
    ScriptRig rig;
    REQUIRE(rig.game.world_role() == engine_core::WorldRole::Standalone);
    add_three(rig.game);
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(ran(rig, "server_ran"));
    REQUIRE(ran(rig, "client_ran"));
    REQUIRE(ran(rig, "any_ran"));
}

TEST_CASE("SC13 a Server world runs Server and Any scripts only", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Server);
    add_three(rig.game);
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(ran(rig, "server_ran"));
    REQUIRE_FALSE(ran(rig, "client_ran"));
    REQUIRE(ran(rig, "any_ran"));
}

TEST_CASE("SC14 a Client world runs Client and Any scripts only", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Client);
    add_three(rig.game);
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE_FALSE(ran(rig, "server_ran"));
    REQUIRE(ran(rig, "client_ran"));
    REQUIRE(ran(rig, "any_ran"));
}

TEST_CASE("SC15 RunService.IsServer and IsClient follow the role; both are true in Standalone", "[server-client][run-context]") {
    const char* source =
        "local run = game:GetService('RunService')\n"
        "_G.is_server = run:IsServer()\n"
        "_G.is_client = run:IsClient()";
    struct Case { engine_core::WorldRole role; bool server; bool client; };
    for (const Case c : {Case{engine_core::WorldRole::Standalone, true, true},
                         Case{engine_core::WorldRole::Server, true, false},
                         Case{engine_core::WorldRole::Client, false, true}}) {
        ScriptRig rig;
        rig.game.set_world_role(c.role);
        engine_core::Script& script = add_script(rig.game, "Probe", source);
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
        rig.game.start_simulation();
        rig.frames(1);
        bool value = false;
        REQUIRE(rig.runtime.global_boolean("is_server", value));
        REQUIRE(value == c.server);
        REQUIRE(rig.runtime.global_boolean("is_client", value));
        REQUIRE(value == c.client);
    }
}

TEST_CASE("SC16 changing RunContext during play starts or stops the script at once", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Server);
    engine_core::Script& script = add_script(rig.game, "Ticker",
        "_G.ticks = 0\n"
        "game:GetService('RunService').Heartbeat:Connect(function() _G.ticks = _G.ticks + 1 end)");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    rig.game.start_simulation();
    rig.frames(2);
    double ticks = -1;
    REQUIRE_FALSE(rig.runtime.global_number("ticks", ticks));   // never started on a server
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Server)));
    rig.frames(3);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks >= 1);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    const double stopped_at = ticks;
    rig.frames(3);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks == stopped_at);   // its Heartbeat connection died with it
}

namespace {

const char* kCountingSource =
    "_G.starts = (_G.starts or 0) + 1\n"
    "_G.ticks = _G.ticks or 0\n"
    "game:GetService('RunService').Heartbeat:Connect(function() _G.ticks = _G.ticks + 1 end)";

double number(ScriptRig& rig, const char* name) {
    double value = -1;
    REQUIRE(rig.runtime.global_number(name, value));
    return value;
}

}  // namespace

TEST_CASE("SC16b Standalone: changing RunContext during play never restarts the script", "[server-client][run-context]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "Counter", kCountingSource);
    rig.game.start_simulation();
    rig.frames(2);
    REQUIRE(number(rig, "starts") == 1);
    for (const RunContext next : {RunContext::Client, RunContext::Any, RunContext::Server}) {
        const double before = number(rig, "ticks");
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(next)));
        rig.frames(2);
        REQUIRE(number(rig, "starts") == 1);
        REQUIRE(number(rig, "ticks") > before);
    }
}

TEST_CASE("SC16c Server: RunContext changes that keep the script running here do not restart it", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Server);
    engine_core::Script& script = add_script(rig.game, "Counter", kCountingSource);
    rig.game.start_simulation();
    rig.frames(2);
    REQUIRE(number(rig, "starts") == 1);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
    rig.frames(2);
    REQUIRE(number(rig, "starts") == 1);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    const double stopped_at = number(rig, "ticks");
    rig.frames(3);
    REQUIRE(number(rig, "ticks") == stopped_at);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Server)));
    rig.frames(3);
    REQUIRE(number(rig, "starts") == 2);
    REQUIRE(number(rig, "ticks") > stopped_at);
}
