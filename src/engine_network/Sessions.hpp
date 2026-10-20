#pragma once

// A server or a client session: the transport's Endpoint, wired to the tree.
// The engine pumps one through engine_core::INetworkPump, receive at the start
// of a step and send at its end
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).
//
// Three rules for whoever owns a session:
// - It does not know the Engine pumping it: whoever called Engine::set_network
//   with it calls Engine::set_network(nullptr) before destroying it.
// - It holds its DataModel (the Game) by reference, and its destructor still
//   writes to it (Connections destroyed, play stopped), so the Game outlives it.
// - connect, disconnect, close and the destructors change the world, so they run
//   on the simulation thread under the write lock, as receive and send do when
//   the engine pumps them.

#include "DataModel.hpp"
#include "Endpoint.hpp"
#include "INetworkPump.hpp"
#include "RemoteSink.hpp"
#include "ReplicaApplier.hpp"
#include "Replicator.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_network {

// A client holds a server's RemoteEvent messages that no script listens to
// yet, up to this many bytes, and fires each once OnClientEvent has a listener:
// a script that arrives in the snapshot connects after the messages behind it.
constexpr std::size_t kMaxUnheardRemoteBytes = 4u << 20;

// A client holds FireServer messages the server has not taken yet up to this
// many bytes; past it, FireServer raises rather than grow without end while
// the server is not acking.
constexpr std::size_t kMaxServerOutboxBytes = 4u << 20;

// Bytes a second a session may send each peer: a join's snapshot drains in seconds, and flow
// control still halves it on a bad link. A bare Endpoint keeps its own, smaller default.
constexpr double kDefaultSendBudget = 1048576.0;   // bytes per second per client

// How a client session meets the world it joins into.
// - FromEmpty (--connect): the world was cleared before connecting. The accept
//   makes it a Client and starts play while it is still empty.
// - Swap (a script's Connections:Join): the world already plays standalone and
//   stays exactly as it is until the whole snapshot is in. Then swap_for_client
//   clears it, keeping what is Persistent, and it becomes a Client. A connection
//   that ends never stops play, and the role goes back only if the swap ran.
enum class ClientMode { FromEmpty, Swap };

struct SessionConfig {
    // role is set by the session; the budget starts at kDefaultSendBudget.
    network::EndpointConfig endpoint = [] {
        network::EndpointConfig config;
        config.bytes_per_second = kDefaultSendBudget;
        return config;
    }();
    ReplicatorOptions replicator;
    // A client is dropped when its queue makes no progress this long: nothing
    // handed to the channel and nothing acked. A big snapshot draining slowly
    // into a busy world is progress, however long it takes.
    double max_stall_seconds = 30.0;
    std::size_t max_queued_bytes = 256u << 20;   // or when its queue holds more than this
    ClientMode client_mode = ClientMode::FromEmpty;   // a client's; FromEmpty is --connect, as in 2c
    // RemoteEvent bytes the server reads from one client in one receive, each
    // message charged whether it is accepted or refused; the rest of that
    // receive's are dropped unread and counted. Protection, not rate limiting.
    std::size_t max_remote_bytes_per_pass = 4u << 20;
    // And the values those messages decode to, counted as kRemoteMaxValues
    // counts them, a refused message's up to where it failed: a decoded value
    // is a few hundred bytes, whatever its size on the wire.
    std::size_t max_remote_values_per_pass = 262144;
};

