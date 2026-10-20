#include "Sessions.hpp"

#include "ClientWorld.hpp"
#include "RemoteCodec.hpp"

#include "RemoteEvent.hpp"
#include "RemoteValues.hpp"

#include <string>
#include <utility>

namespace engine_network {

ClientSession::ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config)
    : game_(game), config_(std::move(config)), applier_(std::make_unique<ReplicaApplier>(game)) {
    config_.endpoint.role = network::Role::Client;
    network::EndpointCallbacks callbacks;
    callbacks.on_connected = [this](network::PeerId) { on_connected(); };
    callbacks.on_message = [this](network::PeerId, const std::uint8_t* data, std::size_t size) { on_message(data, size); };
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

void ClientSession::on_disconnected(network::DisconnectReason why, network::DenyReason deny) {
    // Nothing fired for this connection goes to the next one, either way.
    outbox_.clear();
    outbox_bytes_ = 0;
    unheard_.clear();
    unheard_bytes_ = 0;
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
    unheard_.clear();
    unheard_bytes_ = 0;
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
    if (!decode_remote(in, remote_netid, *values, to_id, engine_core::kRemoteMaxReliableBytes)) {
        return false;
    }
    // A remote this client no longer holds (a script destroyed it) hears nothing.
    const engine_core::InstanceId remote = remote_netid != 0 ? to_id(remote_netid) : 0;
    if (remote == 0 || dynamic_cast<engine_core::RemoteEvent*>(game_.instance(remote)) == nullptr) {
        return true;
    }
    // Nobody listens yet, or earlier ones for it still wait: it waits behind them.
    bool waiting = !game_.event_listened(remote, "OnClientEvent");
    for (const Unheard& held : unheard_) {
        waiting = waiting || held.remote == remote;
    }
    if (waiting) {
        if (unheard_bytes_ + size <= kMaxUnheardRemoteBytes) {
            unheard_.push_back(Unheard{remote, std::move(values), size});
            unheard_bytes_ += size;
        }
        return true;
    }
    engine_core::EventArgs args(1);
    args[0].kind = engine_core::LuaSlot::Kind::Values;
    args[0].values = std::move(values);
    game_.fire_event(remote, "OnClientEvent", std::move(args));
    return true;
}

void ClientSession::fire_unheard() {
    // In order: a remote that has a listener now has it for every message of its own.
    for (auto held = unheard_.begin(); held != unheard_.end();) {
        const bool alive = dynamic_cast<engine_core::RemoteEvent*>(game_.instance(held->remote)) != nullptr;
        if (alive && !game_.event_listened(held->remote, "OnClientEvent")) {
            ++held;
            continue;
        }
        if (alive) {
            engine_core::EventArgs args(1);
            args[0].kind = engine_core::LuaSlot::Kind::Values;
            args[0].values = std::move(held->values);
            game_.fire_event(held->remote, "OnClientEvent", std::move(args));
        }
        unheard_bytes_ -= held->bytes;
        held = unheard_.erase(held);
    }
}

bool ClientSession::fire_server(engine_core::InstanceId remote, std::shared_ptr<const engine_core::RemoteValues> values,
                                bool, std::string& error) {
    if (values == nullptr) {
        return true;
    }
    // The server knows its own instances by netid; a client's own reads nil there.
    const std::uint32_t remote_netid = applier_->netid_of(remote);
    ByteWriter out;
    encode_remote(remote_netid, *values, [this](engine_core::InstanceId id) { return applier_->netid_of(id); }, out);
    if (out.data().size() > engine_core::kRemoteMaxReliableBytes) {
        error = "message is over " + std::to_string(engine_core::kRemoteMaxReliableBytes);
        return false;
    }
    // A remote of this client's own is one the server has never heard of.
    if (remote_netid == 0 || (state_ != ClientState::Joining && state_ != ClientState::Joined)) {
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
ReplicaApplier& ClientSession::applier() { return *applier_; }

}  // namespace engine_network
