// A server and its clients: the handshake, identity checks, timeouts,
// disconnects, and a whole session over a bad link (spec sections 5 and 6).

#include "Bits.hpp"
#include "Endpoint.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using Catch::Approx;
using network::Address;
using network::ConnectionState;
using network::DenyReason;
using network::DisconnectReason;
using network::Endpoint;
using network::EndpointCallbacks;
using network::EndpointConfig;
using network::GameIdentity;
using network::PeerId;
using network::Role;

namespace {

struct Log {
    std::vector<PeerId> connected;
    std::vector<std::tuple<PeerId, DisconnectReason, DenyReason>> disconnected;
    std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> messages;
    std::vector<std::tuple<PeerId, std::uint16_t, std::vector<std::uint8_t>>> payloads;
    std::vector<std::tuple<PeerId, std::uint16_t, std::uint32_t>> delivered;
    EndpointCallbacks callbacks() {
        EndpointCallbacks c;
        c.on_connected = [this](PeerId p) { connected.push_back(p); };
        c.on_disconnected = [this](PeerId p, DisconnectReason r, DenyReason d) { disconnected.emplace_back(p, r, d); };
        c.on_message = [this](PeerId p, const std::uint8_t* b, std::size_t n) { messages.emplace_back(p, std::vector<std::uint8_t>(b, b + n)); };
        c.on_payload = [this](PeerId p, std::uint16_t s, const std::uint8_t* b, std::size_t n) {
            payloads.emplace_back(p, s, std::vector<std::uint8_t>(b, b + n));
        };
        c.on_delivered = [this](PeerId p, std::uint16_t s, std::uint32_t t) { delivered.emplace_back(p, s, t); };
        return c;
    }
};

GameIdentity identity(const char* id = "0123456789abcdef0123456789abcdef", const char* version = "1.0.0", std::uint64_t hash = 42) {
    GameIdentity identity;
    network::parse_game_id(id, identity.game_id);
    identity.version = version;
    identity.resources_hash = hash;
    return identity;
}

// Addresses on the hub: the server is port 1, clients are 100 + n.
const Address kServer = Address::loopback(1);
Address client_address(int n) { return Address::loopback(static_cast<std::uint16_t>(100 + n)); }

struct Net {
    network::LinkHub hub;
    Log server_log;
    std::unique_ptr<Endpoint> server;
    std::deque<Log> client_logs;   // a deque: callbacks hold each Log's address
    std::vector<std::unique_ptr<Endpoint>> clients;

    explicit Net(std::uint8_t max_peers = 4, GameIdentity server_identity = identity()) {
        EndpointConfig config;
        config.role = Role::Server;
        config.max_peers = max_peers;
        config.identity = server_identity;
        server = std::make_unique<Endpoint>(hub.open(kServer), config, server_log.callbacks());
    }
    Endpoint& add_client(GameIdentity client_identity = identity()) {
        client_logs.emplace_back();
        EndpointConfig config;
        config.role = Role::Client;
        config.identity = client_identity;
        clients.push_back(std::make_unique<Endpoint>(hub.open(client_address(static_cast<int>(clients.size()))), config,
                                                     client_logs.back().callbacks()));
        return *clients.back();
    }
    // Everyone receives, then everyone sends, as the engine ticks.
    void tick(double now) {
        server->receive(now);
        for (auto& c : clients) c->receive(now);
        server->send(now);
        for (auto& c : clients) c->send(now);
    }
    double run(double from, double seconds) {
        const int ticks = static_cast<int>(seconds * 60.0);
        for (int i = 0; i <= ticks; ++i) tick(from + i / 60.0);
        return from + ticks / 60.0;
    }
};

// Sends a raw packet from an address on the hub.
void raw_send(network::LinkHub& hub, const Address& from, const Address& to, network::BitWriter& writer, std::uint8_t* buffer) {
    auto socket = hub.open(from);
    const std::size_t length = network::encode_packet(buffer, writer.bytes_written(), network::kMaxPacketBytes);
    socket->send(to, buffer, length);
}

}  // namespace

