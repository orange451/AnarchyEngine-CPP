#include "Endpoint.hpp"

#include <algorithm>
#include <chrono>

namespace network {

namespace {

constexpr double kRequestInterval = 0.1;
constexpr double kTimeout = 5.0;
constexpr int kDisconnectSends = 10;

void write_u64(BitWriter& writer, std::uint64_t value) {
    writer.bits(static_cast<std::uint32_t>(value & 0xFFFFFFFFu), 32);
    writer.bits(static_cast<std::uint32_t>(value >> 32), 32);
}

std::uint64_t read_u64(BitReader& reader) {
    const std::uint64_t low = reader.bits(32);
    const std::uint64_t high = reader.bits(32);
    return low | (high << 32);
}

}  // namespace

Endpoint::Endpoint(std::unique_ptr<ISocket> socket, EndpointConfig config, EndpointCallbacks callbacks)
    : conditioner_(std::move(socket)), config_(std::move(config)), callbacks_(std::move(callbacks)),
      peers_(config_.role == Role::Server ? config_.max_peers : 1),
      random_(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
              reinterpret_cast<std::uintptr_t>(this)) {}

Endpoint::Peer* Endpoint::find(const Address& address) {
    for (Peer& peer : peers_) {
        if (peer.used && peer.address == address) return &peer;
    }
    return nullptr;
}

const Endpoint::Peer* Endpoint::connected(PeerId id) const {
    if (id >= peers_.size()) return nullptr;
    const Peer& peer = peers_[id];
    return peer.used && peer.state == ConnectionState::Connected ? &peer : nullptr;
}

bool Endpoint::connect(const Address& server, double now) {
    if (config_.role != Role::Client || failed_ || peers_[0].used) return false;
    Peer& peer = peers_[0];
    peer.used = true;
    peer.address = server;
    peer.client_salt = random_();
    peer.server_salt = 0;
    peer.state = ConnectionState::Connecting;
    peer.state_time = now;
    peer.last_request = -1.0;
    peer.connection.reset();
    server_address_ = server;
    denied_version_.clear();
    return true;
}

void Endpoint::disconnect(PeerId id, double now) {
    (void)now;
    if (id >= peers_.size() || !peers_[id].used) return;
    if (peers_[id].state == ConnectionState::Connected) send_disconnect(peers_[id]);
    drop_peer(id, DisconnectReason::Requested, DenyReason::None);
}

void Endpoint::receive(double now) {
    if (failed_) return;
    conditioner_.advance(now);
    Address from;
    for (;;) {
        std::size_t length = conditioner_.receive(from, buffer_, kMaxPacketBytes);
        if (length == 0) break;
        length = decode_packet(buffer_, length);
        if (length == 0) continue;
        BitReader reader(buffer_, length);
        PacketHeader header;
        if (!read_header(reader, header)) {
            ++stats_.dropped_bad_protocol;
            continue;
        }
        if (config_.role == Role::Server) {
            receive_as_server(from, header, reader, length, now);
        } else {
            receive_as_client(from, header, reader, length, now);
        }
    }
    if (conditioner_.failed()) {
        fail_all();
        return;
    }
    // Silence. A connected peer's last sign of life is its last packet, or
    // the accept that made it, whichever is later.
    for (PeerId id = 0; id < peers_.size(); ++id) {
        Peer& peer = peers_[id];
        if (!peer.used) continue;
        double last = peer.state_time;
        if (peer.state == ConnectionState::Connected && peer.connection->last_receive() > last) last = peer.connection->last_receive();
        if (now - last > kTimeout) drop_peer(id, DisconnectReason::Timeout, DenyReason::None);
    }
}

void Endpoint::receive_as_server(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now) {
    Peer* peer = find(from);
    switch (header.type) {
        case PacketType::ConnectRequest: {
            const std::uint64_t salt = read_u64(reader);
            GameIdentity identity;
            if (!read_identity(reader, identity)) {
                ++stats_.dropped_malformed;
                return;
            }
            if (peer != nullptr) {
                // The same client did not get our accept: answer again. A
                // different salt from a live address is someone else, or a
                // restart; it waits until this peer is gone.
                if (peer->client_salt == salt) send_accept(*peer);
                else ++stats_.dropped_unknown_peer;
                return;
            }
            DenyReason deny = DenyReason::None;
            if (identity.game_id != config_.identity.game_id) deny = DenyReason::WrongGame;
            else if (identity.version != config_.identity.version) deny = DenyReason::WrongVersion;
            else if (identity.resources_hash != config_.identity.resources_hash) deny = DenyReason::WrongContent;
            PeerId slot = kNoPeer;
            if (deny == DenyReason::None) {
                for (PeerId id = 0; id < peers_.size(); ++id) {
                    if (!peers_[id].used) {
                        slot = id;
                        break;
                    }
                }
                if (slot == kNoPeer) deny = DenyReason::Full;
            }
            if (deny != DenyReason::None) {
                send_deny(from, deny);
                return;
            }
            Peer& made = peers_[slot];
            made.used = true;
            made.address = from;
            made.client_salt = salt;
            made.server_salt = random_();
            made.last_request = -1.0;
            open_connection(slot, now);
            send_accept(made);
            if (callbacks_.on_connected) callbacks_.on_connected(slot);
            return;
        }
        case PacketType::ConnectAccept:
        case PacketType::ConnectDeny:
            return;
        default:
            break;
    }
    if (peer == nullptr || peer->state != ConnectionState::Connected) {
        ++stats_.dropped_unknown_peer;
        return;
    }
    if (header.session != peer->connection->session()) {
        ++stats_.dropped_bad_session;
        return;
    }
    const PeerId id = static_cast<PeerId>(peer - peers_.data());
    if (header.type == PacketType::Disconnect) {
        drop_peer(id, DisconnectReason::Requested, DenyReason::None);
        return;
    }
    peer->connection->read_packet(header, reader, buffer_, length, now);
}

void Endpoint::receive_as_client(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now) {
    Peer& peer = peers_[0];
    if (!peer.used || from != server_address_) {
        ++stats_.dropped_unknown_peer;
        return;
    }
    switch (header.type) {
        case PacketType::ConnectAccept: {
            if (peer.state != ConnectionState::Connecting) return;
            const std::uint64_t client_salt = read_u64(reader);
            const std::uint64_t server_salt = read_u64(reader);
            if (reader.failed() || client_salt != peer.client_salt) {
                ++stats_.dropped_malformed;
                return;
            }
            peer.server_salt = server_salt;
            open_connection(0, now);
            if (callbacks_.on_connected) callbacks_.on_connected(0);
            return;
        }
        case PacketType::ConnectDeny: {
            if (peer.state != ConnectionState::Connecting) return;
            const std::uint32_t reason = reader.bits(8);
            const std::uint32_t version_length = reader.bits(8);
            if (reader.failed() || reason > static_cast<std::uint32_t>(DenyReason::WrongContent) || reader.bytes_left() < version_length) {
                ++stats_.dropped_malformed;
                return;
            }
            denied_version_.resize(version_length);
            reader.bytes(reinterpret_cast<std::uint8_t*>(denied_version_.data()), version_length);
            drop_peer(0, DisconnectReason::Denied, static_cast<DenyReason>(reason));
            return;
        }
        case PacketType::ConnectRequest:
            return;
        default:
            break;
    }
    if (peer.state != ConnectionState::Connected) {
        ++stats_.dropped_unknown_peer;
        return;
    }
    if (header.session != peer.connection->session()) {
        ++stats_.dropped_bad_session;
        return;
    }
    if (header.type == PacketType::Disconnect) {
        drop_peer(0, DisconnectReason::Requested, DenyReason::None);
        return;
    }
    peer.connection->read_packet(header, reader, buffer_, length, now);
}

void Endpoint::open_connection(PeerId id, double now) {
    Peer& peer = peers_[id];
    peer.state = ConnectionState::Connected;
    peer.state_time = now;
    peer.connection = std::make_unique<Connection>(config_.bytes_per_second, config_.send_interval);
    peer.connection->set_session(session_of(peer));
    Connection& connection = *peer.connection;
    connection.callbacks.on_message = [this, id](const std::uint8_t* bytes, std::size_t length) {
        if (callbacks_.on_message) callbacks_.on_message(id, bytes, length);
    };
    connection.callbacks.on_payload = [this, id](std::uint16_t sequence, const std::uint8_t* bytes, std::size_t length) {
        if (callbacks_.on_payload) callbacks_.on_payload(id, sequence, bytes, length);
    };
    connection.callbacks.on_delivered = [this, id](std::uint16_t sequence, std::uint32_t tag) {
        if (callbacks_.on_delivered) callbacks_.on_delivered(id, sequence, tag);
    };
}

void Endpoint::drop_peer(PeerId id, DisconnectReason reason, DenyReason deny) {
    Peer& peer = peers_[id];
    if (!peer.used) return;
    peer.used = false;
    peer.state = ConnectionState::Disconnected;
    peer.connection.reset();
    if (callbacks_.on_disconnected) callbacks_.on_disconnected(id, reason, deny);
}

void Endpoint::fail_all() {
    failed_ = true;
    for (PeerId id = 0; id < peers_.size(); ++id) drop_peer(id, DisconnectReason::Socket, DenyReason::None);
}

void Endpoint::send(double now) {
    if (failed_) return;
    conditioner_.advance(now);
    for (PeerId id = 0; id < peers_.size(); ++id) {
        Peer& peer = peers_[id];
        if (!peer.used) continue;
        if (peer.state == ConnectionState::Connecting) {
            if (config_.role == Role::Client && (peer.last_request < 0.0 || now - peer.last_request >= kRequestInterval)) {
                send_request(peer);
                peer.last_request = now;
            }
            continue;
        }
        if (peer.state == ConnectionState::Connected && peer.connection->wants_packet(now)) {
            const std::size_t length = peer.connection->write_packet(buffer_, kMaxPacketBytes, now);
            conditioner_.send(peer.address, buffer_, length);
        }
    }
    if (conditioner_.failed()) fail_all();
}

void Endpoint::send_request(Peer& peer) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::ConnectRequest;
    write_header(writer, header);
    write_u64(writer, peer.client_salt);
    write_identity(writer, config_.identity);
    conditioner_.send(peer.address, buffer_, encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes));
}