// The server's side: each client that connects gets a Connection under
// Connections and, at the end of that tick, a snapshot of the world; after
// that, every tick's changes. A client that cannot keep up is dropped. It is
// the world's IRemoteSink: a RemoteEvent fired to clients goes after the tick's
// changes, and one a client sends fires OnServerEvent when it checks out.
class ServerSession : public engine_core::INetworkPump, public engine_core::IRemoteSink {
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
    // Read-only: only send() ends a tick, so nothing else can take a tick's changes from the clients.
    const Replicator& replicator() const;
    // The peer's Connection; 0 when it has none.
    engine_core::InstanceId connection_of(network::PeerId peer) const;
    // Messages clients sent that are not RemoteEvent messages: each is counted and dropped.
    std::size_t dropped_messages() const;
    // RemoteEvent messages clients sent that were refused: bad bytes, a remote
    // clients do not have, a client not yet joined, too many values, or past
    // a per-receive cap (bytes or values).
    std::size_t dropped_remotes() const;
    // Values read from clients' RemoteEvent messages, those refused included:
    // what the per-receive value allowance has been charged, ever.
    std::size_t decoded_remote_values() const;
    // Unreliable messages fired to clients that never went: past a payload's
    // budget in their tick, or to a client whose snapshot was not yet queued.
    std::size_t dropped_unreliable() const;
    bool fire_server(engine_core::InstanceId remote, std::shared_ptr<const engine_core::RemoteValues> values,
                     bool unreliable, std::string& error) override;
    bool fire_clients(engine_core::InstanceId remote, const std::vector<engine_core::InstanceId>& connections,
                      bool all, std::shared_ptr<const engine_core::RemoteValues> values, bool unreliable,
                      std::string& error) override;
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
        std::vector<Message> held;   // RemoteEvent messages fired before its snapshot was queued: they go after it
        std::size_t remote_bytes = 0;   // RemoteEvent bytes read from it in this receive
        std::size_t remote_values = 0;  // and the values they decoded to
        std::vector<Message> unreliable;   // unreliable messages for this tick's payload, in firing order
        std::size_t unreliable_bytes = 0;
    };
    void on_connected(network::PeerId peer);
    void on_disconnected(network::PeerId peer);
    void push(Peer& entry, Message message);
    void flush(network::PeerId peer, double now);
    void on_message(network::PeerId peer, const std::uint8_t* data, std::size_t size);
    void on_payload(network::PeerId peer, const std::uint8_t* data, std::size_t size);
    // A client's RemoteEvent message by either path: fires OnServerEvent, or is dropped and counted.
    void accept_remote(network::PeerId peer, const std::uint8_t* data, std::size_t size);
    // A fired message and whom it goes to, until send() queues it behind the
    // tick's changes, or packs an unreliable one into each peer's payload.
    struct Outgoing {
        std::vector<engine_core::InstanceId> connections;
        bool all = false;
        bool unreliable = false;
        Message message;
    };
    void route(const Outgoing& outgoing);
    void send_unreliable(network::PeerId peer, Peer& entry, double now);

    engine_core::DataModel& game_;
    SessionConfig config_;
    Replicator replicator_;
    std::unordered_map<network::PeerId, Peer> peers_;
    std::size_t dropped_messages_ = 0;
    std::size_t dropped_remotes_ = 0;
    std::size_t decoded_remote_values_ = 0;
    std::vector<Outgoing> outbox_;
    std::size_t unreliable_outbox_bytes_ = 0;   // the unreliable messages in outbox_
    std::size_t dropped_unreliable_ = 0;
    std::vector<std::uint8_t> payload_;   // a peer's packed payload, kept to spare a tick the allocation
    bool remote_bytes_counted_ = false;   // some peer's remote_bytes is not 0
    std::size_t fell_behind_ = 0;
    double now_ = 0.0;   // the last time the engine gave
    // Last, so it goes first: its callbacks reach everything above.
    std::unique_ptr<network::Endpoint> endpoint_;
};

enum class ClientState { Idle, Connecting, Joining, Joined, Disconnected };

// A client's state and why its connection ended, copied out together for a
// thread other than the simulation's, such as a window drawing an overlay.
struct ClientStatus {
    ClientState state = ClientState::Idle;
    std::string reason;
};

