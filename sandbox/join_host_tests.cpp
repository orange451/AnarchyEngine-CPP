// ClientJoinHost: a script's join against a real AnarchyServer host on
// loopback UDP, the swap at the snapshot, and --connect through the same host
// (docs/superpowers/specs/2026-10-19-join-from-lua-design.md).

#include "support.hpp"

#include "ClientJoinHost.hpp"
#include "ClientWorld.hpp"
#include "ConnectionInstance.hpp"
#include "Connections.hpp"
#include "Engine.hpp"
#include "FileBytes.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "Gui.hpp"
#include "Project.hpp"
#include "ProjectIdentity.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "Sessions.hpp"
#include "Skybox.hpp"
#include "Socket.hpp"
#include "server/ServerHost.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using engine_core::InstanceId;
using engine_core::JoinState;

// A project with one Server-context script that adds Workspace.FromServer when
// play starts, and fillers empty Folders beside it, to make its snapshot bigger.
// client_source, when set, is a Client-context Script beside it, which replicates
// and runs on each client that joins.
void make_project(const fs::path& root, int fillers = 0, const std::string& client_source = {}) {
    engine_core::Engine authoring;
    engine_core::Project project = engine_core::Project::create(root, authoring.datamodel());
    engine_core::Script& script = add_script(authoring.datamodel(), "MakeFolder",
        "local f = Instance.new(\"Folder\") f.Name = \"FromServer\" f.Parent = workspace");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    engine_core::DataModel& game = authoring.datamodel();
    if (!client_source.empty()) {
        engine_core::Script& client = add_script(game, "OnClient", client_source.c_str());
        REQUIRE_FALSE(client.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    }
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

    // resources: a folder the host hashes at the first join; empty, identity is already whole.
    explicit JoinClient(const network::GameIdentity& identity, bool play = true, const fs::path& resources = {}) {
        engine.analysis().set_enabled(false);
        engine_core::DataModel& game = engine.datamodel();
        host = std::make_unique<engine_network::ClientJoinHost>(engine, identity, resources);
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
        // Only the frames before the join: the one that applies the snapshot does real work.
        if (client.connections().join_state() != JoinState::Joined) {
            slowest = std::max(slowest, std::chrono::steady_clock::now() - begun);
        }
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
    // A lookup that is still running when the host goes, and then answers.
    auto answered = std::make_shared<std::atomic<bool>>(false);
    client.host->set_lookup([answered](const std::string&, std::string&) -> std::optional<network::Address> {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        answered->store(true);
        return network::Address::loopback(1);
    });

    REQUIRE(client.join("slow:1") == "ok");
    REQUIRE(client.host->resolvers_running() == 1);
    REQUIRE_FALSE(answered->load());
    client.game().set_join_host(nullptr);
    client.host.reset();
    // The destructor waited for the lookup; its answer was dropped.
    REQUIRE(answered->load());
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
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
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
    // The connect, and its timeout, wait for the engine's first step.
    REQUIRE(client.host->session()->state() == engine_network::ClientState::Idle);
    REQUIRE(client.host->status().state == engine_network::ClientState::Connecting);
    client.frames(1);
    REQUIRE(client.host->session()->state() != engine_network::ClientState::Idle);
    REQUIRE(client.host->session()->state() != engine_network::ClientState::Disconnected);
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
    REQUIRE(client.host->status().state == engine_network::ClientState::Joined);
}

TEST_CASE("JN29 status() follows a script's join, and is Idle once its session is gone", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));
    REQUIRE(client.host->status().state == engine_network::ClientState::Idle);

    REQUIRE(client.join(loopback(server->port())) == "ok");
    // Still resolving: there is no session yet.
    REQUIRE(client.host->status().state == engine_network::ClientState::Idle);
    client.step_until([&] { return client.host->status().state == engine_network::ClientState::Joined; },
                      std::chrono::seconds(5));
    REQUIRE(client.host->status().state == engine_network::ClientState::Joined);

    server->stop();
    client.step_until([&] { return client.heard("left:"); }, std::chrono::seconds(2));
    REQUIRE(client.heard("left:The server closed;"));
    const engine_network::ClientStatus after = client.host->status();
    REQUIRE(after.state == engine_network::ClientState::Idle);
    REQUIRE(after.reason.empty());
}