void Endpoint::send_accept(const Peer& peer) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::ConnectAccept;
    write_header(writer, header);
    write_u64(writer, peer.client_salt);
    write_u64(writer, peer.server_salt);
    conditioner_.send(peer.address, buffer_, encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes));
}

void Endpoint::send_deny(const Address& to, DenyReason reason) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::ConnectDeny;
    write_header(writer, header);
    writer.bits(static_cast<std::uint32_t>(reason), 8);
    const std::size_t length = std::min<std::size_t>(config_.identity.version.size(), 255);
    writer.bits(static_cast<std::uint32_t>(length), 8);
    writer.bytes(reinterpret_cast<const std::uint8_t*>(config_.identity.version.data()), length);
    conditioner_.send(to, buffer_, encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes));
}

void Endpoint::send_disconnect(const Peer& peer) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::Disconnect;
    header.session = peer.connection->session();
    write_header(writer, header);
    const std::size_t length = encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes);
    for (int i = 0; i < kDisconnectSends; ++i) conditioner_.send(peer.address, buffer_, length);
}

ConnectionState Endpoint::state(PeerId id) const {
    if (id >= peers_.size() || !peers_[id].used) return ConnectionState::Disconnected;
    return peers_[id].state;
}

std::uint16_t Endpoint::session(PeerId id) const {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->session() : 0;
}

ConnectionStats Endpoint::stats(PeerId id, double now) const {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->stats(now) : ConnectionStats{};
}

bool Endpoint::send_message(PeerId id, const std::uint8_t* bytes, std::size_t length) {
    const Peer* peer = connected(id);
    return peer != nullptr && peer->connection->send_message(bytes, length);
}

std::size_t Endpoint::payload_budget(PeerId id, double now) const {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->payload_budget(now) : 0;
}

std::optional<std::uint16_t> Endpoint::send_payload(PeerId id, const std::uint8_t* bytes, std::size_t length, std::uint32_t tag) {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->set_payload(bytes, length, tag) : std::nullopt;
}

std::vector<PeerId> Endpoint::peers() const {
    std::vector<PeerId> ids;
    for (PeerId id = 0; id < peers_.size(); ++id) {
        if (connected(id) != nullptr) ids.push_back(id);
    }
    return ids;
}

}  // namespace network
