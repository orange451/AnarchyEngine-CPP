// A server and its clients: the handshake, identity checks, timeouts,
// disconnects, and a whole session over a bad link (spec sections 5 and 6).

#include "Bits.hpp"
#include "Endpoint.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(rtt > 0.28);
    REQUIRE(rtt < 0.42);
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
