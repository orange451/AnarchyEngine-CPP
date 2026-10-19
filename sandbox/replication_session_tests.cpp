// Sessions: a server and its clients over the transport, with Connections,
// join snapshots, and every way a connection ends
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "replication_support.hpp"

#include "ClientWorld.hpp"
#include "Connections.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "Lighting.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using engine_core::InstanceId;

namespace {

network::GameIdentity identity(std::uint64_t hash = 7) {
    network::GameIdentity id;
    network::parse_game_id("0123456789abcdef0123456789abcdef", id.game_id);
    id.resources_hash = hash;
    return id;
}

const network::Address kServer = network::Address::loopback(1);
network::Address client_address(int n) { return network::Address::loopback(static_cast<std::uint16_t>(100 + n)); }

struct Client {
    ScriptRig rig;
    std::unique_ptr<engine_network::ClientSession> session;
};

// A server world and its clients, each world stepped by hand as an engine steps it.
struct Net {
    network::LinkHub hub;
    ScriptRig server;
    std::unique_ptr<engine_network::ServerSession> session;
    std::deque<std::unique_ptr<Client>> clients;
    double now = 0.0;

    explicit Net(engine_network::SessionConfig config = {}) {
        config.endpoint.identity = identity();
        replication::start_server(server);
        session = std::make_unique<engine_network::ServerSession>(server.game, hub.open(kServer), config);
    }
    Client& add_client(network::GameIdentity id = identity()) {
        clients.push_back(std::make_unique<Client>());
        Client& client = *clients.back();
        engine_network::SessionConfig config;
        config.endpoint.identity = id;
        client.session = std::make_unique<engine_network::ClientSession>(
            client.rig.game, hub.open(client_address(static_cast<int>(clients.size()) - 1)), config);
        REQUIRE(client.session->connect(kServer, now));
        return client;
    }
    // One tick: receive, scripts, send, for every world.
    void tick() {
        now += 1.0 / 60.0;
        session->receive(now);
        for (auto& c : clients) if (c->session) c->session->receive(now);
        server.frames(1);
        for (auto& c : clients) c->rig.frames(1);
        session->send(now);
        for (auto& c : clients) if (c->session) c->session->send(now);
    }
    void run(double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) tick();
    }
    void same(Client& client) {
        replication::require_same_world(session->replicator(), server.game, client.rig.game);
    }
};

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

// The one peer in after that is not in before.
network::PeerId only_new_peer(const std::vector<network::PeerId>& before, const std::vector<network::PeerId>& after) {
    std::vector<network::PeerId> fresh;
    for (const network::PeerId peer : after) {
        if (std::find(before.begin(), before.end(), peer) == before.end()) fresh.push_back(peer);
    }
    REQUIRE(fresh.size() == 1);
    return fresh[0];
}

// The Connection Id a client holds as its LocalConnection; empty when it has none.
std::string local_connection_id(Client& client) {
    engine_core::DataModel& game = client.rig.game;
    const auto* connections = static_cast<engine_core::Connections*>(game.instance(game.connections()));
    const InstanceId local = connections->local_connection();
    if (local == 0 || !game.alive(local)) return std::string();
    return static_cast<engine_core::ConnectionInstance*>(game.instance(local))->connection_id();
}

std::string server_connection_id(Net& net, network::PeerId peer) {
    const InstanceId id = net.session->connection_of(peer);
    REQUIRE(id != 0);
    return static_cast<engine_core::ConnectionInstance*>(net.server.game.instance(id))->connection_id();
}

// A raw server endpoint sends messages to a ClientSession once it connects,
// then the two are pumped until the client gives up or a second passes.
void serve_raw(engine_network::ClientSession& client, network::LinkHub& hub,
               const std::vector<std::vector<std::uint8_t>>& messages) {
    network::EndpointConfig server_config;
    server_config.identity = identity();
    network::EndpointCallbacks callbacks;
    std::vector<network::PeerId> connected;
    callbacks.on_connected = [&](network::PeerId peer) { connected.push_back(peer); };
    network::Endpoint server(hub.open(kServer), server_config, callbacks);
    REQUIRE(client.connect(kServer, 0.0));
    double now = 0.0;
    bool sent = false;
    for (int i = 0; i < 60 && client.state() != engine_network::ClientState::Disconnected; ++i) {
        now += 1.0 / 60.0;
        server.receive(now);
        client.receive(now);
        if (!connected.empty() && !sent) {
            for (const auto& message : messages) {
                REQUIRE(server.send_message(connected[0], message.data(), message.size()));
            }
            sent = true;
        }
        server.send(now);
        client.send(now);
    }
}

}  // namespace

