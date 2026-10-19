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
    // Ends the join or the connection; Left says "You left the game".
    void leave() override;
    // --connect: clears the world (clear_for_client) and connects in FromEmpty
    // mode to an address already resolved. False with why when no socket opens
    // or the endpoint refuses. The session stays after its connection ends, so
    // an overlay can still show why.
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
    std::unique_ptr<ClientSession> session_;
    ClientMode session_mode_ = ClientMode::Swap;
    ClientState reported_ = ClientState::Idle;   // the session state on_session_state last acted on
    std::vector<std::unique_ptr<ClientSession>> retired_;   // ended inside their own callbacks
    std::vector<Resolver> resolvers_;

    // Shared with the resolver threads.
    std::mutex mutex_;
    network::GameIdentity identity_;
    std::filesystem::path unhashed_;      // the resources folder still to hash; empty once hashed
    bool closing_ = false;              // the destructor began: answers are dropped
    std::vector<Resolved> answers_;
    std::atomic<bool> answered_{false};   // answers_ holds something; read each frame without the lock
    std::atomic<int> resolvers_running_{0};

    // Written on the simulation thread, read from any.
    mutable std::mutex status_mutex_;
    ClientStatus status_;
};

}  // namespace engine_network