TEST_CASE("N60 a client connects, both sides talk, and a second request from the same address with another salt is ignored", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    REQUIRE(client.connect(kServer, 0.0));
    REQUIRE_FALSE(client.connect(kServer, 0.0));
    REQUIRE(client.state(0) == ConnectionState::Connecting);
    net.run(0.0, 0.1);
    REQUIRE(net.server_log.connected == std::vector<PeerId>{0});
    REQUIRE(net.client_logs[0].connected == std::vector<PeerId>{0});
    REQUIRE(client.state(0) == ConnectionState::Connected);
    REQUIRE(net.server->state(0) == ConnectionState::Connected);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
    REQUIRE(client.session(0) == net.server->session(0));
    const std::uint8_t hello[2] = {'h', 'i'};
    REQUIRE(client.send_message(0, hello, 2));
    REQUIRE(net.server->send_message(0, hello, 2));
    const std::uint8_t state[3] = {1, 2, 3};
    const auto sequence = client.send_payload(0, state, 3, 9);
    REQUIRE(sequence.has_value());
    net.run(0.2, 0.2);  // the ack rides the server's next keep-alive, which is 0.1 s away
    REQUIRE(net.server_log.messages.size() == 1);
    REQUIRE(net.client_logs[0].messages.size() == 1);
    REQUIRE(net.server_log.payloads.size() == 1);
    REQUIRE(std::get<1>(net.server_log.payloads[0]) == *sequence);
    REQUIRE(net.client_logs[0].delivered.size() == 1);
    REQUIRE(std::get<2>(net.client_logs[0].delivered[0]) == 9);
    // Review Focus 1: another salt from the live client's address.
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    header.type = network::PacketType::ConnectRequest;
    network::write_header(writer, header);
    writer.bits(123, 32);
    writer.bits(0, 32);
    network::write_identity(writer, identity());
    raw_send(net.hub, client_address(0), kServer, writer, buffer);
    net.run(0.4, 0.1);
    REQUIRE(net.server_log.connected.size() == 1);
    REQUIRE(net.server->stats().dropped_unknown_peer == 1);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
    REQUIRE(client.send_message(0, hello, 2));
    net.run(0.6, 0.1);
    REQUIRE(net.server_log.messages.size() == 2);
}

TEST_CASE("N61 a packet with a session that ended is dropped and counted", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    const std::uint16_t old_session = client.session(0);
    client.disconnect(0, 0.2);
    net.run(0.2, 0.1);
    REQUIRE(net.server->peers().empty());
    client.connect(kServer, 0.4);
    double now = net.run(0.4, 0.1);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
    REQUIRE(client.session(0) != old_session);
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    header.type = network::PacketType::Payload;
    header.session = old_session;
    header.sequence = 500;
    network::write_header(writer, header);
    writer.boolean(false);
    raw_send(net.hub, client_address(0), kServer, writer, buffer);
    net.run(now, 0.05);
    REQUIRE(net.server->stats().dropped_bad_session == 1);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
}

TEST_CASE("N62 silence times a peer out after 5 s, and a clock that jumps a minute times everyone out cleanly", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    // The client stops pumping.
    for (int i = 0; i < 60 * 6; ++i) {
        net.server->receive(0.2 + i / 60.0);
        net.server->send(0.2 + i / 60.0);
    }
    REQUIRE(net.server_log.disconnected.size() == 1);
    REQUIRE(std::get<1>(net.server_log.disconnected[0]) == DisconnectReason::Timeout);
    REQUIRE(net.server->peers().empty());
    // Review Focus 3: a jump forward.
    Net jump;
    Endpoint& c2 = jump.add_client();
    c2.connect(kServer, 0.0);
    jump.run(0.0, 0.1);
    jump.tick(60.0);
    jump.tick(60.02);
    REQUIRE(jump.server_log.disconnected.size() == 1);
    REQUIRE(jump.client_logs[0].disconnected.size() == 1);
    REQUIRE(c2.connect(kServer, 60.1));
    jump.run(60.1, 0.1);
    REQUIRE(jump.server_log.connected.size() == 2);
}

