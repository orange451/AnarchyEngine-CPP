#include "Sessions.hpp"

#include "ClientWorld.hpp"
#include "RemoteCodec.hpp"

#include "RemoteEvent.hpp"
#include "RemoteValues.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace engine_network {

ClientSession::ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config)
    : game_(game), config_(std::move(config)), applier_(std::make_unique<ReplicaApplier>(game)) {
    config_.endpoint.role = network::Role::Client;
    network::EndpointCallbacks callbacks;
    callbacks.on_connected = [this](network::PeerId) { on_connected(); };
    callbacks.on_message = [this](network::PeerId, const std::uint8_t* data, std::size_t size) { on_message(data, size); };
    callbacks.on_payload = [this](network::PeerId, std::uint16_t, const std::uint8_t* data, std::size_t size) {
        on_payload(data, size);
    };
    callbacks.on_disconnected = [this](network::PeerId, network::DisconnectReason why, network::DenyReason deny) {
        on_disconnected(why, deny);
    };
    endpoint_ = std::make_unique<network::Endpoint>(std::move(socket), config_.endpoint, std::move(callbacks));
    game_.set_remote_sink(this);
}

// Whoever destroys a session has stopped listening to it, so on_state is not told of this last leave.
ClientSession::~ClientSession() {
    on_state = nullptr;
    if (game_.remote_sink() == this) {
        game_.set_remote_sink(nullptr);
    }
    disconnect(now_);
}

void ClientSession::on_connected() {
    set_state(ClientState::Joining);
    if (config_.client_mode == ClientMode::Swap) {
        // The world plays on untouched while the snapshot arrives; swap_world runs once it is in.
        return;
    }
    // A client plays the server's world: it starts while still empty, and the
    // snapshot builds the place during play.
    if (game_.world_role() != engine_core::WorldRole::Client) {
        previous_role_ = game_.world_role();
        set_role_ = true;
        game_.set_world_role(engine_core::WorldRole::Client);
    }
    if (!game_.simulation_running()) {
        game_.capture_place();
        game_.start_simulation();
        started_play_ = true;
    }
}

void ClientSession::on_message(const std::uint8_t* data, std::size_t size) {
    // Only a connected server's messages are applied; after bad data the
    // endpoint swallows the rest of the packet.
    if (state_ != ClientState::Joining && state_ != ClientState::Joined) {
        return;
    }
    if (size != 0 && data[0] == static_cast<std::uint8_t>(MessageType::Remote)) {
        if (!on_remote(data, size)) {
            if (reason_.empty()) {
                set_reason("Bad data from the server");
            }
            endpoint_->disconnect(0, now_);
        }
        return;
    }
    switch (applier_->receive_message(data, size)) {
        case ApplyResult::SnapshotApplied:
            set_state(ClientState::Joined);
            break;
        case ApplyResult::Bad:
            if (reason_.empty()) {
                set_reason("Bad data from the server");
            }
            // Inside a packet's read, so the endpoint drops the server after it
            // and on_disconnected stops play then.
            endpoint_->disconnect(0, now_);
            break;
        case ApplyResult::Ok:
            break;
    }
}

void ClientSession::on_payload(const std::uint8_t* data, std::size_t size) {
    // Until the snapshot is in, a message could name nothing here.
    if (state_ != ClientState::Joined) {
        return;
    }
    bool bad = false;
    const bool framed = read_remote_payload(data, size, [this, &bad](const std::uint8_t* message, std::size_t length) {
        if (bad || state_ != ClientState::Joined) {
            return;
        }
        bad = length == 0 || message[0] != static_cast<std::uint8_t>(MessageType::Remote) || !on_remote(message, length);
    });
    if (!framed || bad) {
        if (reason_.empty()) {
            set_reason("Bad data from the server");
        }
        endpoint_->disconnect(0, now_);
    }
}