TEST_CASE("JN30 an unhashed identity hashes the resources at the first join", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    engine_core::Project project = engine_core::Project::load(dir.path);
    network::GameIdentity identity;
    fs::path resources;
    std::string error;
    REQUIRE(engine_network::project_identity_unhashed(project, identity, resources, error));
    REQUIRE(identity.resources_hash == 0);

    SECTION("a readable folder joins") {
        JoinClient client(identity, true, resources);
        REQUIRE(client.join(loopback(server->port())) == "ok");
        client.step_until([&] { return client.heard("joined;") || client.heard("left:"); }, std::chrono::seconds(5));
        REQUIRE(client.events() == "joined;");
    }
    SECTION("a folder that cannot be read fails the join, with why") {
        const fs::path missing = dir.path / "no-such-folder";
        JoinClient client(identity, true, missing);
        REQUIRE(client.join(loopback(server->port())) == "ok");
        client.step_until([&] { return client.heard("left:"); }, std::chrono::seconds(5));
        REQUIRE(client.events() == "left:Could not read the project's resources folder (" +
                                       engine_core::utf8_path(missing) + ");");
        REQUIRE(client.connections().join_state() == JoinState::Idle);
        REQUIRE(client.game().alive(client.map));
    }
}

TEST_CASE("JN31 a script's Leave during --connect stops play between steps, not under the script", "[join]") {
    SimRole role;
    TempDir dir;
    // A Client script from the server leaves as soon as it runs, inside the play
    // VM that the leave closes, and carries on: the leave waits for the next step.
    make_project(dir.path, 0,
                 "local connections = game:GetService('Connections')\n"
                 "connections:Leave()\n"
                 "print('after leave: ' .. tostring(connections.JoinState))\n");
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    std::string left;
    JoinClient client(identity_for(dir.path), false);
    // Play stops with the leave, and the play VM with it, so Left is heard from here.
    const auto link = client.connections().on_left.connect([&left](const std::string& reason) { left += reason + ";"; });

    std::string error;
    REQUIRE(client.host->connect_from_empty(network::Address::loopback(server->port()), error));
    std::string printed;
    std::string errors;
    client.step_until(
        [&] {
            for (const auto& line : client.engine.scripts().drain_output().lines) {
                if (line.kind == engine_core::ScriptRuntime::OutputKind::Error) {
                    errors += line.text;
                }
                printed += line.text;
            }
            return !left.empty();
        },
        std::chrono::seconds(5));
    client.frames(5);
    client.connections().on_left.disconnect(link);

    INFO(printed);
    INFO(errors);
    REQUIRE(left == "You left the game;");
    REQUIRE(errors.empty());
    // The script ran on after its Leave, with the connection still up.
    REQUIRE(printed.find("after leave: Enum.JoinState.Joined") != std::string::npos);
    REQUIRE_FALSE(client.game().simulation_running());
    REQUIRE(client.host->status().state == engine_network::ClientState::Disconnected);
    REQUIRE(client.host->status().reason == "You left the game");
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
}

TEST_CASE("JN32 a second Join while the first still looks its server up: only the second's answer counts", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));
    const std::uint16_t port = server->port();
    client.host->set_lookup([port](const std::string& address, std::string& error) -> std::optional<network::Address> {
        if (address == "slow:1") {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            error = "the first lookup";
            return std::nullopt;
        }
        return network::Address::loopback(port);
    });

    REQUIRE(client.join("slow:1") == "ok");
    REQUIRE(client.command("Leave()") == "ok");
    REQUIRE(client.join("fast:1") == "ok");
    client.step_until([&] { return client.heard("joined;"); }, std::chrono::seconds(5));
    // The first lookup answers late, and that answer must change nothing.
    client.step_until([&] { return client.host->resolvers_running() == 0; }, std::chrono::seconds(5));
    REQUIRE(client.host->resolvers_running() == 0);
    client.frames(10);

    REQUIRE(client.events() == "left:You left the game;joined;");
    REQUIRE(client.connections().join_state() == JoinState::Joined);
    REQUIRE(client.sees("FromServer"));
}

