#include "ClientJoinHost.hpp"

#include "ClientWorld.hpp"
#include "Connections.hpp"
#include "ProjectIdentity.hpp"

#include <system_error>
#include <utility>

namespace engine_network {

namespace {
constexpr const char* kLeft = "You left the game";
}  // namespace

ClientJoinHost::ClientJoinHost(engine_core::Engine& engine, network::GameIdentity identity)
    : ClientJoinHost(engine, std::move(identity), std::filesystem::path()) {}

ClientJoinHost::ClientJoinHost(engine_core::Engine& engine, network::GameIdentity identity,
                               std::filesystem::path resources)
    : engine_(engine), identity_(std::move(identity)), unhashed_(std::move(resources)) {
    // The resolver's answer and the sessions ended inside their own callbacks
    // are handled here, early in each step, on the thread stepping the world:
    // the engine thread in a program, the test thread for an engine never started.
    job_ = engine_.scheduler().bind(engine_core::Phase::PreAnimation, [this](double) { tick(); });
}

ClientJoinHost::~ClientJoinHost() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    // A lookup still running finishes first; its answer is dropped above.
    reap_resolvers(true);
    if (resolving_ || session_ != nullptr || !retired_.empty()) {
        end_silently();
    }
    engine_.scheduler().unbind(job_);
}

engine_core::Connections* ClientJoinHost::connections() {
    engine_core::DataModel& game = engine_.datamodel();
    return dynamic_cast<engine_core::Connections*>(game.instance(game.connections()));
}

std::string ClientJoinHost::join(const std::string& address) {
    if (resolving_ || (session_ != nullptr && session_->state() != ClientState::Disconnected &&
                       session_->state() != ClientState::Idle)) {
        return "Already joining or joined; call Leave first";
    }
    // A connection that ended and was kept for its reason goes now.
    if (session_ != nullptr) {
        retire_session();
    }
    reap_resolvers(false);
    const std::uint64_t generation = ++generation_;
    auto done = std::make_shared<std::atomic<bool>>(false);
    resolvers_running_.fetch_add(1);
    try {
        std::thread thread([this, generation, address, done] {
            Resolved result;
            result.generation = generation;
            // The first join pays for the resources hash here, off the simulation thread.
            network::GameIdentity identity;
            if (!ready_identity(identity, result.error)) {
                result.identity_failed = true;
            } else {
                result.address = network::resolve(address, result.error);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!closing_) {
                    answers_.push_back(std::move(result));
                    answered_.store(true);
                }
            }
            // After the answer is queued, so whoever sees this reach 0 sees the answer too.
            resolvers_running_.fetch_sub(1);
            done->store(true);
        });
        resolvers_.push_back(Resolver{std::move(thread), std::move(done)});
    } catch (const std::system_error& failure) {
        resolvers_running_.fetch_sub(1);
        return std::string("Could not start joining: ") + failure.what();
    }
    resolving_ = true;
    silent_ = false;
    if (engine_core::Connections* list = connections()) {
        list->set_join_state(engine_core::JoinState::Joining);
    }
    return {};
}

void ClientJoinHost::leave() {
    if (resolving_) {
        // The lookup's answer, whenever it comes, is for an older generation.
        ++generation_;
        resolving_ = false;
        if (engine_core::Connections* list = connections()) {
            list->note_left(kLeft);
        }
        return;
    }
    if (session_ != nullptr) {
        // Reported through on_state, as every end is.
        session_->disconnect(engine_core::Engine::network_now());
    }
}

bool ClientJoinHost::connect_from_empty(const network::Address& address, std::string& error) {
    if (session_ != nullptr) {
        retire_session();
    }
    ++generation_;
    resolving_ = false;
    silent_ = false;
    std::unique_ptr<ClientSession> made = make_session(ClientMode::FromEmpty, error);
    if (made == nullptr) {
        return false;
    }
    // The world is the server's: the project only said which game this is.
    clear_for_client(engine_.datamodel());
    session_ = std::move(made);
    session_mode_ = ClientMode::FromEmpty;
    reported_ = ClientState::Idle;
    engine_.set_network(session_.get());
    if (engine_core::Connections* list = connections()) {
        list->set_join_state(engine_core::JoinState::Joining);
    }
    ClientSession* session = session_.get();
    if (!session->connect(address, engine_core::Engine::network_now())) {
        error = session->reason();
        return false;
    }
    return true;
}