TEST_CASE("N63 the server denies another game, another version, other content, a full server, and drops another protocol", "[network][endpoint]") {
    Net net(1);
    Endpoint& wrong_game = net.add_client(identity("ffffffffffffffffffffffffffffffff"));
    wrong_game.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    REQUIRE(net.client_logs[0].disconnected.size() == 1);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[0]) == DisconnectReason::Denied);
    REQUIRE(std::get<2>(net.client_logs[0].disconnected[0]) == DenyReason::WrongGame);
    Endpoint& wrong_version = net.add_client(identity("0123456789abcdef0123456789abcdef", "0.9.0"));
    wrong_version.connect(kServer, 0.2);
    net.run(0.2, 0.1);
    REQUIRE(std::get<2>(net.client_logs[1].disconnected[0]) == DenyReason::WrongVersion);
    REQUIRE(wrong_version.denied_version() == "1.0.0");
    Endpoint& wrong_content = net.add_client(identity("0123456789abcdef0123456789abcdef", "1.0.0", 43));
    wrong_content.connect(kServer, 0.4);
    net.run(0.4, 0.1);
    REQUIRE(std::get<2>(net.client_logs[2].disconnected[0]) == DenyReason::WrongContent);
    Endpoint& first = net.add_client();
    first.connect(kServer, 0.6);
    net.run(0.6, 0.1);
    REQUIRE(first.state(0) == ConnectionState::Connected);
    Endpoint& second = net.add_client();
    second.connect(kServer, 0.8);
    net.run(0.8, 0.1);
    REQUIRE(std::get<2>(net.client_logs[4].disconnected[0]) == DenyReason::Full);
    REQUIRE(net.server->peers().size() == 1);
    // Review Focus 4: another protocol id.
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    header.protocol = 0x1234;
    network::write_header(writer, header);
    raw_send(net.hub, client_address(7), kServer, writer, buffer);
    net.run(1.0, 0.05);
    REQUIRE(net.server->stats().dropped_bad_protocol == 1);
}

TEST_CASE("N64 a connect that nobody answers times out after 5 s", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(Address::loopback(9), 0.0);
    for (int i = 0; i < 60 * 6; ++i) {
        client.receive(i / 60.0);
        client.send(i / 60.0);
    }
    REQUIRE(net.client_logs[0].disconnected.size() == 1);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[0]) == DisconnectReason::Timeout);
    REQUIRE(client.state(0) == ConnectionState::Disconnected);
}

TEST_CASE("N65 either side can disconnect and the other learns why", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    client.disconnect(0, 0.2);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[0]) == DisconnectReason::Requested);
    net.run(0.2, 0.1);
    REQUIRE(std::get<1>(net.server_log.disconnected[0]) == DisconnectReason::Requested);
    client.connect(kServer, 0.4);
    net.run(0.4, 0.1);
    net.server->disconnect(0, 0.6);
    net.run(0.6, 0.1);
    REQUIRE(net.client_logs[0].disconnected.size() == 2);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[1]) == DisconnectReason::Requested);
    REQUIRE(client.state(0) == ConnectionState::Disconnected);
}

TEST_CASE("N66 a session over 150 ms, 20 ms jitter, and 2% loss each way delivers every message in order", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    for (Endpoint* e : {net.server.get(), &client}) {
        e->conditioner().latency_seconds = 0.150;
        e->conditioner().jitter_seconds = 0.020;
        e->conditioner().loss = 0.02f;
    }
    client.connect(kServer, 0.0);
    double now = net.run(0.0, 1.0);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    int sent = 0;
    int delivered_reports = 0;
    for (int tick = 0; tick < 60 * 10; ++tick) {
        now += 1.0 / 60.0;
        if (sent < 300) {
            const std::uint8_t byte = static_cast<std::uint8_t>(sent);
            if (client.send_message(0, &byte, 1)) ++sent;
        }
        const std::uint8_t state[4] = {1, 2, 3, 4};
        client.send_payload(0, state, 4, static_cast<std::uint32_t>(tick));
        net.tick(now);
    }
    REQUIRE(net.server_log.messages.size() == 300);
    for (int i = 0; i < 300; ++i) REQUIRE(net.server_log.messages[i].second[0] == static_cast<std::uint8_t>(i));
    delivered_reports = static_cast<int>(net.client_logs[0].delivered.size());
    REQUIRE(delivered_reports > 500);
    const double rtt = client.stats(0, now).rtt;
    REQUIRE(rtt > 0.24);
    REQUIRE(rtt < 0.36);
    REQUIRE(net.server->stats(0, now).loss < 0.1f);
}

TEST_CASE("N67 a stranger's payload is dropped and counted", "[network][endpoint]") {
    Net net;
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    network::write_header(writer, header);
    writer.boolean(false);
    raw_send(net.hub, client_address(3), kServer, writer, buffer);
    net.run(0.0, 0.05);
    REQUIRE(net.server->stats().dropped_unknown_peer == 1);
}

