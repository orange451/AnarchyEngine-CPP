#include "Sessions.hpp"

#include "ConnectionInstance.hpp"

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
    callbacks.on_message = [this](network::PeerId, const std::uint8_t*, std::size_t) { ++dropped_messages_; };
    endpoint_ = std::make_unique<network::Endpoint>(std::move(socket), config_.endpoint, std::move(callbacks));
}

ServerSession::~ServerSession() = default;

network::Endpoint& ServerSession::endpoint() { return *endpoint_; }
Replicator& ServerSession::replicator() { return replicator_; }
std::size_t ServerSession::dropped_messages() const { return dropped_messages_; }
std::size_t ServerSession::fell_behind() const { return fell_behind_; }

InstanceId ServerSession::connection_of(network::PeerId peer) const {
    const auto found = peers_.find(peer);
    return found != peers_.end() ? found->second.connection : 0;
}

void ServerSession::on_connected(network::PeerId peer) {
    // Who is connected, by a UUID the server chose; names and the rest are the game's.
    auto& connection = game_.create<engine_core::ConnectionInstance>();
    connection.set_connection_id(engine_core::make_connection_uuid());
    game_.set_parent(connection.id(), game_.connections());
    Peer& entry = peers_[peer];
    entry = Peer{};
    entry.connection = connection.id();
}

void ServerSession::on_disconnected(network::PeerId peer) {
    const auto found = peers_.find(peer);
    if (found == peers_.end()) {
        return;
    }
    const InstanceId connection = found->second.connection;
    peers_.erase(found);
    if (game_.alive(connection)) {
        game_.destroy_tree(connection);
    }
}

void ServerSession::receive(double now) { endpoint_->receive(now); }

void ServerSession::push(Peer& entry, const std::vector<std::uint8_t>& message) {
    entry.queue.push_back(message);
    entry.queued_bytes += message.size();
}

void ServerSession::flush(network::PeerId peer, double now) {
    Peer& entry = peers_[peer];
    while (!entry.queue.empty() &&
           endpoint_->send_message(peer, entry.queue.front().data(), entry.queue.front().size())) {
        entry.queued_bytes -= entry.queue.front().size();
        entry.queue.pop_front();
    }
    // A client this far behind will not catch up: it starts again by reconnecting.
    if (entry.queue.size() > config_.max_queued_messages || entry.queued_bytes > config_.max_queued_bytes) {
        ++fell_behind_;
        // Outside a packet's read, the endpoint reports the drop at once, which
        // forgets the peer; the second call covers an endpoint that did not.
        endpoint_->disconnect(peer, now);
        on_disconnected(peer);
    }
}

void ServerSession::send(double now) {
    std::vector<std::uint8_t> changes;
    const bool any = replicator_.end_tick(changes);
    std::vector<network::PeerId> fresh;
    for (auto& [peer, entry] : peers_) {
        if (entry.joined) {
            if (any) {
                push(entry, changes);
            }
        } else {
            fresh.push_back(peer);
        }
    }
    // After this tick's changes: a snapshot taken now already holds them, and
    // every later tick's changes queue behind it, so a client gets each change
    // once, after its snapshot.
    for (const network::PeerId peer : fresh) {
        Peer& entry = peers_[peer];
        std::vector<std::vector<std::uint8_t>> snapshot;
        replicator_.snapshot_messages(entry.connection, snapshot);
        for (const auto& part : snapshot) {
            push(entry, part);
        }
        entry.joined = true;
    }
    std::vector<network::PeerId> ids;
    ids.reserve(peers_.size());
    for (const auto& [peer, entry] : peers_) {
        ids.push_back(peer);
    }
    for (const network::PeerId peer : ids) {
        if (peers_.count(peer) != 0) {
            flush(peer, now);
        }
    }
    endpoint_->send(now);
}

void ServerSession::close(double now) {
    std::vector<network::PeerId> ids;
    ids.reserve(peers_.size());
    for (const auto& [peer, entry] : peers_) {
        ids.push_back(peer);
    }
    for (const network::PeerId peer : ids) {
        endpoint_->disconnect(peer, now);
        on_disconnected(peer);
    }
    endpoint_->send(now);
}

}  // namespace engine_network