void ClientJoinHost::end_silently() {
    ++generation_;
    resolving_ = false;
    silent_ = true;
    if (session_ != nullptr) {
        session_->disconnect(engine_core::Engine::network_now());
    }
    if (session_ != nullptr || !retired_.empty()) {
        engine_.set_network(nullptr);
    }
    // Not inside any session's callback here, so every one can go now.
    session_.reset();
    retired_.clear();
    silent_ = false;
    set_status(ClientState::Idle, {});
    if (engine_core::Connections* list = connections()) {
        list->set_join_state(engine_core::JoinState::Idle);
    }
}

ClientSession* ClientJoinHost::session() { return session_.get(); }

ClientStatus ClientJoinHost::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void ClientJoinHost::set_status(ClientState state, const std::string& reason) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = state;
    status_.reason = reason;
}

void ClientJoinHost::tick() {
    retired_.clear();
    if (answered_.load()) {
        std::vector<Resolved> answers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            answered_.store(false);
            answers.swap(answers_);
        }
        for (const Resolved& result : answers) {
            if (resolving_ && result.generation == generation_) {
                resolving_ = false;
                on_resolved(result);
            }
        }
    }
    reap_resolvers(false);
}

void ClientJoinHost::on_resolved(const Resolved& result) {
    engine_core::Connections* list = connections();
    if (!result.address) {
        if (list != nullptr) {
            list->note_left(result.identity_failed ? result.error : "Could not find the server: " + result.error);
        }
        return;
    }
    std::string error;
    std::unique_ptr<ClientSession> made = make_session(ClientMode::Swap, error);
    if (made == nullptr) {
        if (list != nullptr) {
            list->note_left(error);
        }
        return;
    }
    session_ = std::move(made);
    session_mode_ = ClientMode::Swap;
    reported_ = ClientState::Idle;
    engine_.set_network(session_.get());
    // A refusal here ends the connection through on_state, which says why.
    ClientSession* session = session_.get();
    session->connect(*result.address, engine_core::Engine::network_now());
}

bool ClientJoinHost::ready_identity(network::GameIdentity& out, std::string& error) {
    network::GameIdentity identity;
    std::filesystem::path folder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        identity = identity_;
        folder = unhashed_;
    }
    if (!folder.empty()) {
        // Outside the lock: a large folder takes a while. Two threads hashing at
        // once agree, so whichever stores it last changes nothing.
        if (!hash_identity(identity, folder, error)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        identity_ = identity;
        unhashed_.clear();
    }
    out = identity;
    return true;
}

std::unique_ptr<ClientSession> ClientJoinHost::make_session(ClientMode mode, std::string& error) {
    network::GameIdentity identity;
    if (!ready_identity(identity, error)) {
        return nullptr;
    }
    std::string why;
    std::unique_ptr<network::UdpSocket> socket = network::UdpSocket::open(0, why);
    if (!socket) {
        error = "Could not open a socket to join the server: " + why;
        return nullptr;
    }
    SessionConfig config;
    config.endpoint.identity = identity;
    config.endpoint.bytes_per_second = kDefaultSendBudget;
    config.client_mode = mode;
    auto session = std::make_unique<ClientSession>(engine_.datamodel(), std::move(socket), config);
    session->on_state = [this](ClientState state, const std::string& reason) { on_session_state(state, reason); };
    return session;
}

void ClientJoinHost::on_session_state(ClientState state, const std::string& reason) {
    // The overlay sees every change, the reason's included.
    set_status(state, reason);
    // A reason that changes alone (it is set just before Disconnected) is not news here.
    if (state == reported_) {
        return;
    }
    reported_ = state;
    engine_core::Connections* list = connections();
    if (state == ClientState::Joined) {
        if (list != nullptr && !silent_) {
            list->note_joined();
        }
        return;
    }
    if (state != ClientState::Disconnected) {
        return;
    }
    if (list != nullptr) {
        if (silent_) {
            list->set_join_state(engine_core::JoinState::Idle);
        } else {
            list->note_left(reason);
        }
    }
    // --connect keeps its session, so the player's overlay can still show why.
    if (session_mode_ == ClientMode::Swap) {
        retire_session();
    }
}

void ClientJoinHost::retire_session() {
    // Inside the session's own callback, perhaps: the engine stops pumping it
    // now, and it is destroyed at the next tick.
    engine_.set_network(nullptr);
    if (session_ != nullptr) {
        retired_.push_back(std::move(session_));
    }
    set_status(ClientState::Idle, {});
}

void ClientJoinHost::reap_resolvers(bool all) {
    for (auto it = resolvers_.begin(); it != resolvers_.end();) {
        if (all || it->done->load()) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = resolvers_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace engine_network