TEST_CASE("JN33 at most four lookups run at once, and a Join past that is refused", "[join]") {
    SimRole role;
    JoinClient client(network::GameIdentity{});
    auto release = std::make_shared<std::atomic<bool>>(false);
    client.host->set_lookup([release](const std::string&, std::string& error) -> std::optional<network::Address> {
        while (!release->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        error = "released";
        return std::nullopt;
    });
    // Destroyed before the client, so a failed check still lets the host's destructor join its lookups.
    struct Release {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Release() { flag->store(true); }
    } releaser{release};

    for (int i = 0; i < engine_network::ClientJoinHost::kMaxLookups; ++i) {
        REQUIRE(client.join("held:1") == "ok");
        REQUIRE(client.command("Leave()") == "ok");
    }
    REQUIRE(client.host->resolvers_running() == engine_network::ClientJoinHost::kMaxLookups);
    const std::string refused = client.join("held:1");
    INFO(refused);
    REQUIRE(refused.find("Too many joins still looking up their server; try again in a moment") != std::string::npos);
    REQUIRE(client.connections().join_state() == JoinState::Idle);

    release->store(true);
    client.step_until([&] { return client.host->resolvers_running() == 0; }, std::chrono::seconds(5));
    client.frames(2);
    REQUIRE(client.join("held:1") == "ok");
    client.step_until([&] { return client.heard("left:Could not find"); }, std::chrono::seconds(2));
    REQUIRE(client.heard("left:Could not find the server: released;"));
}

TEST_CASE("JN34 Studio's Stop after a join brings the place back with no Connection left", "[join]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));
    REQUIRE(client.join(loopback(server->port())) == "ok");
    client.step_until([&] { return client.heard("joined;") && client.sees("FromServer"); }, std::chrono::seconds(5));
    REQUIRE(client.connections().local_connection() != 0);
    REQUIRE_FALSE(client.game().get_children(client.game().connections()).empty());

    // As stop_test does: play stops first, then the connection ends without Left, then the Connections go.
    client.game().stop_simulation();
    client.host->end_silently();
    engine_network::clear_connections(client.game());
    client.frames(5);

    REQUIRE(client.sees("Map"));
    REQUIRE_FALSE(client.sees("FromServer"));
    REQUIRE(client.game().get_children(client.game().connections()).empty());
    REQUIRE(client.connections().local_connection() == 0);
    REQUIRE(client.game().world_role() == engine_core::WorldRole::Standalone);
    REQUIRE(client.connections().join_state() == JoinState::Idle);
    REQUIRE_FALSE(client.game().simulation_running());
}

TEST_CASE("JN35 a Persistent instance its service refuses goes at the swap, and the developer is told",
          "[join][persistent]") {
    SimRole role;
    TempDir dir;
    make_project(dir.path);
    std::unique_ptr<server::ServerHost> server = serve(dir.path);
    JoinClient client(identity_for(dir.path));
    engine_core::DataModel& game = client.game();
    engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
    game.set_parent(screen.id(), game.scene_service("Gui"));
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), screen.id());
    engine_core::Skybox& sky = game.create<engine_core::Skybox>();
    game.set_name(sky.id(), "Backdrop");
    game.set_parent(sky.id(), pane.id());
    game.set_persistent(sky.id(), true);
    const InstanceId kept = sky.id();

    REQUIRE(client.join(loopback(server->port())) == "ok");
    client.step_until([&] { return client.heard("joined;"); }, std::chrono::seconds(5));
    REQUIRE(client.heard("joined;"));

    std::string errors;
    for (const auto& line : client.engine.scripts().drain_output().lines) {
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error) {
            errors += line.text;
        }
    }
    REQUIRE(errors == "Persistent Skybox \"Backdrop\" could not move to Gui for the join, so it was destroyed: "
                      "A Skybox must be in Lighting or a ViewportPane\n");
    REQUIRE_FALSE(game.alive(kept));
}

TEST_CASE("JN36 clear_connections destroys every Connection and clears LocalConnection", "[join]") {
    SimRole role;
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::ConnectionInstance& mine = game.create<engine_core::ConnectionInstance>();
    game.set_parent(mine.id(), game.connections());
    engine_core::ConnectionInstance& theirs = game.create<engine_core::ConnectionInstance>();
    game.set_parent(theirs.id(), game.connections());
    const InstanceId mine_id = mine.id();
    const InstanceId theirs_id = theirs.id();
    auto* list = dynamic_cast<engine_core::Connections*>(game.instance(game.connections()));
    REQUIRE(list != nullptr);
    list->set_local_connection(mine_id);

    engine_network::clear_connections(game);

    REQUIRE(game.get_children(game.connections()).empty());
    REQUIRE_FALSE(game.alive(mine_id));
    REQUIRE_FALSE(game.alive(theirs_id));
    REQUIRE(list->local_connection() == 0);
}