TEST_CASE("N68 a lost accept is answered again on the client's next request, with one connected event", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    net.server->conditioner().loss = 1.0f;
    client.connect(kServer, 0.0);
    net.run(0.0, 0.3);
    REQUIRE(net.server_log.connected.size() == 1);
    REQUIRE(client.state(0) == ConnectionState::Connecting);
    net.server->conditioner().loss = 0.0f;
    net.run(0.3, 0.3);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    REQUIRE(net.server_log.connected.size() == 1);
    REQUIRE(net.client_logs[0].connected.size() == 1);
}

namespace {

// A socket that stops working after a number of sends.
class FailingSocket : public network::ISocket {
public:
    FailingSocket(std::unique_ptr<network::ISocket> inner, int sends_before_failure) : inner_(std::move(inner)), left_(sends_before_failure) {}
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
        if (left_-- <= 0) {
            failed_ = true;
            return false;
        }
        return inner_->send(to, bytes, length);
    }
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
        return failed_ ? 0 : inner_->receive(from, buffer, capacity);
    }
    bool failed() const override { return failed_; }

private:
    std::unique_ptr<network::ISocket> inner_;
    int left_;
    bool failed_ = false;
};

}  // namespace

TEST_CASE("N69 a socket that fails drops every peer with reason Socket", "[network][endpoint]") {
    network::LinkHub hub;
    Log server_log;
    Log client_log;
    EndpointConfig server_config;
    server_config.identity = identity();
    Endpoint server(std::make_unique<FailingSocket>(hub.open(kServer), 20), server_config, server_log.callbacks());
    EndpointConfig client_config;
    client_config.role = Role::Client;
    client_config.identity = identity();
    Endpoint client(hub.open(client_address(0)), client_config, client_log.callbacks());
    client.connect(kServer, 0.0);
    for (int i = 0; i < 60 * 3; ++i) {
        const double now = i / 60.0;
        server.receive(now);
        client.receive(now);
        server.send(now);
        client.send(now);
    }
    REQUIRE(server.failed());
    REQUIRE(server_log.disconnected.size() == 1);
    REQUIRE(std::get<1>(server_log.disconnected[0]) == DisconnectReason::Socket);
    REQUIRE(server.peers().empty());
}

TEST_CASE("N69b a callback that disconnects another peer does not disturb the packet being read", "[network][endpoint]") {
    network::LinkHub hub;
    Log server_log;
    Endpoint* server_ptr = nullptr;
    double clock = 0.0;
    std::vector<std::string> order;
    EndpointCallbacks server_callbacks = server_log.callbacks();
    server_callbacks.on_message = [&](PeerId peer, const std::uint8_t*, std::size_t) {
        order.push_back("message");
        if (peer == 0) server_ptr->disconnect(1, clock);
    };
    server_callbacks.on_payload = [&](PeerId peer, std::uint16_t sequence, const std::uint8_t* bytes, std::size_t length) {
        order.push_back("payload");
        server_log.payloads.emplace_back(peer, sequence, std::vector<std::uint8_t>(bytes, bytes + length));
    };
    EndpointConfig server_config;
    server_config.identity = identity();
    Endpoint server(hub.open(kServer), server_config, server_callbacks);
    server_ptr = &server;
    Log logs[2];
    std::unique_ptr<Endpoint> clients[2];
    for (int i = 0; i < 2; ++i) {
        EndpointConfig config;
        config.role = Role::Client;
        config.identity = identity();
        clients[i] = std::make_unique<Endpoint>(hub.open(client_address(i)), config, logs[i].callbacks());
    }
    auto tick = [&](double now) {
        clock = now;
        server.receive(now);
        for (auto& c : clients) c->receive(now);
        server.send(now);
        for (auto& c : clients) c->send(now);
    };
    clients[0]->connect(kServer, 0.0);
    clients[1]->connect(kServer, 0.0);
    double now = 0.0;
    for (int i = 0; i < 12; ++i) tick(now += 1.0 / 60.0);
    REQUIRE(server.peers().size() == 2);
    const std::uint8_t byte = 5;
    const std::uint8_t state[4] = {9, 8, 7, 6};
    REQUIRE(clients[0]->send_message(0, &byte, 1));
    REQUIRE(clients[0]->send_payload(0, state, 4, 1).has_value());
    for (int i = 0; i < 12; ++i) tick(now += 1.0 / 60.0);
    REQUIRE(order == std::vector<std::string>{"message", "payload"});
    REQUIRE(server_log.payloads.size() == 1);
    REQUIRE(std::get<0>(server_log.payloads[0]) == 0);
    REQUIRE(std::get<2>(server_log.payloads[0]) == std::vector<std::uint8_t>{9, 8, 7, 6});
    REQUIRE(server.state(1) == ConnectionState::Disconnected);
    REQUIRE(std::get<1>(server_log.disconnected.at(0)) == DisconnectReason::Requested);
    REQUIRE(logs[1].disconnected.size() == 1);
    REQUIRE(std::get<1>(logs[1].disconnected[0]) == DisconnectReason::Requested);
}

