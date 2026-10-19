#pragma once

// A server or a client session: the transport's Endpoint, wired to the tree.
// The engine pumps one through engine_core::INetworkPump, receive at the start
// of a step and send at its end
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).
//
// Three rules for whoever owns a session:
// - It does not know the Engine pumping it: whoever called Engine::set_network
//   with it calls Engine::set_network(nullptr) before destroying it.
// - It holds its DataModel (the Game) by reference, and its destructor still
//   writes to it (Connections destroyed, play stopped), so the Game outlives it.
// - connect, disconnect, close and the destructors change the world, so they run
//   on the simulation thread under the write lock, as receive and send do when
//   the engine pumps them.

#include "DataModel.hpp"
#include "Endpoint.hpp"
#include "INetworkPump.hpp"
#include "ReplicaApplier.hpp"
#include "Replicator.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_network {

// Bytes a second a session may send each peer: a join's snapshot drains in seconds, and flow
// control still halves it on a bad link. A bare Endpoint keeps its own, smaller default.
constexpr double kDefaultSendBudget = 1048576.0;   // bytes per second per client

struct SessionConfig {
    // role is set by the session; the budget starts at kDefaultSendBudget.
    network::EndpointConfig endpoint = [] {
        network::EndpointConfig config;
        config.bytes_per_second = kDefaultSendBudget;
        return config;
    }();
    ReplicatorOptions replicator;
    // A client is dropped when its queue makes no progress this long: nothing
    // handed to the channel and nothing acked. A big snapshot draining slowly
    // into a busy world is progress, however long it takes.
    double max_stall_seconds = 30.0;
    std::size_t max_queued_bytes = 256u << 20;   // or when its queue holds more than this
};

// The server's side: each client that connects gets a Connection under
// Connections and, at the end of that tick, a snapshot of the world; after
// that, every tick's changes. A client that cannot keep up is dropped.
class ServerSession : public engine_core::INetworkPump {
public:
    ServerSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
    // Closes: every client is told and its Connection destroyed.
    ~ServerSession() override;
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    void receive(double now) override;
    void send(double now) override;
    // Disconnects every client and destroys its Connection.
    void close(double now);
    network::Endpoint& endpoint();
    // Read-only: only send() ends a tick, so nothing else can take a tick's changes from the clients.
    const Replicator& replicator() const;
    // The peer's Connection; 0 when it has none.
    engine_core::InstanceId connection_of(network::PeerId peer) const;
    // Messages clients sent: nothing a client says has meaning yet, so each is counted and dropped.
    std::size_t dropped_messages() const;
    // Clients dropped because their queue stalled past max_stall_seconds or grew past max_queued_bytes.
    std::size_t fell_behind() const;
    // Bytes waiting for room in the peer's reliable window; 0 for a peer it does not have.
    std::size_t queued_bytes(network::PeerId peer) const;

private:
    // One tick's changes go to every joined peer; they share the bytes.
    using Message = std::shared_ptr<const std::vector<std::uint8_t>>;
    struct Peer {
        engine_core::InstanceId connection = 0;
        bool joined = false;   // its snapshot is queued; it gets every tick's changes after it
        std::deque<Message> queue;   // messages the reliable window had no room for yet
        std::size_t queued_bytes = 0;
        double last_progress = 0.0;       // when the queue was last empty, handed a message, or saw an ack
        std::uint64_t acked = 0;          // the channel's acked count then
    };
    void on_connected(network::PeerId peer);
    void on_disconnected(network::PeerId peer);
    void push(Peer& entry, Message message);
    void flush(network::PeerId peer, double now);

    engine_core::DataModel& game_;
    SessionConfig config_;
    Replicator replicator_;
    std::unordered_map<network::PeerId, Peer> peers_;
    std::size_t dropped_messages_ = 0;
    std::size_t fell_behind_ = 0;
    double now_ = 0.0;   // the last time the engine gave
    // Last, so it goes first: its callbacks reach everything above.
    std::unique_ptr<network::Endpoint> endpoint_;
};

enum class ClientState { Idle, Connecting, Joining, Joined, Disconnected };

// A client's state and why its connection ended, copied out together for a
// thread other than the simulation's, such as a window drawing an overlay.
struct ClientStatus {
    ClientState state = ClientState::Idle;
    std::string reason;
};

// The client's side: it connects, starts play while its world is still empty,
// builds the world from the server's snapshot, then applies each tick's
// changes. When the connection ends, play it started stops, the world role it
// set goes back, and reason() says why.
class ClientSession : public engine_core::INetworkPump {
public:
    ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
    // Leaves, as disconnect does, when still connecting or connected.
    ~ClientSession() override;
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Starts connecting; false when the endpoint refuses (already connected, or its
    // socket failed). A refusal while no connection is under way leaves the state
    // Disconnected with the reason "Could not connect to the server".
    bool connect(const network::Address& server, double now);
    // The player leaves; reason "You left the game".
    void disconnect(double now);
    void receive(double now) override;
    void send(double now) override;
    // state() and reason() are for a thread that may write them (see state_);
    // any other reads status().
    ClientState state() const;
    // Why the connection ended, worded for a player; empty until then.
    const std::string& reason() const;
    // Any thread: the state and reason as of the last receive or send.
    ClientStatus status() const;
    network::Endpoint& endpoint();
    // A fresh one for each connect.
    ReplicaApplier& applier();

private:
    void on_connected();
    void on_message(const std::uint8_t* data, std::size_t size);
    void on_disconnected(network::DisconnectReason why, network::DenyReason deny);
    std::string describe(network::DisconnectReason why, network::DenyReason deny) const;
    // Every write of state_ or reason_ goes through these, under status_mutex_.
    void set_state(ClientState state);
    void set_reason(std::string reason);

    engine_core::DataModel& game_;
    SessionConfig config_;
    std::unique_ptr<ReplicaApplier> applier_;
    // Written under status_mutex_, so status() can copy them from any thread, by
    // the simulation thread, or by any thread while the engine is stopped or
    // under its write lock.
    mutable std::mutex status_mutex_;
    ClientState state_ = ClientState::Idle;
    std::string reason_;
    double now_ = 0.0;
    bool started_play_ = false;   // this connection started play, so its end stops it
    bool set_role_ = false;       // this connection made the world a Client, so its end puts previous_role_ back
    engine_core::WorldRole previous_role_ = engine_core::WorldRole::Standalone;
    std::unique_ptr<network::Endpoint> endpoint_;
};

}  // namespace engine_network
