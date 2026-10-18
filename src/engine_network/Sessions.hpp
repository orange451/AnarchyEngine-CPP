#pragma once

// A server or a client session: the transport's Endpoint, wired to the tree.
// The engine pumps one through engine_core::INetworkPump, receive at the start
// of a step and send at its end
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

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
    std::size_t max_queued_messages = 3600;      // about a minute of ticks waiting on one client
    std::size_t max_queued_bytes = 64u << 20;
};

// The server's side: each client that connects gets a Connection under
// Connections and, at the end of that tick, a snapshot of the world; after
// that, every tick's changes. A client that cannot keep up is dropped.
class ServerSession : public engine_core::INetworkPump {
public:
    ServerSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
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
    // Clients dropped because their queue passed max_queued_messages or max_queued_bytes.
    std::size_t fell_behind() const;

private:
    struct Peer {
        engine_core::InstanceId connection = 0;
        bool joined = false;   // its snapshot is queued; it gets every tick's changes after it
        std::deque<std::vector<std::uint8_t>> queue;   // messages the reliable window had no room for yet
        std::size_t queued_bytes = 0;
    };
    void on_connected(network::PeerId peer);
    void on_disconnected(network::PeerId peer);
    void push(Peer& entry, const std::vector<std::uint8_t>& message);
    void flush(network::PeerId peer, double now);

    engine_core::DataModel& game_;
    SessionConfig config_;
    Replicator replicator_;
    std::unordered_map<network::PeerId, Peer> peers_;
    std::size_t dropped_messages_ = 0;
    std::size_t fell_behind_ = 0;
    // Last, so it goes first: its callbacks reach everything above.
    std::unique_ptr<network::Endpoint> endpoint_;
};

enum class ClientState { Idle, Connecting, Joining, Joined, Disconnected };

// The client's side: it connects, starts play while its world is still empty,
// builds the world from the server's snapshot, then applies each tick's
// changes. When the connection ends, play stops and reason() says why.
class ClientSession : public engine_core::INetworkPump {
public:
    ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
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
    std::unique_ptr<network::Endpoint> endpoint_;
};

}  // namespace engine_network
