#include "Sessions.hpp"

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
}

ClientSession::~ClientSession() { disconnect(now_); }

void ClientSession::on_connected() {
    set_state(ClientState::Joining);
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
        case DisconnectReason::Socket:
            return "Network error";
        default:
            return "Disconnected";
    }
}

bool ClientSession::connect(const network::Address& server, double now) {
    now_ = now;
    if (!endpoint_->connect(server, now)) {
        return false;
    }
    // Each connection builds its world from nothing.
    applier_ = std::make_unique<ReplicaApplier>(game_);
    set_state(ClientState::Connecting);
    set_reason(std::string());
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

void ClientSession::receive(double now) {
    now_ = now;
    endpoint_->receive(now);
}

void ClientSession::send(double now) {
    now_ = now;
    endpoint_->send(now);
}

ClientState ClientSession::state() const { return state_; }
const std::string& ClientSession::reason() const { return reason_; }

ClientStatus ClientSession::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return ClientStatus{state_, reason_};
}

void ClientSession::set_state(ClientState state) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    state_ = state;
}

void ClientSession::set_reason(std::string reason) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    reason_ = std::move(reason);
}

network::Endpoint& ClientSession::endpoint() { return *endpoint_; }
ReplicaApplier& ClientSession::applier() { return *applier_; }

}  // namespace engine_network
