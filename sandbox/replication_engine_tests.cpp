// What tree replication needs from the engine: a network pump at each step's
// ends, a client world that never steps physics, and two worlds on one thread
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "support.hpp"

#include "Engine.hpp"
#include "GameObject.hpp"
#include "INetworkPump.hpp"
#include "PhysicsObject.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using engine_core::InstanceId;

namespace {

// Writes 1 into First when the step starts, and records at the step's end
// whether a Heartbeat script had copied it into Second by then.
struct OrderPump : engine_core::INetworkPump {
    engine_core::DataModel* game = nullptr;
    InstanceId first = 0;
    InstanceId second = 0;
    std::vector<double> receives;
    std::vector<double> seen_at_send;   // Second's value at each send
    void receive(double now) override {
        receives.push_back(now);
        static_cast<engine_core::NumberValue*>(game->instance(first))->set_value(1.0);
    }
    void send(double) override {
        seen_at_send.push_back(static_cast<engine_core::NumberValue*>(game->instance(second))->value());
    }
};

}  // namespace

TEST_CASE("RP1 the engine pumps the network before a step's scripts and after them", "[replication][engine]") {
    SimRole role;
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::NumberValue& first = game.create<engine_core::NumberValue>();
    game.set_name(first.id(), "First");
    game.set_parent(first.id(), game.scene_service("Workspace"));
    engine_core::NumberValue& second = game.create<engine_core::NumberValue>();
    game.set_name(second.id(), "Second");
    game.set_parent(second.id(), game.scene_service("Workspace"));
    add_script(game, "Copy",
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "    workspace.Second.Value = workspace.First.Value * 2\n"
               "end)");
    OrderPump pump;
    pump.game = &game;
    pump.first = first.id();
    pump.second = second.id();
    engine.set_network(&pump);
    game.start_simulation();
    engine.resume();
    for (int i = 0; i < 4; ++i) {
        engine.run_frame(1.0 / 60.0);
    }
    engine.set_network(nullptr);
    REQUIRE(pump.receives.size() == 4);
    REQUIRE(pump.seen_at_send.size() == 4);
    for (std::size_t i = 1; i < pump.receives.size(); ++i) {
        REQUIRE(pump.receives[i] >= pump.receives[i - 1]);
    }
    // The script ran between receive and send: by the last send it had seen First == 1.
    REQUIRE(pump.seen_at_send.back() == 2.0);
}

TEST_CASE("RP2 a client world never steps physics; a server world does", "[replication][engine]") {
    for (const engine_core::WorldRole world : {engine_core::WorldRole::Client, engine_core::WorldRole::Server}) {
        SimRole role;
        engine_core::Engine engine;
        engine_core::DataModel& game = engine.datamodel();
        game.set_world_role(world);
        engine_core::GameObject& object = game.create<engine_core::GameObject>();
        engine_core::Matrix4 up = engine_core::matrix4_identity();
        up.m[13] = 10.0f;
        object.set_transform(up);
        game.set_parent(object.id(), game.scene_service("Workspace"));
        engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
        game.set_parent(body.id(), object.id());
        game.start_simulation();
        engine.resume();
        for (int i = 0; i < 30; ++i) {
            engine.run_frame(1.0 / 60.0);
        }
        const float y = object.transform().m[13];
        if (world == engine_core::WorldRole::Client) {
            REQUIRE(y == 10.0f);
        } else {
            REQUIRE(y < 10.0f);
        }
    }
}

TEST_CASE("RP3 two worlds on one thread run their own scripts", "[replication][engine]") {
    ScriptRig server;
    ScriptRig client;
    server.game.set_world_role(engine_core::WorldRole::Server);
    client.game.set_world_role(engine_core::WorldRole::Client);
    engine_core::Script& on_server = add_script(server.game, "Mark", "_G.where = 'server'");
    engine_core::Script& on_client = add_script(client.game, "Mark", "_G.where = 'client'");
    REQUIRE_FALSE(on_server.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    REQUIRE_FALSE(on_client.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    server.game.start_simulation();
    client.game.start_simulation();
    for (int i = 0; i < 3; ++i) {
        server.frames(1);
        client.frames(1);
    }
    std::string where;
    REQUIRE(server.runtime.global_string("where", where));
    REQUIRE(where == "server");
    REQUIRE(client.runtime.global_string("where", where));
    REQUIRE(where == "client");
}
