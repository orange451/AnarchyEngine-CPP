#pragma once

#include "Conditioner.hpp"
#include "Connection.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace network {

// A peer's slot. A server has up to max_peers; a client has one, 0.
using PeerId = std::uint8_t;
constexpr PeerId kNoPeer = 0xFF;

enum class Role { Server, Client };
enum class ConnectionState { Disconnected, Connecting, Connected };
enum class DisconnectReason { None, Requested, Timeout, Denied, Socket };
// WrongProtocol is reserved: a packet with another protocol id is dropped and
// counted without a reply, so it is never sent.
enum class DenyReason : std::uint8_t { None, Full, WrongProtocol, WrongGame, WrongVersion, WrongContent };

struct EndpointConfig {
    Role role = Role::Server;
    std::uint8_t max_peers = 64;
    double bytes_per_second = 32000.0;   // 256 kbps
    double send_interval = 1.0 / 60.0;
    GameIdentity identity;
};

// Set once; the engine's tree may be touched from inside them, since the
// pump runs under its lock. A callback may disconnect any peer, including the
// one it was called for: that peer's remaining callbacks for the packet being
// read are swallowed and on_disconnected fires once, after the packet.
struct EndpointCallbacks {
    std::function<void(PeerId)> on_connected;
    std::function<void(PeerId, DisconnectReason, DenyReason)> on_disconnected;
    std::function<void(PeerId, const std::uint8_t*, std::size_t)> on_message;
    std::function<void(PeerId, std::uint16_t sequence, const std::uint8_t*, std::size_t)> on_payload;
    std::function<void(PeerId, std::uint16_t sequence, std::uint32_t tag)> on_delivered;
};

// Packets dropped before they reached a connection.
struct EndpointStats {
    std::uint64_t dropped_bad_protocol = 0;
    std::uint64_t dropped_bad_session = 0;
    std::uint64_t dropped_unknown_peer = 0;
    std::uint64_t dropped_malformed = 0;
};

// A server or a client: owns the socket, makes and ends connections, and
// pumps them. receive() at the start of an engine tick, send() at its end.
class Endpoint {
public:
    Endpoint(std::unique_ptr<ISocket> socket, EndpointConfig config, EndpointCallbacks callbacks);

    ConditionerSettings& conditioner() { return conditioner_.settings; }

    bool connect(const Address& server, double now);
    void disconnect(PeerId peer, double now);
    void receive(double now);
    void send(double now);

    ConnectionState state(PeerId peer) const;
    std::uint16_t session(PeerId peer) const;
    ConnectionStats stats(PeerId peer, double now) const;
    const EndpointStats& stats() const { return stats_; }
    bool send_message(PeerId peer, const std::uint8_t* bytes, std::size_t length);
    // The same, holding message rather than copying it: one buffer can go to many peers.
    bool send_message(PeerId peer, SharedMessage message);
    std::size_t payload_budget(PeerId peer, double now) const;
    std::optional<std::uint16_t> send_payload(PeerId peer, const std::uint8_t* bytes, std::size_t length, std::uint32_t tag);
    std::vector<PeerId> peers() const;
    // The server's version after a WrongVersion deny; empty after any other outcome.
    const std::string& denied_version() const { return denied_version_; }
    bool failed() const { return failed_; }

private:
    struct Peer {
        bool used = false;
        Address address;
        std::uint64_t client_salt = 0;
        std::uint64_t server_salt = 0;
        ConnectionState state = ConnectionState::Disconnected;
        double state_time = 0.0;
        double last_request = -1.0;
        std::unique_ptr<Connection> connection;
        bool pending_drop = false;   // disconnect requested while its packet was being read
        DisconnectReason pending_reason = DisconnectReason::None;
        DenyReason pending_deny = DenyReason::None;
    };

    Peer* find(const Address& address);
    const Peer* connected(PeerId peer) const;
    void receive_as_server(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now);
    void receive_as_client(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now);
    void open_connection(PeerId id, double now);
    void drop_peer(PeerId id, DisconnectReason reason, DenyReason deny);
    void fail_all();
    void send_request(Peer& peer);
    void send_accept(const Peer& peer);
    void send_deny(const Address& to, DenyReason reason);
    void send_disconnect(const Peer& peer);
    // Reads one packet on a peer's connection; a disconnect requested by a callback inside it is carried out after.
    void read_connection(PeerId id, const PacketHeader& header, BitReader& reader, std::size_t length, double now);
    static std::uint16_t session_of(const Peer& peer) {
        return static_cast<std::uint16_t>((peer.client_salt ^ peer.server_salt) & 0xFFFF);
    }

    Conditioner conditioner_;
    EndpointConfig config_;
    EndpointCallbacks callbacks_;
    std::vector<Peer> peers_;
    Address server_address_;
    std::mt19937_64 random_;
    EndpointStats stats_;
    std::string denied_version_;
    bool failed_ = false;
    PeerId reading_ = kNoPeer;   // the peer whose packet is inside read_packet
    std::uint8_t buffer_[kMaxPacketBytes];        // receive only: callbacks may send while a packet is read
    std::uint8_t send_buffer_[kMaxPacketBytes];
};

}  // namespace network
