#include "Sessions.hpp"

#include "RemoteCodec.hpp"

#include "ConnectionInstance.hpp"
#include "RemoteEvent.hpp"
#include "RemoteValues.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace engine_network {

using engine_core::InstanceId;

ServerSession::ServerSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config)
    : game_(game), config_(std::move(config)), replicator_(game, config_.replicator) {
    config_.endpoint.role = network::Role::Server;
    network::EndpointCallbacks callbacks;
    callbacks.on_connected = [this](network::PeerId peer) { on_connected(peer); };
    callbacks.on_disconnected = [this](network::PeerId peer, network::DisconnectReason, network::DenyReason) {
        on_disconnected(peer);
    };
    callbacks.on_message = [this](network::PeerId peer, const std::uint8_t* data, std::size_t size) {
        on_message(peer, data, size);
    };
    endpoint_ = std::make_unique<network::Endpoint>(std::move(socket), config_.endpoint, std::move(callbacks));
    game_.set_remote_sink(this);
}

ServerSession::~ServerSession() {
    if (game_.remote_sink() == this) {
        game_.set_remote_sink(nullptr);
    }
    close(now_);
}

network::Endpoint& ServerSession::endpoint() { return *endpoint_; }
const Replicator& ServerSession::replicator() const { return replicator_; }
std::size_t ServerSession::dropped_messages() const { return dropped_messages_; }
std::size_t ServerSession::dropped_remotes() const { return dropped_remotes_; }
std::size_t ServerSession::fell_behind() const { return fell_behind_; }

InstanceId ServerSession::connection_of(network::PeerId peer) const {
    const auto found = peers_.find(peer);
    return found != peers_.end() ? found->second.connection : 0;
}

std::size_t ServerSession::queued_bytes(network::PeerId peer) const {
    const auto found = peers_.find(peer);
    return found != peers_.end() ? found->second.queued_bytes : 0;
}

void ServerSession::on_connected(network::PeerId peer) {
    // Who is connected, by a UUID the server chose; names and the rest are the game's.
    auto& connection = game_.create<engine_core::ConnectionInstance>();
    connection.set_connection_id(engine_core::make_connection_uuid());
    game_.set_parent(connection.id(), game_.connections());
    Peer& entry = peers_[peer];
    entry = Peer{};
    entry.connection = connection.id();
    entry.last_progress = now_;
}

void ServerSession::on_disconnected(network::PeerId peer) {
    const auto found = peers_.find(peer);
    if (found == peers_.end()) {
        return;
    }
    const InstanceId connection = found->second.connection;
    peers_.erase(found);
    // A script may have destroyed it already; a stale id no longer resolves.
    if (game_.alive(connection)) {
        game_.destroy_tree(connection);
    }
}

void ServerSession::receive(double now) {
    now_ = now;
    // Each receive starts every client's remote allowance again.
    if (remote_bytes_counted_) {
        for (auto& [peer, entry] : peers_) {
            entry.remote_bytes = 0;
        }
        remote_bytes_counted_ = false;
    }
    endpoint_->receive(now);
}

void ServerSession::on_message(network::PeerId peer, const std::uint8_t* data, std::size_t size) {
    if (size == 0 || data[0] != static_cast<std::uint8_t>(MessageType::Remote)) {
        ++dropped_messages_;
        return;
    }
    // A RemoteEvent message. Anything wrong with it drops it and counts it; the
    // client is never kicked for what it sent.
    const auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.joined || !game_.alive(found->second.connection) ||
        found->second.remote_bytes + size > config_.max_remote_bytes_per_pass) {
        ++dropped_remotes_;
        return;
    }
    Peer& entry = found->second;
    // A client names only what it could have been sent: instances clients see.
    const NetToId to_id = [this](std::uint32_t netid) -> InstanceId {
        return game_.alive(netid) && replicator_.replicated(netid) ? netid : 0;
    };
    ByteReader in(data, size);
    std::uint32_t remote_netid = 0;
    auto values = std::make_shared<engine_core::RemoteValues>();
    if (!decode_remote(in, remote_netid, *values, to_id, engine_core::kRemoteMaxReliableBytes) ||
        to_id(remote_netid) == 0 || remote_netid == 0 ||
        dynamic_cast<engine_core::RemoteEvent*>(game_.instance(remote_netid)) == nullptr) {
        ++dropped_remotes_;
        return;
    }
    entry.remote_bytes += size;
    remote_bytes_counted_ = true;
    engine_core::EventArgs args(2);
    args[0].kind = engine_core::LuaSlot::Kind::Instance;
    args[0].id = entry.connection;
    args[1].kind = engine_core::LuaSlot::Kind::Values;
    args[1].values = std::move(values);
    game_.fire_event(remote_netid, "OnServerEvent", std::move(args));
}

bool ServerSession::fire_server(InstanceId, std::shared_ptr<const engine_core::RemoteValues>, bool, std::string& error) {
    // RemoteEventBindings refuses FireServer on a server before it gets here.
    error = "is for clients";
    return false;
}

