// AnarchyServer: its arguments, the projects it refuses, and a real UDP client
// served by it (src/server/ServerHost.hpp).

#include "support.hpp"

#include "Engine.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "Project.hpp"
#include "ProjectIdentity.hpp"
#include "ScriptAnalysis.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"
#include "runner/GamePack.hpp"
#include "server/ServerHost.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// A project on disk with one Server-context script that adds Workspace.FromServer when play starts,
// and fillers empty Folders beside it, to make its snapshot bigger.
void make_served_project(const fs::path& root, int fillers = 0) {
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

// The identity a client of the project at root presents.
network::GameIdentity identity_of(const fs::path& root) {
    engine_core::Project project = engine_core::Project::load(root);
    network::GameIdentity identity;
    std::string error;
    REQUIRE(engine_network::project_identity(project, identity, error));
    return identity;
}

// A client engine, never started and stepped by hand, with a session on a real UDP socket.
struct UdpClient {
    engine_core::Engine engine;
    std::unique_ptr<engine_network::ClientSession> session;

    explicit UdpClient(const network::GameIdentity& identity) {
        engine.analysis().set_enabled(false);
        std::string error;
        std::unique_ptr<network::UdpSocket> socket = network::UdpSocket::open(0, error);
        INFO(error);
        REQUIRE(socket != nullptr);
        engine_network::SessionConfig config;
        config.endpoint.identity = identity;
        session = std::make_unique<engine_network::ClientSession>(engine.datamodel(), std::move(socket), config);
        engine.set_network(session.get());
        engine.resume();
    }
    ~UdpClient() {
        engine.set_network(nullptr);
        session.reset();
    }
    engine_network::ClientState state() const { return session->state(); }
    bool sees(const char* name) {
        engine_core::DataModel& game = engine.datamodel();
        return game.find_first_child(game.scene_service("Workspace"), name) != 0;
    }
    // Steps until done() or the real-time limit passes.
    template <class Done>
    void step_until(Done done, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            engine.run_frame(1.0 / 60.0);
        }
    }
};

}  // namespace

TEST_CASE("SV1 parse_server_args reads defaults and flags, and refuses the rest", "[server]") {
    std::string error;
    {
        const std::optional<server::ServerOptions> options = server::parse_server_args({}, error);
        REQUIRE(options.has_value());
        REQUIRE(options->project.empty());
        REQUIRE(options->port == 7777);
        REQUIRE(options->max_connections == 64);
        REQUIRE(options->max_send == 1048576.0);
    }
    {
        const std::optional<server::ServerOptions> options = server::parse_server_args(
            {"proj", "--port", "9000", "--max-connections", "8", "--max-send", "500000"}, error);
        INFO(error);
        REQUIRE(options.has_value());
        REQUIRE(options->project == fs::path("proj"));
        REQUIRE(options->port == 9000);
        REQUIRE(options->max_connections == 8);
        REQUIRE(options->max_send == 500000.0);
    }
    const std::vector<std::vector<std::string>> refused = {
        {"--port", "70000"}, {"--port"}, {"--max-connections", "0"}, {"--max-connections", "65"},
        {"--max-send", "0"}, {"--bogus"}, {"one", "two"},
    };
    for (const std::vector<std::string>& args : refused) {
        error.clear();
        INFO(args.front());
        REQUIRE_FALSE(server::parse_server_args(args, error).has_value());
        REQUIRE(error.find("Usage") != std::string::npos);
    }
}