TEST_CASE("RP40 a client joins: it gets a Connection, the world, and its LocalConnection", "[replication][session]") {
    Net net;
    add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Level");
    Client& client = net.add_client();
    net.run(0.5);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(client.rig.game.world_role() == engine_core::WorldRole::Client);
    REQUIRE(client.rig.game.simulation_running());
    net.same(client);
    const InstanceId server_connection = net.session->connection_of(0);
    REQUIRE(server_connection != 0);
    auto* local = static_cast<engine_core::Connections*>(client.rig.game.instance(client.rig.game.connections()));
    REQUIRE(local->local_connection() != 0);
    REQUIRE(static_cast<engine_core::ConnectionInstance*>(client.rig.game.instance(local->local_connection()))->connection_id() ==
            static_cast<engine_core::ConnectionInstance*>(net.server.game.instance(server_connection))->connection_id());
    auto* on_server = static_cast<engine_core::Connections*>(net.server.game.instance(net.server.game.connections()));
    REQUIRE(on_server->local_connection() == 0);
}

TEST_CASE("RP41 changes made while a client joins reach it once, after its snapshot", "[replication][session]") {
    engine_network::SessionConfig config;
    config.replicator.snapshot_part_bytes = 2048;   // a snapshot several ticks long
    Net net(config);
    const InstanceId workspace = net.server.game.scene_service("Workspace");
    for (int i = 0; i < 400; ++i) {
        add_folder(net.server.game, workspace, ("F" + std::to_string(i)).c_str());
    }
    Client& client = net.add_client();
    engine_core::Folder& busy = add_folder(net.server.game, workspace, "Busy");
    bool saw_joining = false;
    for (int i = 0; i < 240; ++i) {
        net.server.game.set_name(busy.id(), "Busy" + std::to_string(i));
        if (i % 20 == 0) add_folder(net.server.game, busy.id(), ("Kid" + std::to_string(i)).c_str());
        net.tick();
        saw_joining = saw_joining || client.session->state() == engine_network::ClientState::Joining;
    }
    net.run(1.0);
    REQUIRE(saw_joining);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    net.same(client);
    REQUIRE(client.session->applier().skipped() == 0);
}

TEST_CASE("RP42 two clients see the same world, and see each other leave", "[replication][session]") {
    Net net;
    Client& a = net.add_client();
    Client& b = net.add_client();
    net.run(0.5);
    add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Shared");
    net.run(0.2);
    net.same(a);
    net.same(b);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 2);
    b.session->disconnect(net.now);
    net.run(0.5);
    REQUIRE(b.session->reason() == "You left the game");
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 1);
    REQUIRE(a.rig.game.get_children(a.rig.game.connections()).size() == 1);
    REQUIRE(b.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE_FALSE(b.rig.game.simulation_running());   // play stopped: the empty place is back
    net.same(a);
    REQUIRE(a.session->applier().skipped() == 0);
}

TEST_CASE("RP43 a message from a client is dropped and counted", "[replication][session]") {
    Net net;
    Client& client = net.add_client();
    net.run(0.5);
    const std::uint8_t hello[3] = {1, 2, 3};
    REQUIRE(client.session->endpoint().send_message(0, hello, sizeof(hello)));
    net.run(0.3);
    REQUIRE(net.session->dropped_messages() == 1);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
}

