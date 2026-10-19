#pragma once

// AnarchyServer's work: one project, loaded into a headless engine, served to
// clients over UDP until it is stopped. No window, no renderer, no audio device.

#include "Engine.hpp"
#include "Project.hpp"
#include "Sessions.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace server {

// How AnarchyServer was started.
struct ServerOptions {
    std::filesystem::path project;     // a project folder; empty: the game packed into pack_file
    std::filesystem::path pack_file;   // the file holding a packed game (main: this program)
    std::uint16_t port = 7777;         // 0: any free port (tests)
    std::uint8_t max_connections = 64;
    double max_send = engine_network::kDefaultSendBudget;
};

// The line printed with every refused argument.
extern const char* const kUsage;

// args excludes the program name. Flags: --port N, --max-connections N (1-64),
// --max-send BYTES (> 0); one positional project folder at most. nullopt with
// error, the usage line included, on anything else.
std::optional<ServerOptions> parse_server_args(const std::vector<std::string>& args, std::string& error);

// A loaded project in a headless engine (no renderer, no audio device, no
// client sync, paced at 60 Hz, role Server) with a ServerSession on a UDP socket.
class ServerHost {
public:
    // Loads, checks identity, opens the socket. Null with error when any fails.
    static std::unique_ptr<ServerHost> open(const ServerOptions& options, std::string& error);
    // stop() if still running.
    ~ServerHost();
    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    // Starts play and the engine thread.
    void start();
    // Closes every client ("The server closed"), stops the engine, unhooks the session. Idempotent.
    void stop();
    // The bound port.
    std::uint16_t port() const { return port_; }
    const std::string& project_name() const { return project_->name(); }
    engine_core::Engine& engine() { return *engine_; }
    engine_network::ServerSession& session() { return *session_; }
    // Writes the scripts' pending output as "[HH:MM:SS] text" lines; errors to err.
    void print_output(std::FILE* out, std::FILE* err);

private:
    ServerHost() = default;

    // In this order, so the session goes first and the engine last.
    std::unique_ptr<engine_core::Engine> engine_;
    std::unique_ptr<engine_core::Project> project_;
    std::unique_ptr<engine_network::ServerSession> session_;
    std::uint16_t port_ = 0;
    std::mutex stop_mutex_;   // stop() from main and from the destructor
    bool started_ = false;
    bool stopped_ = false;
};

}  // namespace server
