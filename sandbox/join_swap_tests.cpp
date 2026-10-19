// The swap a join runs, and scripts at a role flip during play
// (docs/superpowers/specs/2026-10-19-join-from-lua-design.md, "Persistent" and
// "Scripts at the role flip").

#include "support.hpp"

#include "ClientWorld.hpp"
#include "Folder.hpp"
#include "Script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const char* name, InstanceId parent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder.id();
}

std::vector<InstanceId> every_service(engine_core::DataModel& game) {
    std::vector<InstanceId> out;
    std::vector<InstanceId> pending{0};
    while (!pending.empty()) {
        const InstanceId at = pending.back();
        pending.pop_back();
        for (const InstanceId child : game.get_children(at)) {
            if (game.instance(child)->is_service()) {
                out.push_back(child);
                pending.push_back(child);
            }
        }
    }
    return out;
}

double number(engine_core::ScriptRuntime& runtime, const char* name) {
    double value = -1;
    REQUIRE(runtime.global_number(name, value));
    return value;
}

// A Persistent Script under Workspace that counts each Heartbeat in _G[global].
engine_core::Script& counter(engine_core::DataModel& game, const char* name, const char* global,
                             engine_core::RunContext context) {
    const std::string source = std::string("_G.") + global + " = 0\n"
                               "game:GetService('RunService').Heartbeat:Connect(function()\n"
                               "    _G." + global + " += 1\n"
                               "end)\n";
    engine_core::Script& script = add_script(game, name, source.c_str());
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(context)));
    game.set_persistent(script.id(), true);
    return script;
}

}  // namespace

TEST_CASE("JN10 swap_for_client keeps Persistent trees and nothing else", "[join][persistent]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId gui = game.scene_service("Gui");
    const InstanceId menu = add_folder(game, "Menu", workspace);
    game.set_persistent(menu, true);
    const InstanceId logo = add_folder(game, "Logo", menu);
    const InstanceId map = add_folder(game, "Map", workspace);
    const InstanceId loading = add_folder(game, "Loading", gui);
    game.set_persistent(loading, true);
    const std::vector<InstanceId> services = every_service(game);
    REQUIRE(game.world_role() == engine_core::WorldRole::Standalone);
    game.capture_place();
    game.start_simulation();
    rig.frames(1);

    engine_network::swap_for_client(game);

    REQUIRE(game.alive(menu));
    REQUIRE(game.alive(logo));
    REQUIRE(game.alive(loading));
    REQUIRE(game.parent(menu) == workspace);
    REQUIRE(game.parent(logo) == menu);
    REQUIRE(game.parent(loading) == gui);
    REQUIRE_FALSE(game.alive(map));
    for (const InstanceId service : services) {
        REQUIRE(game.alive(service));
    }
}

TEST_CASE("JN11 a Persistent instance under a cleared folder moves to its service, with a fresh GUID",
          "[join][persistent]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId level = add_folder(game, "Level", workspace);
    const InstanceId hud = add_folder(game, "Hud", level);
    game.set_persistent(hud, true);
    const InstanceId bar = add_folder(game, "Bar", hud);
    game.capture_place();
    game.start_simulation();
    const std::string hud_guid = game.guid(hud);
    const std::string bar_guid = game.guid(bar);

    engine_network::swap_for_client(game);

    REQUIRE_FALSE(game.alive(level));
    REQUIRE(game.alive(hud));
    REQUIRE(game.alive(bar));
    REQUIRE(game.parent(hud) == workspace);
    REQUIRE(game.parent(bar) == hud);
    REQUIRE(game.name(hud) == "Hud");
    REQUIRE(game.guid(hud) != hud_guid);
    REQUIRE(game.guid(bar) != bar_guid);
    REQUIRE(engine_core::valid_guid(game.guid(hud)));
    REQUIRE(game.find_guid(game.guid(hud)) == hud);
    REQUIRE_FALSE(game.find_guid(hud_guid).has_value());
}

TEST_CASE("JN12 at the flip to Client a Persistent Server script stops and a Client script keeps its state",
          "[join][persistent]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    counter(game, "ServerCounter", "count_server", engine_core::RunContext::Server);
    counter(game, "ClientCounter", "count_client", engine_core::RunContext::Client);
    game.capture_place();
    game.start_simulation();
    rig.frames(5);
    INFO(rig.runtime.last_error());
    // A script starts inside its first frame, after that frame's Heartbeat: 4 counts in 5 frames.
    const double server_before = number(rig.runtime, "count_server");
    const double client_before = number(rig.runtime, "count_client");
    REQUIRE(server_before == 4);
    REQUIRE(client_before == 4);

    game.set_world_role(engine_core::WorldRole::Client);
    rig.frames(5);

    REQUIRE(number(rig.runtime, "count_server") == server_before);
    // Every one of the 5 frames after the flip counted, on the same state.
    REQUIRE(number(rig.runtime, "count_client") == client_before + 5);
}

TEST_CASE("JN13 at the flip back to Standalone a Server script that never ran starts", "[join]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    engine_core::Script& script = add_script(game, "Starter", "_G.started = true");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    game.set_world_role(engine_core::WorldRole::Client);
    game.capture_place();
    game.start_simulation();
    rig.frames(1);
    bool started = false;
    REQUIRE_FALSE(rig.runtime.global_boolean("started", started));

    game.set_world_role(engine_core::WorldRole::Standalone);
    rig.frames(1);

    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.global_boolean("started", started));
    REQUIRE(started);
}
