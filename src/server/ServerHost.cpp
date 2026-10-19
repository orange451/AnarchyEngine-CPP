#include "ServerHost.hpp"

#include "FileBytes.hpp"
#include "GameIdentity.hpp"
#include "ProjectIdentity.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "Socket.hpp"
#include "runner/GamePack.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <future>
#include <system_error>

namespace server {

namespace fs = std::filesystem;

const char* const kUsage =
    "Usage: AnarchyServer [project folder] [--port N] [--max-connections 1-64] [--max-send BYTES]";

namespace {

// The engine's frames a second: the simulation's step rate, with nothing to draw.
constexpr double kServerHz = 60.0;

// A whole decimal number from min to max, nothing else in text.
bool ParseWhole(const std::string& text, unsigned long min, unsigned long max, unsigned long& out) {
    unsigned long value = 0;
    const char* end = text.data() + text.size();
    const std::from_chars_result read = std::from_chars(text.data(), end, value);
    if (text.empty() || read.ec != std::errc() || read.ptr != end || value < min || value > max) {
        return false;
    }
    out = value;
    return true;
}

// A positive, finite number of bytes, nothing else in text.
bool ParseBytes(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || !(value > 0.0)) {
        return false;
    }
    out = value;
    return true;
}

// The project folder to serve: the one named, or the game packed into pack_file, unpacked.
bool FindProject(const ServerOptions& options, fs::path& root, std::string& error) {
    if (!options.project.empty()) {
        root = options.project;
        return true;
    }
    const std::optional<runner::PackLocation> where =
        options.pack_file.empty() ? std::nullopt : runner::find_game_pack(options.pack_file);
    if (!where) {
        error = std::string("There is no game in this server. Start it with a project folder.\n") + kUsage;
        return false;
    }
    std::error_code failure;
    const fs::path temp = fs::temp_directory_path(failure);
    if (failure || temp.empty()) {
        error = "This system has no temporary folder to unpack the game into.";
        return false;
    }
    fs::path folder;
    if (!runner::unpack_game(options.pack_file, *where, temp / "AnarchyEngine" / "servers", folder, error)) {
        error = "The game could not be unpacked: " + error;
        return false;
    }
    root = folder / runner::kPackedProject;
    return true;
}

}  // namespace

std::optional<ServerOptions> parse_server_args(const std::vector<std::string>& args, std::string& error) {
    ServerOptions options;
    bool have_project = false;
    // Each refusal names what was wrong, then the usage line.
    const auto refuse = [&](const std::string& why) {
        error = why + "\n" + kUsage;
        return std::nullopt;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool flag = arg.size() > 1 && arg[0] == '-';
        if (!flag) {
            if (have_project) {
                return refuse("Only one project folder can be served: " + arg);
            }
            options.project = fs::u8path(arg);
            have_project = true;
            continue;
        }
        if (arg != "--port" && arg != "--max-connections" && arg != "--max-send") {
            return refuse("Unknown option " + arg);
        }
        if (i + 1 >= args.size()) {
            return refuse(arg + " needs a value");
        }
        const std::string& value = args[++i];
        unsigned long whole = 0;
        if (arg == "--port") {
            if (!ParseWhole(value, 0, 65535, whole)) {
                return refuse("--port takes a port from 0 to 65535, not " + value);
            }
            options.port = static_cast<std::uint16_t>(whole);
        } else if (arg == "--max-connections") {
            if (!ParseWhole(value, 1, 64, whole)) {
                return refuse("--max-connections takes a number from 1 to 64, not " + value);
            }
            options.max_connections = static_cast<std::uint8_t>(whole);
        } else if (!ParseBytes(value, options.max_send)) {
            return refuse("--max-send takes bytes a second above 0, not " + value);
        }
    }
    return options;
}