TEST_CASE("N69c a callback that disconnects the peer being read is deferred and fires on_disconnected once", "[network][endpoint]") {
    network::LinkHub hub;
    Log server_log;
    Endpoint* server_ptr = nullptr;
    double clock = 0.0;
    int server_messages = 0;
    EndpointCallbacks server_callbacks = server_log.callbacks();
    server_callbacks.on_message = [&](PeerId peer, const std::uint8_t*, std::size_t) {
        ++server_messages;
        server_ptr->disconnect(peer, clock);
    };
    EndpointConfig server_config;
    server_config.identity = identity();
    Endpoint server(hub.open(kServer), server_config, server_callbacks);
    server_ptr = &server;
    Log client_log;
    EndpointConfig client_config;
    client_config.role = Role::Client;
    client_config.identity = identity();
    Endpoint client(hub.open(client_address(0)), client_config, client_log.callbacks());
    auto tick = [&](double now) {
        clock = now;
        server.receive(now);
        client.receive(now);
        server.send(now);
        client.send(now);
    };
    client.connect(kServer, 0.0);
    double now = 0.0;
    for (int i = 0; i < 12; ++i) tick(now += 1.0 / 60.0);
    REQUIRE(server.peers().size() == 1);
    const std::uint8_t one = 1, two = 2;
    const std::uint8_t state[3] = {4, 5, 6};
    REQUIRE(client.send_message(0, &one, 1));
    REQUIRE(client.send_message(0, &two, 1));
    REQUIRE(client.send_payload(0, state, 3, 1).has_value());
    for (int i = 0; i < 12; ++i) tick(now += 1.0 / 60.0);
    REQUIRE(server_messages == 1);
    REQUIRE(server_log.payloads.empty());
    REQUIRE(server_log.disconnected.size() == 1);
    REQUIRE(std::get<1>(server_log.disconnected[0]) == DisconnectReason::Requested);
    REQUIRE(server.peers().empty());
    REQUIRE(server.state(0) == ConnectionState::Disconnected);
    REQUIRE(client_log.disconnected.size() == 1);
    REQUIRE(std::get<1>(client_log.disconnected[0]) == DisconnectReason::Requested);
}

namespace {

// What happened to one big message the server sent its only client.
struct BigSend {
    int delivered_at_tick = -1;
    std::size_t deliveries = 0;
    bool bytes_equal = false;
    bool halved = false;
};

// Connects a client, lets RTT settle, sends it one message of length bytes, and pumps at 60 Hz
// until it lands or max_ticks pass.
BigSend send_big(double bytes_per_second, float loss, std::size_t length, int max_ticks) {
    network::LinkHub hub;
    Log server_log, client_log;
    EndpointConfig server_config;
    server_config.role = Role::Server;
    server_config.identity = identity();
    server_config.bytes_per_second = bytes_per_second;
    Endpoint server(hub.open(kServer), server_config, server_log.callbacks());
    EndpointConfig client_config;
    client_config.role = Role::Client;
    client_config.identity = identity();
    Endpoint client(hub.open(client_address(0)), client_config, client_log.callbacks());
    server.conditioner().loss = loss;
    client.conditioner().loss = loss;
    const auto tick = [&](double now) {
        server.receive(now);
        client.receive(now);
        server.send(now);
        client.send(now);
    };
    REQUIRE(client.connect(kServer, 0.0));
    int t = 0;
    for (; t < 60; ++t) tick(t / 60.0);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<std::uint8_t>(i * 131 + (i >> 8));
    REQUIRE(server.send_message(0, bytes.data(), bytes.size()));
    BigSend result;
    for (int i = 1; i <= max_ticks; ++i) {
        const double now = (t + i) / 60.0;
        tick(now);
        if (server.stats(0, now).budget_bytes_per_second == Approx(bytes_per_second * 0.5)) result.halved = true;
        if (result.delivered_at_tick < 0 && !client_log.messages.empty()) result.delivered_at_tick = i;
    }
    result.deliveries = client_log.messages.size();
    result.bytes_equal = result.deliveries == 1 && client_log.messages[0].second == bytes;
    return result;
}

}  // namespace