TEST_CASE("RP44 a client of another game or version is told why", "[replication][session]") {
    Net net;
    network::GameIdentity other = identity();
    other.game_id[0] ^= 0xFF;
    Client& wrong_game = net.add_client(other);
    network::GameIdentity old = identity();
    old.version = "0.9.0";
    Client& wrong_version = net.add_client(old);
    net.run(0.5);
    REQUIRE(wrong_game.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE(wrong_game.session->reason() == "This server runs a different game");
    REQUIRE(wrong_version.session->reason() == "This game needs an update (the server runs 1.0.0)");
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
}

TEST_CASE("RP45 the server closing tells clients so; a client that stops hearing the server says it lost it", "[replication][session]") {
    Net net;
    Client& closed = net.add_client();
    net.run(0.5);
    net.session->close(net.now);
    net.run(0.5);
    REQUIRE(closed.session->reason() == "The server closed");
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
    Net silent;
    Client& lost = silent.add_client();
    silent.run(0.5);
    silent.session->endpoint().conditioner().loss = 1.0f;   // nothing more reaches the client
    silent.run(6.0);
    REQUIRE(lost.session->reason() == "Lost connection to the server");
    Net nobody;
    nobody.session.reset();   // no server listens
    Client& unreachable = nobody.add_client();
    for (int i = 0; i < 6 * 60; ++i) {
        nobody.now += 1.0 / 60.0;
        unreachable.session->receive(nobody.now);
        unreachable.session->send(nobody.now);
    }
    REQUIRE(unreachable.session->reason() == "Could not reach the server");
}

TEST_CASE("RP46 a client too far behind is dropped", "[replication][session]") {
    engine_network::SessionConfig config;
    SECTION("its queue stalls") {
        config.max_stall_seconds = 1.0;
    }
    SECTION("its queue grows too big") {
        config.max_queued_bytes = 256;
    }
    Net net(config);
    Client& client = net.add_client();
    net.run(0.5);
    const network::PeerId peer = only_new_peer({}, net.session->endpoint().peers());
    REQUIRE(net.session->connection_of(peer) != 0);
    net.session->endpoint().conditioner().loss = 1.0f;   // the client never acks
    engine_core::Folder& churn = add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Churn");
    // The window fills in about four seconds; the client gives up on a silent server at five.
    for (int i = 0; i < 60 * 9 && net.session->fell_behind() == 0; ++i) {
        net.server.game.set_name(churn.id(), "Churn" + std::to_string(i));
        net.tick();
    }
    REQUIRE(net.session->fell_behind() == 1);
    REQUIRE(net.session->connection_of(peer) == 0);
    REQUIRE(net.session->queued_bytes(peer) == 0);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
    (void)client;
}

TEST_CASE("RP47 bad data from the server ends the connection with a reason", "[replication][session]") {
    // A real snapshot, as a server would send it, for the cases that join first.
    ScriptRig source;
    replication::start_server(source);
    add_folder(source.game, source.game.scene_service("Workspace"), "Level");
    std::vector<std::vector<std::uint8_t>> snapshot;
    {
        engine_network::Replicator replicator(source.game);
        replicator.snapshot_messages(0, snapshot);
    }
    std::vector<std::vector<std::uint8_t>> messages;
    SECTION("an unknown message type") {
        messages.push_back({0xEE, 0x01, 0x02, 0x03});
    }
    SECTION("changes before the snapshot") {
        messages.push_back({2, 0x00});
    }
    SECTION("a snapshot part claiming a huge snapshot") {
        messages.push_back({1, engine_network::kCodecVersion, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F});
    }
    SECTION("a snapshot of another codec version") {
        messages = snapshot;
        messages[0][1] = engine_network::kCodecVersion + 1;
    }
    SECTION("an empty message") {
        messages.push_back({});
    }
    SECTION("garbage changes after a good snapshot") {
        messages = snapshot;
        messages.push_back({2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    }
    network::LinkHub hub;
    ScriptRig rig;
    engine_network::SessionConfig config;
    config.endpoint.identity = identity();
    engine_network::ClientSession client(rig.game, hub.open(client_address(0)), config);
    serve_raw(client, hub, messages);
    REQUIRE(client.state() == engine_network::ClientState::Disconnected);
    REQUIRE(client.reason() == "Bad data from the server");
    REQUIRE_FALSE(rig.game.simulation_running());
}

TEST_CASE("RP41b a big snapshot into a world that changes every tick still joins", "[replication][session]") {
    engine_network::SessionConfig config;
    config.replicator.snapshot_part_bytes = 8192;
    // Each part takes longer than this to cross the thin link below, so only its acks show progress.
    config.max_stall_seconds = 3.0;
    Net net(config);
    const InstanceId workspace = net.server.game.scene_service("Workspace");
    for (int i = 0; i < 4000; ++i) {
        add_folder(net.server.game, workspace, ("Folder" + std::to_string(i * 7919)).c_str());
    }
    engine_core::Folder& busy = add_folder(net.server.game, workspace, "Busy");
    Client& client = net.add_client();
    // A thin link: the snapshot takes over a minute, so far more than 3600
    // changes queue behind it, and its acks are the only progress for long stretches.
    net.session->endpoint().conditioner().bandwidth_bytes_per_second = 1500.0;
    int joining_ticks = 0;
    for (int i = 0; i < 60 * 300 && client.session->state() != engine_network::ClientState::Joined; ++i) {
        net.server.game.set_name(busy.id(), "Busy" + std::to_string(i));
        if (i % 600 == 0) add_folder(net.server.game, busy.id(), ("Kid" + std::to_string(i)).c_str());
        net.tick();
        if (client.session->state() == engine_network::ClientState::Joining) ++joining_ticks;
    }
    INFO("joining ticks " << joining_ticks);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(joining_ticks > 3600 + 256);
    REQUIRE(net.session->fell_behind() == 0);
    // The link opens up; the backlog drains and the client catches up.
    net.session->endpoint().conditioner().bandwidth_bytes_per_second = 0.0;
    const network::PeerId peer = only_new_peer({}, net.session->endpoint().peers());
    for (int i = 0; i < 60 * 120 && net.session->queued_bytes(peer) != 0; ++i) net.tick();
    net.run(1.0);
    REQUIRE(net.session->queued_bytes(peer) == 0);
    net.same(client);
    REQUIRE(client.session->applier().skipped() == 0);
}

TEST_CASE("RP42b two clients joining in the same tick each get their own Connection", "[replication][session]") {
    Net net;
    add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Level");
    Client& a = net.add_client();
    Client& b = net.add_client();
    for (int i = 0; i < 60 && net.session->endpoint().peers().empty(); ++i) net.tick();
    const std::vector<network::PeerId> peers = net.session->endpoint().peers();
    REQUIRE(peers.size() == 2);   // both connected in the same tick, so both snapshots went in it
    net.run(0.5);
    REQUIRE(a.session->state() == engine_network::ClientState::Joined);
    REQUIRE(b.session->state() == engine_network::ClientState::Joined);
    net.same(a);
    net.same(b);
    const std::string ids[2] = {server_connection_id(net, peers[0]), server_connection_id(net, peers[1])};
    REQUIRE(ids[0] != ids[1]);
    REQUIRE(local_connection_id(a) != local_connection_id(b));
    for (Client* client : {&a, &b}) {
        const std::string mine = local_connection_id(*client);
        REQUIRE((mine == ids[0] || mine == ids[1]));
        REQUIRE(client->session->applier().skipped() == 0);
    }
}

TEST_CASE("RP42c a client leaving mid-snapshot leaves nothing behind", "[replication][session]") {
    engine_network::SessionConfig config;
    config.replicator.snapshot_part_bytes = 2048;
    Net net(config);
    const InstanceId workspace = net.server.game.scene_service("Workspace");
    for (int i = 0; i < 2000; ++i) {
        add_folder(net.server.game, workspace, ("F" + std::to_string(i * 7919)).c_str());
    }
    Client& stays = net.add_client();
    net.run(3.0);
    REQUIRE(stays.session->state() == engine_network::ClientState::Joined);
    const std::vector<network::PeerId> before = net.session->endpoint().peers();
    Client& leaves = net.add_client();
    for (int i = 0; i < 60 && net.session->endpoint().peers().size() < 2; ++i) net.tick();
    const network::PeerId peer = only_new_peer(before, net.session->endpoint().peers());
    net.tick();
    REQUIRE(leaves.session->state() == engine_network::ClientState::Joining);
    leaves.session->disconnect(net.now);
    engine_core::Folder& later = add_folder(net.server.game, workspace, "Later");
    net.run(0.5);
    REQUIRE(net.session->connection_of(peer) == 0);
    REQUIRE(net.session->queued_bytes(peer) == 0);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 1);
    net.server.game.set_name(later.id(), "Later2");
    net.run(0.2);
    net.same(stays);
    REQUIRE(stays.session->applier().skipped() == 0);
    // A new client, likely in the same slot, starts from a clean queue.
    Client& next = net.add_client();
    net.run(3.0);
    REQUIRE(next.session->state() == engine_network::ClientState::Joined);
    net.same(next);
    REQUIRE(next.session->applier().skipped() == 0);
}

TEST_CASE("RP44b a refused connect leaves play it did not start alone", "[replication][session]") {
    Net net;
    network::GameIdentity other = identity();
    other.game_id[0] ^= 0xFF;
    net.clients.push_back(std::make_unique<Client>());
    Client& client = *net.clients.back();
    client.rig.game.start_simulation();   // already playing, standalone
    engine_network::SessionConfig config;
    config.endpoint.identity = other;
    client.session = std::make_unique<engine_network::ClientSession>(client.rig.game, net.hub.open(client_address(0)), config);
    REQUIRE(client.session->connect(kServer, net.now));
    net.run(0.5);
    REQUIRE(client.session->reason() == "This server runs a different game");
    REQUIRE(client.rig.game.simulation_running());
}

TEST_CASE("RP45b destroying a session ends its connections", "[replication][session]") {
    Net net;
    Client& a = net.add_client();
    Client& b = net.add_client();
    net.run(0.5);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 2);
    b.session.reset();   // the client goes away: it leaves (its world stays, as a rig must until the test ends)
    net.run(0.5);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 1);
    net.session.reset();   // the server goes away: it closes
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
    for (int i = 0; i < 30; ++i) {
        net.now += 1.0 / 60.0;
        a.session->receive(net.now);
        a.session->send(net.now);
    }
    REQUIRE(a.session->reason() == "The server closed");
    REQUIRE_FALSE(a.rig.game.simulation_running());
}

