// ClientJoinHost: a script's join against a real AnarchyServer host on
// loopback UDP, the swap at the snapshot, and --connect through the same host
// (docs/superpowers/specs/2026-10-19-join-from-lua-design.md).

#include "support.hpp"

#include "ClientJoinHost.hpp"
#include "Connections.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "Project.hpp"
#include "ProjectIdentity.hpp"
#include "ScriptAnalysis.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"
#include "server/ServerHost.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;
using engine_core::InstanceId;
using engine_core::JoinState;

// A project with one Server-context script that adds Workspace.FromServer when
// play starts, and fillers empty Folders beside it, to make its snapshot bigger.
void make_project(const fs::path& root, int fillers = 0) {
    engine_core::Engine authoring;
    engine_core::Project project = engine_core::Project::create(root, authoring.datamodel());
    engine_core::Script& script = add_script(authoring.datamodel(), "MakeFolder",
        "local f = Instance.new(\"Folder\") f.Name = \"FromServer\" f.Parent = workspace");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    engine_core::DataModel& game = authoring.datamodel();
    for (int i = 0; i < fillers; ++i) {
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), "Filler" + std::to_string(i));
        game.set_parent(folder.id(), game.scene_service("Workspace"));
    }
    project.save();
}

network::GameIdentity identity_for(const fs::path& root) {
    engine_core::Project project = engine_core::Project::load(root);
    network::GameIdentity identity;
    std::string error;
    REQUIRE(engine_network::project_identity(project, identity, error));
    return identity;
}

// A started server on any free port, serving the project at root.
std::unique_ptr<server::ServerHost> serve(const fs::path& root, double max_send = engine_network::kDefaultSendBudget) {
    server::ServerOptions options;
    options.project = root;
    options.port = 0;
    options.max_send = max_send;
    std::string error;
    std::unique_ptr<server::ServerHost> host = server::ServerHost::open(options, error);
    INFO(error);
    REQUIRE(host != nullptr);
    host->start();
    return host;
}

// How many Connections the running server holds, asked on its own thread; -1 when it does not answer.
int server_connections(server::ServerHost& host) {
    auto answer = std::make_shared<std::promise<int>>();
    std::future<int> count = answer->get_future();
    host.engine().on_simulation([answer](engine_core::DataModel& game) {
        answer->set_value(static_cast<int>(game.get_children(game.connections()).size()));
    });
    return count.wait_for(std::chrono::seconds(1)) == std::future_status::ready ? count.get() : -1;
}

// A client engine, never started and stepped by hand on this thread, with a
// ClientJoinHost installed. Its menu world: Workspace/Map, Gui/Loading
// (Persistent), and a Persistent Client Script that records Joined and Left in _G.events.
struct JoinClient {
    engine_core::Engine engine;
    std::unique_ptr<engine_network::ClientJoinHost> host;
    InstanceId map = 0;
    InstanceId loading = 0;