TEST_CASE("N73 a 600 KB message reaches a client with a 1 MB/s budget in under a second", "[network][endpoint]") {
    const BigSend result = send_big(1048576.0, 0.0f, 600000, 120);
    REQUIRE(result.delivered_at_tick > 0);
    REQUIRE(result.delivered_at_tick <= 60);
    REQUIRE(result.deliveries == 1);
    REQUIRE(result.bytes_equal);
}

TEST_CASE("N74 the budget holds: the same message at 64 KB/s takes at least eight seconds", "[network][endpoint]") {
    const BigSend result = send_big(65536.0, 0.0f, 600000, 60 * 20);
    REQUIRE(result.delivered_at_tick >= 8 * 60);
    REQUIRE(result.deliveries == 1);
    REQUIRE(result.bytes_equal);
}

TEST_CASE("N75 under 10% loss a 600 KB message arrives exactly once and flow control halves the budget", "[network][endpoint]") {
    // Pumped a while past delivery, so a duplicate would show.
    const BigSend result = send_big(1048576.0, 0.10f, 600000, 60 * 35);
    REQUIRE(result.delivered_at_tick > 0);
    REQUIRE(result.delivered_at_tick <= 60 * 30);
    REQUIRE(result.deliveries == 1);
    REQUIRE(result.bytes_equal);
    REQUIRE(result.halved);
}

TEST_CASE("N76 packets in flight stay inside the ring: at 250 ms RTT a tick carries at most 8 packets", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    for (Endpoint* e : {net.server.get(), &client}) e->conditioner().latency_seconds = 0.125;
    // The server's own config sets the budget; Net's default is EndpointConfig's.
    EndpointConfig config;
    config.role = Role::Server;
    config.identity = identity();
    config.bytes_per_second = 1048576.0;
    Log server_log;
    net.server = std::make_unique<Endpoint>(net.hub.open(Address::loopback(2)), config, server_log.callbacks());
    net.server->conditioner().latency_seconds = 0.125;
    REQUIRE(client.connect(Address::loopback(2), 0.0));
    double now = net.run(0.0, 2.0);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    REQUIRE(net.server->stats(0, now).rtt > 0.2);
    std::vector<std::uint8_t> bytes(2u << 20, 7);
    REQUIRE(net.server->send_message(0, bytes.data(), bytes.size()));
    std::uint64_t max_per_tick = 0;
    std::uint64_t last = net.server->stats(0, now).packets_sent;
    // The client's count over one second, from when the burst has had half a second to reach it.
    std::uint64_t client_before = 0;
    for (int i = 0; i < 60 * 4; ++i) {
        now += 1.0 / 60.0;
        net.tick(now);
        const std::uint64_t sent = net.server->stats(0, now).packets_sent;
        max_per_tick = std::max(max_per_tick, sent - last);
        last = sent;
        if (i == 29) client_before = client.stats(0, now).packets_sent;
        if (i == 89) REQUIRE(client.stats(0, now).packets_sent - client_before >= 55);
    }
    REQUIRE(max_per_tick > 1);
    REQUIRE(max_per_tick <= 8);
}

namespace {

// Passes everything through except the one send it is told to drop.
class DropOneSocket : public network::ISocket {
public:
    explicit DropOneSocket(std::unique_ptr<network::ISocket> inner) : inner_(std::move(inner)) {}
    // Drops the nth send from now (1 is the next).
    void drop_in(int n) { countdown_ = n; }
    bool dropped() const { return dropped_; }
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
        if (countdown_ > 0 && --countdown_ == 0) {
            dropped_ = true;
            return true;
        }
        return inner_->send(to, bytes, length);
    }
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
        return inner_->receive(from, buffer, capacity);
    }
    bool failed() const override { return inner_->failed(); }

private:
    std::unique_ptr<network::ISocket> inner_;
    int countdown_ = 0;
    bool dropped_ = false;
};

// A server and one client on the hub, ticked at 60 Hz the way the engine does.
struct Pair {
    network::LinkHub hub;
    Log server_log, client_log;
    std::unique_ptr<Endpoint> server, client;
    DropOneSocket* client_socket = nullptr;
    int ticks = 0;