void ClientSession::on_disconnected(network::DisconnectReason why, network::DenyReason deny) {
    // Nothing fired for this connection goes to the next one, either way.
    outbox_.clear();
    outbox_bytes_ = 0;
    unreliable_.clear();
    unreliable_bytes_ = 0;
    clear_unheard();
    if (reason_.empty()) {
        set_reason(describe(why, deny));
    }
    set_state(ClientState::Disconnected);
    // Stopping puts back the empty place the client started play with; play
    // this connection did not start, as after a refused connect, is left alone.
    if (started_play_ && game_.simulation_running()) {
        game_.stop_simulation();
    }
    started_play_ = false;
    // A role this connection set goes back to what it was, after play has stopped in it.
    if (set_role_) {
        game_.set_world_role(previous_role_);
    }
    set_role_ = false;
}

std::string ClientSession::describe(network::DisconnectReason why, network::DenyReason deny) const {
    using network::DenyReason;
    using network::DisconnectReason;
    switch (why) {
        case DisconnectReason::Denied:
            switch (deny) {
                case DenyReason::WrongGame:
                    return "This server runs a different game";
                case DenyReason::WrongVersion:
                    return "This game needs an update (the server runs " + endpoint_->denied_version() + ")";
                case DenyReason::WrongContent:
                    return "This game's files differ from the server's";
                case DenyReason::Full:
                    return "The server is full";
                default:
                    return "The server refused the connection";
            }
        case DisconnectReason::Timeout:
            return state_ == ClientState::Connecting ? "Could not reach the server" : "Lost connection to the server";
        case DisconnectReason::Requested:
            return "The server closed the connection";
        case DisconnectReason::Closed:
            return "The server closed";
        case DisconnectReason::Removed:
            return "Removed by the server";
        case DisconnectReason::FellBehind:
            return "Your connection could not keep up";
        case DisconnectReason::Socket:
            return "Network error";
        default:
            return "Disconnected";
    }
}

bool ClientSession::connect(const network::Address& server, double now) {
    now_ = now;
    if (!endpoint_->connect(server, now)) {
        // A connection under way goes on; otherwise the refusal ends this one
        // before it began, so status() says so rather than Connecting forever.
        if (state_ != ClientState::Connecting && state_ != ClientState::Joining && state_ != ClientState::Joined) {
            set_reason("Could not connect to the server");
            set_state(ClientState::Disconnected);
        }
        return false;
    }
    leave_requested_ = false;
    outbox_.clear();
    outbox_bytes_ = 0;
    unreliable_.clear();
    unreliable_bytes_ = 0;
    clear_unheard();
    // Each connection builds its world from nothing.
    applier_ = std::make_unique<ReplicaApplier>(game_);
    if (config_.client_mode == ClientMode::Swap) {
        applier_->before_snapshot = [this] { swap_world(); };
    }
    // The old reason goes first, so status() never shows Connecting with the last connection's reason.
    set_reason(std::string());
    set_state(ClientState::Connecting);
    return true;
}

void ClientSession::disconnect(double now) {
    now_ = now;
    if (state_ != ClientState::Connecting && state_ != ClientState::Joining && state_ != ClientState::Joined) {
        return;
    }
    // The endpoint reports a leave we asked for as Requested, which reads as the server's doing.
    set_reason("You left the game");
    endpoint_->disconnect(0, now);
}

void ClientSession::request_disconnect() {
    if (state_ == ClientState::Connecting || state_ == ClientState::Joining || state_ == ClientState::Joined) {
        leave_requested_ = true;
    }
}

void ClientSession::receive(double now) {
    now_ = now;
    if (leave_requested_) {
        // Between steps: what the end stops or starts runs under no script.
        leave_requested_ = false;
        disconnect(now);
    }
    // Before what arrives now, so a remote's messages stay in order.
    if (!unheard_.empty()) {
        fire_unheard();
    }
    endpoint_->receive(now);
}

void ClientSession::send(double now) {
    now_ = now;
    // FireServer messages go in firing order; what the window has no room for waits.
    while (!outbox_.empty() && endpoint_->state(0) == network::ConnectionState::Connected &&
           endpoint_->send_message(0, outbox_.front())) {
        outbox_bytes_ -= outbox_.front()->size();
        outbox_.pop_front();
    }
    // Unreliable ones ride this tick's payload, as many as fit; the rest are dropped, never resent.
    if (!unreliable_.empty()) {
        std::size_t packed = 0;
        if (endpoint_->state(0) == network::ConnectionState::Connected) {
            packed = pack_remote_payload(unreliable_, endpoint_->payload_budget(0, now), payload_);
            if (packed != 0 && !endpoint_->send_payload(0, payload_.data(), payload_.size(), 0)) {
                packed = 0;
            }
        }
        dropped_unreliable_ += unreliable_.size() - packed;
        unreliable_.clear();
        unreliable_bytes_ = 0;
    }
    endpoint_->send(now);
}

