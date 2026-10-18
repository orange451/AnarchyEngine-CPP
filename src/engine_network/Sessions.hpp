#pragma once

// A server or a client session: the transport's Endpoint, wired to the tree.
// The engine pumps one through engine_core::INetworkPump, receive at the start
// of a step and send at its end
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).
// A session does not know the Engine pumping it: whoever called
// Engine::set_network with it calls set_network(nullptr) before destroying it.

#include "DataModel.hpp"
#include "Endpoint.hpp"
#include "INetworkPump.hpp"
#include "ReplicaApplier.hpp"
#include "Replicator.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_network {

struct SessionConfig {
    network::EndpointConfig endpoint;            // role is set by the session
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
    Replicator& replicator();
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

// The client's side: it connects, starts play while its world is still empty,
// builds the world from the server's snapshot, then applies each tick's
// changes. When the connection ends, play it started stops and reason() says why.
class ClientSession : public engine_core::INetworkPump {
public:
    ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
    // Leaves, as disconnect does, when still connecting or connected.
    ~ClientSession() override;
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Starts connecting; false when the endpoint refuses (already connected, or its socket failed).
    bool connect(const network::Address& server, double now);
    // The player leaves; reason "You left the game".
    void disconnect(double now);
    void receive(double now) override;
    void send(double now) override;
    ClientState state() const;
    // Why the connection ended, worded for a player; empty until then.
    const std::string& reason() const;
    network::Endpoint& endpoint();
    // A fresh one for each connect.
    ReplicaApplier& applier();

private:
    void on_connected();
    void on_message(const std::uint8_t* data, std::size_t size);
    void on_disconnected(network::DisconnectReason why, network::DenyReason deny);
    std::string describe(network::DisconnectReason why, network::DenyReason deny) const;

    engine_core::DataModel& game_;
    SessionConfig config_;
    std::unique_ptr<ReplicaApplier> applier_;
    ClientState state_ = ClientState::Idle;
    std::string reason_;
    double now_ = 0.0;
    bool started_play_ = false;   // this connection started play, so its end stops it
    std::unique_ptr<network::Endpoint> endpoint_;
};

}  // namespace engine_network