TEST_CASE("SV2 a project that cannot identify its game is refused", "[server]") {
    SimRole role;
    TempDir dir;
    std::string error;
    SECTION("a zeroed gameId") {
        { engine_core::Project::create(dir.path); }
        const fs::path json = dir.path / "project.json";
        std::string text;
        {
            std::ifstream in(json, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::size_t key = text.find("\"gameId\"");
        REQUIRE(key != std::string::npos);
        const std::size_t open = text.find('"', text.find(':', key));
        REQUIRE(text.find('"', open + 1) - open - 1 == 32);
        text.replace(open + 1, 32, std::string(32, '0'));
        {
            std::ofstream out(json, std::ios::binary | std::ios::trunc);
            out << text;
        }
        server::ServerOptions options;
        options.project = dir.path;
        options.port = 0;
        REQUIRE(server::ServerHost::open(options, error) == nullptr);
        INFO(error);
        REQUIRE(error.find("gameId") != std::string::npos);
    }
    SECTION("a missing folder") {
        server::ServerOptions options;
        options.project = dir.path / "missing";
        options.port = 0;
        REQUIRE(server::ServerHost::open(options, error) == nullptr);
        REQUIRE_FALSE(error.empty());
    }
}

TEST_CASE("SV3 a port in use is refused", "[server]") {
    SimRole role;
    TempDir dir;
    { engine_core::Project::create(dir.path); }
    server::ServerOptions options;
    options.project = dir.path;
    options.port = 0;
    std::string error;
    std::unique_ptr<server::ServerHost> a = server::ServerHost::open(options, error);
    INFO(error);
    REQUIRE(a != nullptr);
    REQUIRE(a->port() != 0);
    options.port = a->port();
    error.clear();
    REQUIRE(server::ServerHost::open(options, error) == nullptr);
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("SV4 a server and a real UDP client: a server script's change arrives, and stopping says \"The server closed\"", "[server]") {
    SimRole role;
    TempDir dir;
    server::ServerOptions options;
    options.project = dir.path;
    options.port = 0;
    std::string error;

    SECTION("joined, then stopped") {
        make_served_project(dir.path);
        const network::GameIdentity identity = identity_of(dir.path);
        std::unique_ptr<server::ServerHost> host = server::ServerHost::open(options, error);
        INFO(error);
        REQUIRE(host != nullptr);
        host->start();
        UdpClient client(identity);
        REQUIRE(client.session->connect(network::Address::loopback(host->port()), engine_core::Engine::network_now()));
        client.step_until([&] { return client.state() == engine_network::ClientState::Joined && client.sees("FromServer"); },
                          std::chrono::seconds(5));
        INFO(client.session->reason());
        REQUIRE(client.state() == engine_network::ClientState::Joined);
        REQUIRE(client.sees("FromServer"));
        host->stop();
        client.step_until([&] { return client.state() == engine_network::ClientState::Disconnected; },
                          std::chrono::seconds(2));
        REQUIRE(client.state() == engine_network::ClientState::Disconnected);
        REQUIRE(client.session->reason() == "The server closed");
    }
    SECTION("stopped while the client is joining") {
        // A small world's snapshot arrives with the accept, so the client would go
        // straight to Joined. A bigger world under a tight budget spreads it over many ticks.
        make_served_project(dir.path, 400);
        const network::GameIdentity identity = identity_of(dir.path);
        options.max_send = 2048.0;
        std::unique_ptr<server::ServerHost> host = server::ServerHost::open(options, error);
        INFO(error);
        REQUIRE(host != nullptr);
        host->start();
        UdpClient client(identity);
        REQUIRE(client.session->connect(network::Address::loopback(host->port()), engine_core::Engine::network_now()));
        client.step_until([&] { return client.state() != engine_network::ClientState::Connecting; },
                          std::chrono::seconds(5));
        INFO(client.session->reason());
        REQUIRE(client.state() == engine_network::ClientState::Joining);
        const auto stopped = std::chrono::steady_clock::now();
        host->stop();
        client.step_until([&] { return client.state() == engine_network::ClientState::Disconnected; },
                          std::chrono::seconds(2));
        REQUIRE(client.state() == engine_network::ClientState::Disconnected);
        REQUIRE(client.session->reason() == "The server closed");
        REQUIRE(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(2));
    }
}

TEST_CASE("SV5 a game packed into a file serves as its folder would", "[server]") {
    SimRole role;
    TempDir dir;
    make_served_project(dir.path / "project");
    const network::GameIdentity identity = identity_of(dir.path / "project");

    std::vector<runner::PackFile> files;
    std::string error;
    REQUIRE(runner::add_pack_folder(dir.path / "project", std::string(runner::kPackedProject) + "/", files, error));
    const fs::path pack = dir.path / "x.bin";
    INFO(error);
    REQUIRE(runner::write_game_pack({}, files, pack, error));

    server::ServerOptions options;
    options.pack_file = pack;
    options.port = 0;
    std::unique_ptr<server::ServerHost> host = server::ServerHost::open(options, error);
    CAPTURE(error);
    REQUIRE(host != nullptr);
    const network::GameIdentity& served = host->session().endpoint().config().identity;
    REQUIRE(served.game_id == identity.game_id);
    REQUIRE(served.version == identity.version);
    REQUIRE(served.resources_hash == identity.resources_hash);

    // Each run's game has its own id, so its unpacked folder would pile up in the temp folder.
    host.reset();
    const std::optional<runner::PackLocation> where = runner::find_game_pack(pack);
    fs::path unpacked;
    std::error_code failure;
    if (where && runner::unpack_game(pack, *where, fs::temp_directory_path(failure) / "AnarchyEngine" / "servers",
                                     unpacked, error)) {
        fs::remove_all(unpacked, failure);
    }
}