ClientState ClientSession::state() const { return state_; }
const std::string& ClientSession::reason() const { return reason_; }

ClientStatus ClientSession::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return ClientStatus{state_, reason_};
}

void ClientSession::set_state(ClientState state) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (state_ == state) {
            return;
        }
        state_ = state;
    }
    if (on_state) {
        on_state(state_, reason_);
    }
}

void ClientSession::set_reason(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (reason_ == reason) {
            return;
        }
        reason_ = std::move(reason);
    }
    if (on_state) {
        on_state(state_, reason_);
    }
}

void ClientSession::swap_world() {
    // The whole snapshot is in and unpacked, so the download itself can no longer
    // fail: the menu goes, its Persistent part stays, and the world becomes a
    // Client, which re-checks every script's RunContext. One of the snapshot's ops
    // can still be malformed after this; the world is then left half built, the
    // role goes back, and the reason is "Bad data from the server"
    // (docs/superpowers/networking-parked.md).
    swap_for_client(game_, [this](const std::string& warning) {
        if (on_warning) {
            on_warning(warning);
        }
    });
    if (game_.world_role() != engine_core::WorldRole::Client) {
        previous_role_ = game_.world_role();
        set_role_ = true;
        game_.set_world_role(engine_core::WorldRole::Client);
    }
}

bool ClientSession::on_remote(const std::uint8_t* data, std::size_t size) {
    // The server names its own instances by netid; one this client lacks reads nil.
    const NetToId to_id = [this](std::uint32_t netid) -> engine_core::InstanceId {
        const engine_core::InstanceId id = applier_->local(netid);
        return id != engine_core::DataModel::kNoParent && game_.alive(id) ? id : 0;
    };
    ByteReader in(data, size);
    std::uint32_t remote_netid = 0;
    auto values = std::make_shared<engine_core::RemoteValues>();
    std::size_t values_read = 0;
    if (!decode_remote(in, remote_netid, *values, to_id, engine_core::kRemoteMaxReliableBytes,
                       engine_core::kRemoteMaxValues, &values_read)) {
        return false;
    }
    // A remote this client no longer holds (a script destroyed it) hears nothing.
    const engine_core::InstanceId remote = remote_netid != 0 ? to_id(remote_netid) : 0;
    if (remote == 0 || dynamic_cast<engine_core::RemoteEvent*>(game_.instance(remote)) == nullptr) {
        return true;
    }
    // Nobody listens yet, or earlier ones for it still wait: it waits behind them.
    const auto held = unheard_.find(remote);
    if (held != unheard_.end() || !game_.event_listened(remote, "OnClientEvent")) {
        // What it takes once decoded: a few hundred bytes a value, whatever its wire size.
        const std::size_t bytes = size + values_read * sizeof(engine_core::RemoteValue);
        if ((held != unheard_.end() && held->second.size() >= kMaxUnheardPerRemote) ||
            unheard_bytes_ + bytes > kMaxUnheardRemoteBytes) {
            ++dropped_unheard_;
            return true;
        }
        std::deque<Unheard>& queue = held != unheard_.end() ? held->second : unheard_[remote];
        queue.push_back(Unheard{std::move(values), bytes, now_, ++unheard_order_});
        unheard_bytes_ += bytes;
        ++unheard_count_;
        return true;
    }
    engine_core::EventArgs args(1);
    args[0].kind = engine_core::LuaSlot::Kind::Values;
    args[0].values = std::move(values);
    game_.fire_event(remote, "OnClientEvent", std::move(args));
    return true;
}