    explicit JoinClient(const network::GameIdentity& identity, bool play = true) {
        engine.analysis().set_enabled(false);
        engine_core::DataModel& game = engine.datamodel();
        host = std::make_unique<engine_network::ClientJoinHost>(engine, identity);
        game.set_join_host(host.get());
        engine.resume();
        engine_core::Folder& map_folder = game.create<engine_core::Folder>();
        game.set_name(map_folder.id(), "Map");
        game.set_parent(map_folder.id(), game.scene_service("Workspace"));
        map = map_folder.id();
        engine_core::Folder& loading_folder = game.create<engine_core::Folder>();
        game.set_name(loading_folder.id(), "Loading");
        game.set_parent(loading_folder.id(), game.scene_service("Gui"));
        game.set_persistent(loading_folder.id(), true);
        loading = loading_folder.id();
        engine_core::Script& recorder = add_script(game, "Recorder",
            "_G.events = ''\n"
            "local connections = game:GetService('Connections')\n"
            "connections.Joined:Connect(function() _G.events ..= 'joined;' end)\n"
            "connections.Left:Connect(function(reason) _G.events ..= 'left:' .. reason .. ';' end)\n");
        REQUIRE_FALSE(recorder.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
        game.set_persistent(recorder.id(), true);
        if (play) {
            game.capture_place();
            game.start_simulation();
            frames(2);
            INFO(engine.scripts().last_error());
            REQUIRE(events().empty());
        }
    }
    ~JoinClient() {
        engine.datamodel().set_join_host(nullptr);
        host.reset();
    }

    engine_core::DataModel& game() { return engine.datamodel(); }
    engine_core::Connections& connections() {
        auto* list = dynamic_cast<engine_core::Connections*>(game().instance(game().connections()));
        REQUIRE(list != nullptr);
        return *list;
    }
    std::string events() {
        std::string out = "(no recorder)";
        engine.scripts().global_string("events", out);
        return out;
    }
    bool heard(const std::string& text) { return events().find(text) != std::string::npos; }
    bool sees(const char* name) { return game().find_first_child(game().scene_service("Workspace"), name) != 0; }
    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            engine.run_frame(1.0 / 60.0);
        }
    }
    // Steps until done() or the real-time limit passes.
    template <class Done>
    void step_until(Done done, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            engine.run_frame(1.0 / 60.0);
        }
    }
    // Runs call on Connections from the command line; "ok" or what it raised.
    std::string command(const std::string& call) {
        engine.scripts().drain_output();
        engine.scripts().run_chunk("local ok, message = pcall(function() game:GetService('Connections'):" + call +
                                   " end)\nprint(ok and 'ok' or ('error: ' .. tostring(message)))\n");
        const engine_core::ScriptRuntime::OutputBatch batch = engine.scripts().drain_output();
        for (const auto& line : batch.lines) {
            if (line.text == "ok\n" || line.text.rfind("error: ", 0) == 0) {
                return line.text == "ok\n" ? "ok" : line.text;
            }
        }
        return "nothing printed";
    }
    std::string join(const std::string& address) { return command("Join('" + address + "')"); }
};

std::string loopback(std::uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

}  // namespace

TEST_CASE("JN20 Join builds the server's world and keeps the Persistent ones", "[join][persistent]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));

    REQUIRE(client.join(loopback(server->port())) == "ok");
    REQUIRE(client.connections().join_state() == JoinState::Joining);
    client.step_until([&] { return client.heard("joined;") && client.sees("FromServer"); }, std::chrono::seconds(5));

    REQUIRE(client.events() == "joined;");
    REQUIRE(client.sees("FromServer"));
    REQUIRE_FALSE(client.game().alive(client.map));
    REQUIRE_FALSE(client.sees("Map"));
    REQUIRE(client.game().find_first_child(client.game().scene_service("Gui"), "Loading") != 0);
    REQUIRE(client.connections().join_state() == JoinState::Joined);
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Client);
    REQUIRE(client.connections().local_connection() != 0);
    REQUIRE(client.game().simulation_running());
}

TEST_CASE("JN21 Join by name resolves without blocking the frame", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));

    REQUIRE(client.join("localhost:" + std::to_string(server->port())) == "ok");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto slowest = std::chrono::steady_clock::duration::zero();
    while (!client.heard("joined;") && std::chrono::steady_clock::now() < deadline) {
        const auto begun = std::chrono::steady_clock::now();
        client.engine.run_frame(1.0 / 60.0);
        slowest = std::max(slowest, std::chrono::steady_clock::now() - begun);
    }

    INFO(client.events());
    REQUIRE(client.heard("joined;"));
    REQUIRE(slowest < std::chrono::milliseconds(100));
}

TEST_CASE("JN22 a refused join leaves the menu untouched", "[join][persistent]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    network::GameIdentity other = identity_for(dir.path);
    other.game_id[0] ^= 0xFF;
    JoinClient client(other);
    const std::string map_guid = client.game().guid(client.map);
    const std::string loading_guid = client.game().guid(client.loading);

    REQUIRE(client.join(loopback(server->port())) == "ok");
    client.step_until([&] { return client.heard("left:"); }, std::chrono::seconds(5));

    REQUIRE(client.events() == "left:This server runs a different game;");
    REQUIRE(client.game().alive(client.map));
    REQUIRE(client.game().alive(client.loading));
    REQUIRE(client.game().guid(client.map) == map_guid);
    REQUIRE(client.game().guid(client.loading) == loading_guid);
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    REQUIRE(client.game().simulation_running());
}

