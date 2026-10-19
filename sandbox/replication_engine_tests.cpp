// What tree replication needs from the engine: a network pump at each step's
// ends, a client world that never steps physics, and two worlds on one thread
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "support.hpp"

#include "Connections.hpp"
#include "Engine.hpp"
#include "GameIdentity.hpp"
#include "GameObject.hpp"
#include "INetworkPump.hpp"
#include "PhysicsObject.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
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

TEST_CASE("RP48 a server engine and a client engine: the client sees the server's script move a part, and runs its own", "[replication][engine]") {
    SimRole role;
    network::LinkHub hub;
    network::GameIdentity id;
    network::parse_game_id("0123456789abcdef0123456789abcdef", id.game_id);
    id.resources_hash = 7;

    engine_core::Engine server;
    server.analysis().set_enabled(false);
    engine_core::DataModel& sg = server.datamodel();
    sg.set_world_role(engine_core::WorldRole::Server);
    engine_core::GameObject& part = sg.create<engine_core::GameObject>();
    sg.set_name(part.id(), "Part");
    sg.set_parent(part.id(), sg.scene_service("Workspace"));
    engine_core::Script& mover = add_script(sg, "Mover",
        "game:GetService('RunService').Heartbeat:Connect(function()\n"
        "    workspace.Part.Transform = workspace.Part.Transform + Vector3.new(1, 0, 0)\n"
        "end)");
    REQUIRE_FALSE(mover.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    engine_core::Script& reader = add_script(sg, "Reader",
        "_G.my_id = game:GetService('Connections').LocalConnection.Id");
    REQUIRE_FALSE(reader.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    engine_network::SessionConfig server_config;
    server_config.endpoint.identity = id;
    engine_network::ServerSession server_session(sg, hub.open(network::Address::loopback(1)), server_config);
    server.set_network(&server_session);
    sg.start_simulation();
    server.resume();

    engine_core::Engine client;
    client.analysis().set_enabled(false);
    engine_core::DataModel& cg = client.datamodel();
    engine_network::SessionConfig client_config;
    client_config.endpoint.identity = id;
    engine_network::ClientSession client_session(cg, hub.open(network::Address::loopback(2)), client_config);
    client.set_network(&client_session);
    client.resume();
    // The connect starts on the clock the engines pump the sessions with.
    const double started = engine_core::Engine::network_now();
    REQUIRE(client_session.connect(network::Address::loopback(1), started));

    // The engines read the steady clock, so the handshake takes real time: step
    // until the client joins (5 seconds at most), then a second more of play.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (client_session.state() != engine_network::ClientState::Joined &&
           std::chrono::steady_clock::now() < deadline) {
        server.run_frame(1.0 / 60.0);
        client.run_frame(1.0 / 60.0);
    }
    for (int i = 0; i < 60; ++i) {
        server.run_frame(1.0 / 60.0);
        client.run_frame(1.0 / 60.0);
    }
    server.set_network(nullptr);
    client.set_network(nullptr);
    INFO(client_session.reason());
    REQUIRE(client_session.state() == engine_network::ClientState::Joined);
    const InstanceId seen = cg.find_first_child(cg.scene_service("Workspace"), "Part");
    REQUIRE(seen != 0);
    const float server_x = part.transform().m[12];
    const float client_x = static_cast<engine_core::GameObject*>(cg.instance(seen))->transform().m[12];
    REQUIRE(server_x > 30.0f);
    REQUIRE(client_x > 20.0f);   // a few ticks behind the server, never ahead
    REQUIRE(client_x <= server_x);
    std::string my_id;
    REQUIRE(client.scripts().global_string("my_id", my_id));
    REQUIRE(my_id.size() == 36);
}