void ClientSession::fire_unheard() {
    // Messages to fire now, from every remote that has a listener, merged back
    // into the order they arrived in, since reliable order spans remotes.
    std::vector<std::pair<std::uint64_t, std::pair<engine_core::InstanceId, Unheard*>>> ready;
    std::vector<engine_core::InstanceId> fired;
    for (auto entry = unheard_.begin(); entry != unheard_.end();) {
        const engine_core::InstanceId remote = entry->first;
        std::deque<Unheard>& queue = entry->second;
        const bool alive = dynamic_cast<engine_core::RemoteEvent*>(game_.instance(remote)) != nullptr;
        // Oldest first, so the expired are at the front.
        while (!queue.empty() && (!alive || now_ - queue.front().arrived >= kUnheardSeconds)) {
            if (alive) {
                ++dropped_unheard_;
            }
            unheard_bytes_ -= queue.front().bytes;
            --unheard_count_;
            queue.pop_front();
        }
        if (queue.empty()) {
            entry = unheard_.erase(entry);
            continue;
        }
        if (game_.event_listened(remote, "OnClientEvent")) {
            for (Unheard& message : queue) {
                ready.push_back({message.order, {remote, &message}});
            }
            fired.push_back(remote);
        }
        ++entry;
    }
    if (ready.empty()) {
        return;
    }
    std::sort(ready.begin(), ready.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [order, target] : ready) {
        engine_core::EventArgs args(1);
        args[0].kind = engine_core::LuaSlot::Kind::Values;
        args[0].values = std::move(target.second->values);
        game_.fire_event(target.first, "OnClientEvent", std::move(args));
    }
    for (const engine_core::InstanceId remote : fired) {
        const auto entry = unheard_.find(remote);
        for (const Unheard& message : entry->second) {
            unheard_bytes_ -= message.bytes;
        }
        unheard_count_ -= entry->second.size();
        unheard_.erase(entry);
    }
}

void ClientSession::clear_unheard() {
    unheard_.clear();
    unheard_bytes_ = 0;
    unheard_count_ = 0;
}

bool ClientSession::fire_server(engine_core::InstanceId remote, std::shared_ptr<const engine_core::RemoteValues> values,
                                bool unreliable, std::string& error) {
    if (values == nullptr) {
        return true;
    }
    // The server knows its own instances by netid; a client's own reads nil there.
    const std::uint32_t remote_netid = applier_->netid_of(remote);
    ByteWriter out;
    encode_remote(remote_netid, *values, [this](engine_core::InstanceId id) { return applier_->netid_of(id); }, out);
    const std::size_t cap = unreliable ? kMaxUnreliableRemoteBytes : engine_core::kRemoteMaxReliableBytes;
    if (out.data().size() > cap) {
        error = "message is over " + std::to_string(cap);
        return false;
    }
    // A remote of this client's own is one the server has never heard of.
    if (remote_netid == 0 || (state_ != ClientState::Joining && state_ != ClientState::Joined)) {
        return true;
    }
    if (unreliable) {
        // More than a payload holds never goes this tick.
        if (unreliable_bytes_ + out.data().size() > network::kMaxPayloadBytes) {
            ++dropped_unreliable_;
            return true;
        }
        unreliable_bytes_ += out.data().size();
        unreliable_.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(out.data())));
        return true;
    }
    // A server that stops acking leaves these waiting; a script cannot pile up more than the cap.
    if (outbox_bytes_ + out.data().size() > kMaxServerOutboxBytes) {
        error = "cannot send: too many messages are waiting for the server";
        return false;
    }
    outbox_bytes_ += out.data().size();
    outbox_.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(out.data())));
    return true;
}

bool ClientSession::fire_clients(engine_core::InstanceId, const std::vector<engine_core::InstanceId>&, bool,
                                 std::shared_ptr<const engine_core::RemoteValues>, bool, std::string& error) {
    // RemoteEventBindings refuses the server's methods on a client before they get here.
    error = "is for the server";
    return false;
}

network::Endpoint& ClientSession::endpoint() { return *endpoint_; }
std::size_t ClientSession::dropped_unreliable() const { return dropped_unreliable_; }
std::size_t ClientSession::held_unheard() const { return unheard_count_; }
std::size_t ClientSession::dropped_unheard() const { return dropped_unheard_; }
ReplicaApplier& ClientSession::applier() { return *applier_; }

}  // namespace engine_network