std::unique_ptr<ServerHost> ServerHost::open(const ServerOptions& options, std::string& error) {
    fs::path root;
    if (!FindProject(options, root, error)) {
        return nullptr;
    }
    std::error_code failure;
    if (!fs::is_regular_file(root / "project.json", failure)) {
        error = "There is no project at " + engine_core::utf8_path(root) + " (no project.json).";
        return nullptr;
    }
    std::unique_ptr<ServerHost> host(new ServerHost());
    engine_core::EngineOptions engine_options;
    engine_options.audio_device = false;
    host->engine_ = std::make_unique<engine_core::Engine>(engine_options);
    engine_core::Engine& engine = *host->engine_;
    // Nothing on a server reads diagnostics, so its scripts are not checked.
    engine.analysis().set_enabled(false);
    engine.set_pace_hz(kServerHz);
    engine.set_client_sync(false);
    try {
        host->project_ = std::make_unique<engine_core::Project>(engine_core::Project::load(root, engine.datamodel()));
    } catch (const std::exception& load_failure) {
        error = std::string("The project could not be opened: ") + load_failure.what();
        return nullptr;
    }
    // A pack holds files, not folders, so a project whose resources folder was
    // empty comes out with none. Made again here, it hashes as the empty original.
    if (options.project.empty()) {
        fs::create_directories(host->project_->resources_root(), failure);
    }
    // Only a client of this exact game, version, and resources may join.
    engine_network::SessionConfig config;
    if (!engine_network::project_identity(*host->project_, config.endpoint.identity, error)) {
        return nullptr;
    }
    config.endpoint.max_peers = options.max_connections;
    config.endpoint.bytes_per_second = options.max_send;
    std::string socket_error;
    std::unique_ptr<network::UdpSocket> socket = network::UdpSocket::open(options.port, socket_error);
    if (!socket) {
        error = "Could not open the server's socket: " + socket_error;
        return nullptr;
    }
    host->port_ = socket->port();
    engine.datamodel().set_world_role(engine_core::WorldRole::Server);
    host->session_ = std::make_unique<engine_network::ServerSession>(engine.datamodel(), std::move(socket), config);
    engine.set_network(host->session_.get());
    return host;
}

ServerHost::~ServerHost() {
    stop();
    // No client is left, so the session's own close writes nothing to the world.
    session_.reset();
    project_.reset();
}

void ServerHost::start() {
    std::lock_guard<std::mutex> guard(stop_mutex_);
    if (started_ || stopped_) {
        return;
    }
    started_ = true;
    engine_->start();
    // As the player does: the place as loaded is what plays.
    engine_->on_simulation([](engine_core::DataModel& game) {
        if (!game.simulation_running()) {
            game.capture_place();
            game.start_simulation();
        }
    });
    engine_->resume();
}

void ServerHost::stop() {
    std::lock_guard<std::mutex> guard(stop_mutex_);
    if (stopped_) {
        return;
    }
    stopped_ = true;
    // A host open gave up on has no session, and its engine never started.
    if (!session_) {
        return;
    }
    // Every client hears "The server closed" from the engine thread, between steps,
    // before the thread goes; this waits until it has.
    std::promise<void> closed;
    std::future<void> done = closed.get_future();
    engine_network::ServerSession* session = session_.get();
    engine_->on_simulation([session, &closed](engine_core::DataModel&) {
        session->close(engine_core::Engine::network_now());
        closed.set_value();
    });
    done.wait();
    engine_->stop();
    engine_->set_network(nullptr);
}

void ServerHost::print_output(std::FILE* out, std::FILE* err) {
    const engine_core::ScriptRuntime::OutputBatch batch = engine_->scripts().drain_output();
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        const std::time_t when = std::chrono::system_clock::to_time_t(line.time);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &when);
#else
        localtime_r(&when, &local);
#endif
        std::string text = line.text;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        std::FILE* stream = line.kind == engine_core::ScriptRuntime::OutputKind::Error ? err : out;
        std::fprintf(stream, "[%02d:%02d:%02d] %s\n", local.tm_hour, local.tm_min, local.tm_sec, text.c_str());
    }
    std::fflush(out);
    std::fflush(err);
}

}  // namespace server