    Pair(double bytes_per_second, double latency, float loss) {
        EndpointConfig server_config;
        server_config.role = Role::Server;
        server_config.identity = identity();
        server_config.bytes_per_second = bytes_per_second;
        server = std::make_unique<Endpoint>(hub.open(kServer), server_config, server_log.callbacks());
        EndpointConfig client_config;
        client_config.role = Role::Client;
        client_config.identity = identity();
        auto socket = std::make_unique<DropOneSocket>(hub.open(client_address(0)));
        client_socket = socket.get();
        client = std::make_unique<Endpoint>(std::move(socket), client_config, client_log.callbacks());
        for (Endpoint* e : {server.get(), client.get()}) {
            e->conditioner().latency_seconds = latency;
            e->conditioner().loss = loss;
        }
        REQUIRE(client->connect(kServer, 0.0));
    }
    double now() const { return ticks / 60.0; }
    void tick() {
        ++ticks;
        server->receive(now());
        client->receive(now());
        server->send(now());
        client->send(now());
    }
};

}  // namespace

TEST_CASE("N83 one lost ack in a burst costs nothing: no fragment is resent and the budget holds", "[network][endpoint]") {
    // 4 MB/s, so each tick carries the full 32 packets and one ack packet stands for a whole burst.
    Pair pair(4.0 * 1048576.0, 0.0, 0.0f);
    while (pair.ticks < 60) pair.tick();
    REQUIRE(pair.client->state(0) == ConnectionState::Connected);
    std::vector<std::uint8_t> bytes(600000, 5);
    REQUIRE(pair.server->send_message(0, bytes.data(), bytes.size()));
    // The client's sends from here are one ack packet a tick; the fifth is lost.
    pair.client_socket->drop_in(5);
    float max_loss = 0.0f;
    double min_budget = 1e30;
    std::uint64_t max_burst = 0;
    std::uint64_t last = pair.server->stats(0, pair.now()).packets_sent;
    for (int i = 0; i < 120; ++i) {
        pair.tick();
        const network::ConnectionStats stats = pair.server->stats(0, pair.now());
        max_loss = std::max(max_loss, stats.loss);
        min_budget = std::min(min_budget, stats.budget_bytes_per_second);
        max_burst = std::max(max_burst, stats.packets_sent - last);
        last = stats.packets_sent;
    }
    REQUIRE(pair.client_socket->dropped());
    REQUIRE(max_burst == 32);
    REQUIRE(pair.client_log.messages.size() == 1);
    REQUIRE(pair.client_log.messages[0].second == bytes);
    REQUIRE(pair.server->stats(0, pair.now()).reliable_resent == 0);
    REQUIRE(max_loss < 0.10f);
    REQUIRE(min_budget == Approx(4.0 * 1048576.0));
}

TEST_CASE("N84 the loss estimate holds while bursting: 0 on a clean 100 ms link, near 5% on a 5% one", "[network][endpoint]") {
    for (const float loss : {0.0f, 0.05f}) {
        CAPTURE(loss);
        Pair pair(1048576.0, 0.05, loss);
        while (pair.ticks < 120) pair.tick();
        REQUIRE(pair.client->state(0) == ConnectionState::Connected);
        std::vector<std::uint8_t> bytes(2u << 20, 9);
        REQUIRE(pair.server->send_message(0, bytes.data(), bytes.size()));
        float min_loss = 1.0f, max_loss = 0.0f;
        double min_budget = 1e30;
        int sampled = 0;
        for (int i = 0; i < 60 * 10 && pair.client_log.messages.empty(); ++i) {
            pair.tick();
            const network::ConnectionStats stats = pair.server->stats(0, pair.now());
            min_budget = std::min(min_budget, stats.budget_bytes_per_second);
            // From a second in, the two-second window is mostly the burst.
            if (i < 60) continue;
            min_loss = std::min(min_loss, stats.loss);
            max_loss = std::max(max_loss, stats.loss);
            ++sampled;
        }
        REQUIRE(pair.client_log.messages.size() == 1);
        REQUIRE(sampled > 30);
        if (loss == 0.0f) {
            REQUIRE(max_loss == 0.0f);
            REQUIRE(min_budget == Approx(1048576.0));
        } else {
            REQUIRE(min_loss >= 0.02f);
            REQUIRE(max_loss <= 0.10f);
        }
    }
}