// The client's side: it connects, builds the world from the server's snapshot,
// then applies each tick's changes. FromEmpty starts play while the world is
// still empty; Swap swaps the playing world at the snapshot (see ClientMode).
// When the connection ends, play it started stops, the world role it set goes
// back, and reason() says why. It is the world's IRemoteSink: FireServer
// sends at the session's next send, and a server's RemoteEvent message fires
// OnClientEvent.
class ClientSession : public engine_core::INetworkPump, public engine_core::IRemoteSink {
public:
    ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
    // Leaves, as disconnect does, when still connecting or connected.
    ~ClientSession() override;
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Starts connecting; false when the endpoint refuses (already connected, or its
    // socket failed). A refusal while no connection is under way leaves the state
    // Disconnected with the reason "Could not connect to the server".
    bool connect(const network::Address& server, double now);
    // The player leaves; reason "You left the game".
    void disconnect(double now);
    // As disconnect, at the start of the next receive: a script's Leave asks for
    // it, and the end it brings (play stopped, a role put back, Left) runs then,
    // outside any script, never under the one that asked.
    void request_disconnect();
    void receive(double now) override;
    void send(double now) override;
    // state() and reason() are for a thread that may write them (see state_);
    // any other reads status().
    ClientState state() const;
    // Why the connection ended, worded for a player; empty until then.
    const std::string& reason() const;
    // Any thread: the state and reason as of the last receive or send.
    ClientStatus status() const;
    network::Endpoint& endpoint();
    // A fresh one for each connect.
    ReplicaApplier& applier();
    // Called on the simulation thread whenever state() or reason() changes, with both
    // as they now are. It may unhook the session from the engine, but never destroy it.
    std::function<void(ClientState, const std::string& reason)> on_state;
    // Swap mode: told of each Persistent instance the swap had to destroy, and why.
    std::function<void(const std::string&)> on_warning;
    // Unreliable FireServer messages that never went: past the payload's budget in their tick.
    std::size_t dropped_unreliable() const;
    bool fire_server(engine_core::InstanceId remote, std::shared_ptr<const engine_core::RemoteValues> values,
                     bool unreliable, std::string& error) override;
    bool fire_clients(engine_core::InstanceId remote, const std::vector<engine_core::InstanceId>& connections,
                      bool all, std::shared_ptr<const engine_core::RemoteValues> values, bool unreliable,
                      std::string& error) override;

private:
    void on_connected();
    void on_message(const std::uint8_t* data, std::size_t size);
    // A tick's unreliable messages from the server; bad framing is bad data.
    void on_payload(const std::uint8_t* data, std::size_t size);
    // A server's RemoteEvent message: false for bad bytes.
    bool on_remote(const std::uint8_t* data, std::size_t size);
    void on_disconnected(network::DisconnectReason why, network::DenyReason deny);
    std::string describe(network::DisconnectReason why, network::DenyReason deny) const;
    // Every write of state_ or reason_ goes through these, under status_mutex_.
    void set_state(ClientState state);
    void set_reason(std::string reason);

    engine_core::DataModel& game_;
    SessionConfig config_;
    std::unique_ptr<ReplicaApplier> applier_;
    // Written under status_mutex_, so status() can copy them from any thread, by
    // the simulation thread, or by any thread while the engine is stopped or
    // under its write lock.
    mutable std::mutex status_mutex_;
    ClientState state_ = ClientState::Idle;
    std::string reason_;
    double now_ = 0.0;
    bool started_play_ = false;   // this connection started play, so its end stops it
    bool set_role_ = false;       // this connection made the world a Client, so its end puts previous_role_ back
    bool leave_requested_ = false;   // request_disconnect: the next receive leaves
    // Swap mode: the applier's hook, run once the whole snapshot is in.
    void swap_world();
    // FireServer messages the reliable window had no room for yet, oldest first.
    std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> outbox_;
    std::size_t outbox_bytes_ = 0;   // kMaxServerOutboxBytes
    // Unreliable FireServer messages for this tick's payload, oldest first.
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> unreliable_;
    std::size_t unreliable_bytes_ = 0;
    std::size_t dropped_unreliable_ = 0;
    std::vector<std::uint8_t> payload_;   // the packed payload, kept to spare a tick the allocation
    // Server messages for a remote nobody listens to yet, oldest first (kMaxUnheardRemoteBytes).
    struct Unheard {
        engine_core::InstanceId remote = 0;
        std::shared_ptr<const engine_core::RemoteValues> values;
        std::size_t bytes = 0;
    };
    std::deque<Unheard> unheard_;
    std::size_t unheard_bytes_ = 0;
    // Fires each held message whose remote now has a listener; drops those whose remote is gone.
    void fire_unheard();
    engine_core::WorldRole previous_role_ = engine_core::WorldRole::Standalone;
    std::unique_ptr<network::Endpoint> endpoint_;
};

}  // namespace engine_network