TEST_CASE("RP45c a connection's end puts back the world role it set, and leaves one it found alone", "[replication][session]") {
    Net net;
    Client& client = net.add_client();
    REQUIRE(client.rig.game.world_role() == engine_core::WorldRole::Standalone);
    net.run(0.5);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(client.rig.game.world_role() == engine_core::WorldRole::Client);
    client.session->disconnect(net.now);
    net.run(0.2);
    REQUIRE(client.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE_FALSE(client.rig.game.simulation_running());
    REQUIRE(client.rig.game.world_role() == engine_core::WorldRole::Standalone);
    // A world that was a Client already stays one.
    Client& already = net.add_client();
    already.session.reset();
    already.rig.game.set_world_role(engine_core::WorldRole::Client);
    engine_network::SessionConfig config;
    config.endpoint.identity = identity();
    already.session = std::make_unique<engine_network::ClientSession>(already.rig.game, net.hub.open(client_address(5)), config);
    REQUIRE(already.session->connect(kServer, net.now));
    net.run(0.5);
    REQUIRE(already.session->state() == engine_network::ClientState::Joined);
    net.session->close(net.now);
    net.run(0.5);
    REQUIRE(already.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE(already.rig.game.world_role() == engine_core::WorldRole::Client);
}

TEST_CASE("RP50 a server script destroying a client's Connection removes that client", "[replication][session]") {
    Net net;
    Client& a = net.add_client();
    for (int i = 0; i < 60 && net.session->endpoint().peers().empty(); ++i) net.tick();
    const network::PeerId peer_a = only_new_peer({}, net.session->endpoint().peers());
    Client& b = net.add_client();
    net.run(0.5);
    REQUIRE(a.session->state() == engine_network::ClientState::Joined);
    REQUIRE(b.session->state() == engine_network::ClientState::Joined);
    REQUIRE(b.rig.game.get_children(b.rig.game.connections()).size() == 2);
    net.server.game.destroy_tree(net.session->connection_of(peer_a));   // as Destroy from a script would
    net.run(0.3);
    REQUIRE(a.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE(a.session->reason() == "Removed by the server");
    REQUIRE_FALSE(a.rig.game.simulation_running());
    REQUIRE(net.session->connection_of(peer_a) == 0);
    REQUIRE(b.session->state() == engine_network::ClientState::Joined);
    const std::vector<InstanceId> left = b.rig.game.get_children(b.rig.game.connections());
    REQUIRE(left.size() == 1);
    REQUIRE(static_cast<engine_core::ConnectionInstance*>(b.rig.game.instance(left[0]))->connection_id() ==
            local_connection_id(b));
    net.same(b);
    REQUIRE(b.session->applier().skipped() == 0);
}

TEST_CASE("RP51 closing the server tells each client \"The server closed\" at once", "[replication][session]") {
    Net net;
    Client& a = net.add_client();
    Client& b = net.add_client();
    net.run(0.5);
    REQUIRE(a.session->state() == engine_network::ClientState::Joined);
    REQUIRE(b.session->state() == engine_network::ClientState::Joined);
    net.session->close(net.now);
    net.tick();
    for (Client* client : {&a, &b}) {
        REQUIRE(client->session->reason() == "The server closed");
        REQUIRE(client->session->state() == engine_network::ClientState::Disconnected);
    }
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
}

namespace {

// status(), read as another thread would, says what state() and reason() say.
void require_status_matches(engine_network::ClientSession& session) {
    const engine_network::ClientStatus status = session.status();
    REQUIRE(status.state == session.state());
    REQUIRE(status.reason == session.reason());
}

}  // namespace

TEST_CASE("RP52 status() matches state() and reason() through a whole connection", "[replication][session]") {
    Net net;
    Client& good = net.add_client();
    REQUIRE(good.session->status().state == engine_network::ClientState::Connecting);
    require_status_matches(*good.session);
    net.run(0.5);
    REQUIRE(good.session->status().state == engine_network::ClientState::Joined);
    require_status_matches(*good.session);
    network::GameIdentity other = identity();
    other.game_id[0] ^= 0xFF;
    Client& wrong = net.add_client(other);
    net.run(0.5);
    REQUIRE(wrong.session->status().state == engine_network::ClientState::Disconnected);
    REQUIRE(wrong.session->status().reason == "This server runs a different game");
    require_status_matches(*wrong.session);
}

TEST_CASE("RP53 status() is safe from another thread", "[replication][session]") {
    Net net;
    Client& client = net.add_client();
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    engine_network::ClientStatus last;
    std::thread reader([&] {
        for (;;) {
            const bool done = stop.load();
            last = client.session->status();
            reads.fetch_add(1);
            if (done) break;
        }
    });
    // A failed REQUIRE below still stops and joins the reader, rather than ending the run.
    struct Join {
        std::atomic<bool>& stop;
        std::thread& thread;
        ~Join() {
            stop.store(true);
            if (thread.joinable()) thread.join();
        }
    } join{stop, reader};
    for (int round = 0; round < 20; ++round) {
        if (round > 0) REQUIRE(client.session->connect(kServer, net.now));
        for (int i = 0; i < 120 && client.session->state() != engine_network::ClientState::Joined; ++i) net.tick();
        REQUIRE(client.session->state() == engine_network::ClientState::Joined);
        client.session->disconnect(net.now);
        for (int i = 0; i < 60 && client.session->state() != engine_network::ClientState::Disconnected; ++i) net.tick();
        REQUIRE(client.session->state() == engine_network::ClientState::Disconnected);
        net.run(0.1);   // the server forgets the client before it comes back
    }
    stop.store(true);
    reader.join();
    REQUIRE(reads.load() > 1);
    REQUIRE(last.state == engine_network::ClientState::Disconnected);
    REQUIRE(last.reason == "You left the game");
}

TEST_CASE("RP63 a service value the server put back to its default reaches a client whose project set it", "[replication][session]") {
    using engine_core::Lighting;
    Net net;
    // The server's project set Exposure, and a server script put it back before anyone joined.
    auto* server_lighting = static_cast<Lighting*>(net.server.game.instance(net.server.game.service("Lighting")));
    REQUIRE_FALSE(server_lighting->set_exposure(2.5));
    REQUIRE_FALSE(server_lighting->set_ambient({0.f, 0.f, 1.f, 1.f}));
    net.tick();
    REQUIRE_FALSE(server_lighting->set_exposure(Lighting::kDefaultExposure));
    REQUIRE_FALSE(server_lighting->set_ambient(Lighting::kDefaultAmbient));
    net.tick();
    // The client loaded the same project, so it starts with the project's values.
    net.clients.push_back(std::make_unique<Client>());
    Client& client = *net.clients.back();
    auto* client_lighting = static_cast<Lighting*>(client.rig.game.instance(client.rig.game.service("Lighting")));
    REQUIRE_FALSE(client_lighting->set_exposure(2.5));
    REQUIRE_FALSE(client_lighting->set_ambient({0.f, 0.f, 1.f, 1.f}));
    engine_network::clear_for_client(client.rig.game);
    engine_network::SessionConfig config;
    config.endpoint.identity = identity();
    client.session = std::make_unique<engine_network::ClientSession>(client.rig.game, net.hub.open(client_address(0)), config);
    REQUIRE(client.session->connect(kServer, net.now));
    net.run(0.5);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(client_lighting->exposure() == Lighting::kDefaultExposure);
    const engine_core::ColorRgb ambient = client_lighting->ambient();
    REQUIRE(ambient.r == Lighting::kDefaultAmbient.r);
    REQUIRE(ambient.g == Lighting::kDefaultAmbient.g);
    REQUIRE(ambient.b == Lighting::kDefaultAmbient.b);
    net.same(client);
}