TEST_CASE("JN23 Leave while the snapshot is still arriving leaves the menu untouched", "[join][persistent]") {
    SimRole role;
    TempDir dir;
    // A bigger world under a tight budget spreads its snapshot over many ticks, as in SV4.
    make_project(dir.path, 400);
    std::unique_ptr<server::ServerHost> server = serve(dir.path, 2048.0);
    JoinClient client(identity_for(dir.path));
    const std::string map_guid = client.game().guid(client.map);

    REQUIRE(client.join(loopback(server->port())) == "ok");
    client.step_until(
        [&] {
            return client.host->session() != nullptr &&
                   client.host->session()->state() == engine_network::ClientState::Joining;
        },
        std::chrono::seconds(5));
    REQUIRE(client.host->session() != nullptr);
    REQUIRE(client.host->session()->state() == engine_network::ClientState::Joining);
    REQUIRE(client.connections().join_state() == JoinState::Joining);

    REQUIRE(client.command("Leave()") == "ok");
    client.step_until([&] { return client.heard("left:"); }, std::chrono::seconds(2));

    REQUIRE(client.events() == "left:You left the game;");
    REQUIRE(client.game().alive(client.map));
    REQUIRE(client.game().guid(client.map) == map_guid);
    REQUIRE(client.game().alive(client.loading));
    REQUIRE_FALSE(client.sees("Filler0"));
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    REQUIRE(client.host->session() == nullptr);
}

TEST_CASE("JN24 a resolve that finishes after Leave is dropped", "[join]") {
    SimRole role;
    JoinClient client(network::GameIdentity{});

    REQUIRE(client.join("no-such-host.invalid:1") == "ok");
    REQUIRE(client.command("Leave()") == "ok");
    // The lookup finishes on its own time; its answer then waits for the next frames.
    client.step_until([&] { return client.host->resolvers_running() == 0; }, std::chrono::seconds(20));
    REQUIRE(client.host->resolvers_running() == 0);
    client.frames(10);

    REQUIRE(client.events() == "left:You left the game;");
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    REQUIRE(client.host->session() == nullptr);
}

TEST_CASE("JN25 destroying the host while it resolves is safe", "[join]") {
    SimRole role;
    JoinClient client(network::GameIdentity{});

    REQUIRE(client.join("localhost:1") == "ok");
    client.game().set_join_host(nullptr);
    client.host.reset();
    client.frames(60);

    REQUIRE(client.events().empty());
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
    REQUIRE(client.game().alive(client.map));
}

TEST_CASE("JN26 end_silently while joined ends the connection without Left", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));
    REQUIRE(client.join(loopback(server->port())) == "ok");
    client.step_until([&] { return client.heard("joined;"); }, std::chrono::seconds(5));
    REQUIRE(client.heard("joined;"));
    REQUIRE(server_connections(*server) == 1);

    client.host->end_silently();
    client.frames(10);

    REQUIRE(client.events() == "joined;");
    REQUIRE(client.host->session() == nullptr);
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int held = server_connections(*server);
    while (held != 0 && std::chrono::steady_clock::now() < deadline) {
        held = server_connections(*server);
    }
    REQUIRE(held == 0);
}

TEST_CASE("JN27 after the server closes, the world keeps running locally", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));
    REQUIRE(client.join(loopback(server->port())) == "ok");
    client.step_until([&] { return client.heard("joined;") && client.sees("FromServer"); }, std::chrono::seconds(5));
    REQUIRE(client.sees("FromServer"));

    server->stop();
    client.step_until([&] { return client.heard("left:"); }, std::chrono::seconds(2));

    REQUIRE(client.events() == "joined;left:The server closed;");
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
    REQUIRE(client.game().simulation_running());
    REQUIRE(client.sees("FromServer"));
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    client.frames(2);
    REQUIRE(client.join(loopback(server->port())) == "ok");
    REQUIRE(client.connections().join_state() == JoinState::Joining);
}

TEST_CASE("JN28 --connect through connect_from_empty behaves as in 2c", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path), false);
    REQUIRE_FALSE(client.game().simulation_running());

    std::string error;
    REQUIRE(client.host->connect_from_empty(network::Address::loopback(server->port()), error));
    INFO(error);
    REQUIRE_FALSE(client.game().alive(client.map));
    REQUIRE_FALSE(client.game().alive(client.loading));
    REQUIRE(client.host->session() != nullptr);
    client.step_until(
        [&] {
            return client.host->session()->state() == engine_network::ClientState::Joined && client.sees("FromServer");
        },
        std::chrono::seconds(5));

    INFO(client.host->session()->reason());
    REQUIRE(client.host->session()->state() == engine_network::ClientState::Joined);
    REQUIRE(client.sees("FromServer"));
    REQUIRE(client.game().simulation_running());
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Client);
    REQUIRE(client.connections().join_state() == JoinState::Joined);
}
