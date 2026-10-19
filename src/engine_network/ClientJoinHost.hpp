#pragma once

// A client program's IJoinHost: Connections:Join and Leave for scripts, and
// --connect for the command line, on one ClientSession it owns with its UDP
// socket and the game's identity
// (docs/superpowers/specs/2026-10-19-join-from-lua-design.md, "Who makes the connection").
//
// Everything here runs on the simulation thread, or with the engine stopped,
// except the resolver: a name is looked up on a thread of its own, and its
// answer waits in a slot until the host's PreAnimation job takes it, on the
// thread stepping the world. A stale answer (after Leave, a second Join, or the
// host's end) is dropped there, and the world never sees it.
//
// The engine outlives the host, and whoever installed it with set_join_host
// takes it out (set_join_host(nullptr)) before destroying it.

#include "Engine.hpp"
#include "GameIdentity.hpp"
#include "JoinHost.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace engine_core {
class Connections;
}

namespace engine_network {

class ClientJoinHost : public engine_core::IJoinHost {
public:
    // Simulation thread, or the engine stopped: it binds a PreAnimation job.
    ClientJoinHost(engine_core::Engine& engine, network::GameIdentity identity);
    // As above, but the resources hash, which reads every resource file, waits
    // until the first join or connect needs it (project_identity_unhashed): a
    // program that never joins never pays for it, and a join hashes on its
    // resolver thread. A folder that cannot be read fails that join, with why.
    ClientJoinHost(engine_core::Engine& engine, network::GameIdentity identity, std::filesystem::path resources);
    // Joins the resolver threads; ends any connection without Left.
    ~ClientJoinHost() override;
    ClientJoinHost(const ClientJoinHost&) = delete;
    ClientJoinHost& operator=(const ClientJoinHost&) = delete;

    // Swap mode: resolves address off the simulation thread, then connects. ""
    // when started; a reason when a join is already under way or no thread starts.
    std::string join(const std::string& address) override;
    // Ends the join or the connection; Left says "You left the game". A script
    // calls it, so a connection ends at the start of the next step (the session's
    // next receive), never under the calling script: --connect's end stops play,
    // which closes the VM that script runs in.
    void leave() override;
    // --connect: readies the identity (hashing it if need be), opens the socket,
    // clears the world (clear_for_client) and makes a FromEmpty session the engine
    // pumps. The connect itself waits for the host's job on the next step, so its
    // timeout starts only once the engine runs: a player calls this before its
    // window is up. False with why when the identity or the socket fails, and the
    // world is then left alone. A refused connect ends the session with "Could not
    // connect to the server". The session stays after its connection ends, so an
    // overlay can still show why.
    bool connect_from_empty(const network::Address& address, std::string& error);
    // Studio Stop: as leave, but Left does not fire; the session is unhooked
    // from the engine and destroyed now. JoinState goes back to Idle.
    void end_silently();
    // The session, for the player's overlay; null while idle or resolving, and
    // after a script's connection ended. Simulation thread.
    ClientSession* session();
    // The session's state and reason, copied under a mutex for any thread, such
    // as a window's overlay: Idle while there is no session (idle, resolving, or
    // after a script's connection ended and its session went).
    ClientStatus status() const;
    // Resolver threads still looking a name up, finished or not yet reaped. For tests.
    int resolvers_running() const { return resolvers_running_.load(); }
    // How a join turns its address into a server's; network::resolve unless a test
    // sets another. Set before the first join; it runs on a resolver thread.
    using Lookup = std::function<std::optional<network::Address>(const std::string& address, std::string& error)>;
    void set_lookup(Lookup lookup);
    // Runs just before the resources are hashed, on whichever thread hashes them,
    // with the folder: a packed game makes its empty resources folder again there
    // (runner::restore_packed_resources). Set before the first join or connect.
    void set_before_hash(std::function<void(const std::filesystem::path&)> before);
    // At most this many lookups run at once; a Join past it is refused, with
    // kTooManyLookups. Only a Leave and Join again, over and over, gets here.
    static constexpr int kMaxLookups = 4;
    static constexpr const char* kTooManyLookups =
        "Too many joins still looking up their server; try again in a moment";

private:
    struct Resolved {
        std::uint64_t generation = 0;
        std::optional<network::Address> address;
        std::string error;   // the resolver's, or the identity's when identity_failed
        bool identity_failed = false;
    };
    struct Resolver {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    engine_core::Connections* connections();
    // The PreAnimation job: frees ended sessions, then takes the resolver's answer.
    void tick();
    void on_resolved(const Resolved& result);
    void on_session_state(ClientState state, const std::string& reason);
    // The identity, hashing the resources first if nothing has yet. Any thread.
    bool ready_identity(network::GameIdentity& out, std::string& error);
    std::unique_ptr<ClientSession> make_session(ClientMode mode, std::string& error);
    // Takes the session out of the engine and keeps it until the next tick.
    void retire_session();
    void reap_resolvers(bool all);
    // Simulation thread; the copy status() returns.
    void set_status(ClientState state, const std::string& reason);

    engine_core::Engine& engine_;
    engine_core::TaskScheduler::JobId job_ = 0;

    // Simulation thread.
    std::uint64_t generation_ = 0;   // bumped by each join, leave and end; an answer for an older one is stale
    bool resolving_ = false;
    bool silent_ = false;            // the session's end tells no script
    std::optional<network::Address> pending_connect_;   // connect_from_empty's, for the next tick
    Lookup lookup_;
    std::unique_ptr<ClientSession> session_;
    ClientMode session_mode_ = ClientMode::Swap;
    ClientState reported_ = ClientState::Idle;   // the session state on_session_state last acted on
    std::vector<std::unique_ptr<ClientSession>> retired_;   // ended inside their own callbacks
    std::vector<Resolver> resolvers_;

    // Shared with the resolver threads.
    std::mutex mutex_;
    network::GameIdentity identity_;
    std::filesystem::path unhashed_;      // the resources folder still to hash; empty once hashed
    std::function<void(const std::filesystem::path&)> before_hash_;
    bool closing_ = false;              // the destructor began: answers are dropped
    std::vector<Resolved> answers_;
    std::atomic<bool> answered_{false};   // answers_ holds something; read each frame without the lock
    std::atomic<int> resolvers_running_{0};

    // Written on the simulation thread, read from any.
    mutable std::mutex status_mutex_;
    ClientStatus status_;
};

}  // namespace engine_network