bool ServerSession::fire_clients(InstanceId remote, const std::vector<InstanceId>& connections, bool all,
                                 std::shared_ptr<const engine_core::RemoteValues> values, bool, std::string& error) {
    if (values == nullptr) {
        return true;
    }
    // A server's netid is its InstanceId; what clients do not hold reads nil there.
    std::vector<std::uint8_t> bytes;
    {
        ByteWriter out;
        encode_remote(remote, *values, [this](InstanceId id) -> std::uint32_t {
            return replicator_.replicated(id) ? id : 0;
        }, out);
        bytes = std::move(out.data());
    }
    if (bytes.size() > engine_core::kRemoteMaxReliableBytes) {
        error = "message is over " + std::to_string(engine_core::kRemoteMaxReliableBytes);
        return false;
    }
    // A remote clients do not hold (one in ServerStorage) reaches no one.
    if ((!all && connections.empty()) || !replicator_.replicated(remote)) {
        return true;
    }
    outbox_.push_back(Outgoing{connections, all, std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes))});
    return true;
}

void ServerSession::route(const Outgoing& outgoing) {
    for (auto& [peer, entry] : peers_) {
        if (!outgoing.all && std::find(outgoing.connections.begin(), outgoing.connections.end(), entry.connection) ==
                                 outgoing.connections.end()) {
            continue;
        }
        // Through the same queue as the tree, behind this tick's changes; a client
        // whose snapshot is not queued yet gets it after the snapshot.
        if (entry.joined) {
            push(entry, outgoing.message);
        } else {
            entry.held.push_back(outgoing.message);
        }
    }
}

void ServerSession::push(Peer& entry, Message message) {
    entry.queued_bytes += message->size();
    entry.queue.push_back(std::move(message));
}

void ServerSession::flush(network::PeerId peer, double now) {
    Peer& entry = peers_[peer];
    bool progressed = entry.queue.empty();
    while (!entry.queue.empty() &&
           endpoint_->send_message(peer, entry.queue.front())) {
        entry.queued_bytes -= entry.queue.front()->size();
        entry.queue.pop_front();
        progressed = true;
    }
    // A long block in flight hands nothing over while it drains; its acks are progress too.
    const std::uint64_t acked = endpoint_->stats(peer, now).reliable_acked;
    if (acked != entry.acked) {
        entry.acked = acked;
        progressed = true;
    }
    if (progressed || entry.queue.empty()) {
        entry.last_progress = now;
    }
    // A client this stuck, or this far behind, will not catch up: it is told so
    // ("Your connection could not keep up") and starts again by reconnecting.
    if (now - entry.last_progress > config_.max_stall_seconds || entry.queued_bytes > config_.max_queued_bytes) {
        ++fell_behind_;
        // Outside a packet's read, the endpoint reports the drop at once, which
        // forgets the peer; the second call covers an endpoint that did not.
        endpoint_->disconnect(peer, now, network::LeaveCode::FellBehind);
        on_disconnected(peer);
    }
}

void ServerSession::send(double now) {
    now_ = now;
    // A script that destroyed a client's Connection removed that client: it is
    // told so, and its changes stop here. Outside a packet's read, the endpoint
    // reports the drop at once; the second call covers an endpoint that did not.
    std::vector<network::PeerId> removed;
    for (const auto& [peer, entry] : peers_) {
        if (!game_.alive(entry.connection)) {
            removed.push_back(peer);
        }
    }
    for (const network::PeerId peer : removed) {
        endpoint_->disconnect(peer, now, network::LeaveCode::Removed);
        on_disconnected(peer);
    }
    std::vector<std::uint8_t> bytes;
    Message changes;
    if (replicator_.end_tick(bytes)) {
        changes = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    }
    std::vector<network::PeerId> fresh;
    for (auto& [peer, entry] : peers_) {
        if (entry.joined) {
            if (changes) {
                push(entry, changes);
            }
        } else {
            fresh.push_back(peer);
        }
    }
    // RemoteEvent messages fired this tick go behind its changes, so an instance
    // created in the same tick is on the client before a message names it.
    if (!outbox_.empty()) {
        for (const Outgoing& outgoing : outbox_) {
            route(outgoing);
        }
        outbox_.clear();
    }
    // After this tick's changes: a snapshot taken now already holds them, and
    // every later tick's changes queue behind it, so a client gets each change
    // once, after its snapshot.
    for (const network::PeerId peer : fresh) {
        Peer& entry = peers_[peer];
        std::vector<std::vector<std::uint8_t>> snapshot;
        replicator_.snapshot_messages(entry.connection, snapshot);
        for (auto& part : snapshot) {
            push(entry, std::make_shared<const std::vector<std::uint8_t>>(std::move(part)));
        }
        // Messages fired for it before now: the snapshot holds what they name.
        for (Message& message : entry.held) {
            push(entry, std::move(message));
        }
        entry.held.clear();
        entry.joined = true;
    }
    // Through the endpoint's slots, which a kick in flush leaves in place, so
    // a steady tick allocates nothing here.
    endpoint_->for_each_peer([this, now](network::PeerId peer) {
        if (peers_.count(peer) != 0) {
            flush(peer, now);
        }
    });
    endpoint_->send(now);
}

void ServerSession::close(double now) {
    // Each client hears the server closed; the endpoint reports each drop at
    // once, which destroys its Connection, and the second pass covers any it did not.
    endpoint_->close(now);
    std::vector<network::PeerId> ids;
    ids.reserve(peers_.size());
    for (const auto& [peer, entry] : peers_) {
        ids.push_back(peer);
    }
    for (const network::PeerId peer : ids) {
        on_disconnected(peer);
    }
    endpoint_->send(now);
}

}  // namespace engine_network
