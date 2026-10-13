// The spec's soak test, the no-allocation rule, and the pumping benchmark
// (spec, Testing).

#include "alloc_counter.hpp"

#include "Endpoint.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using network::Address;
using network::ConnectionState;
using network::Endpoint;
using network::EndpointCallbacks;
using network::EndpointConfig;
using network::GameIdentity;
using network::PeerId;
using network::Role;

namespace {

GameIdentity identity() {
    GameIdentity identity;
    network::parse_game_id("0123456789abcdef0123456789abcdef", identity.game_id);
    identity.resources_hash = 7;
    return identity;
}

EndpointConfig config(Role role, std::uint8_t max_peers = 64) {
    EndpointConfig config;
    config.role = role;
    config.max_peers = max_peers;
    config.identity = identity();
    return config;
}

const Address kServer = Address::loopback(1);
Address client_address(int n) { return Address::loopback(static_cast<std::uint16_t>(100 + n)); }

// Two sockets joined by fixed rings: no heap, for the allocation test.
struct StaticLink {
    struct Datagram {
        std::uint16_t length;
        std::uint8_t bytes[network::kMaxPacketBytes];
    };
    struct Ring {
        Datagram slots[64];
        int head = 0;
        int count = 0;
    };
    Ring to_a;
    Ring to_b;

    class Socket : public network::ISocket {
    public:
        Socket(Ring& out, Ring& in, Address own, Address peer) : out_(out), in_(in), own_(own), peer_(peer) {}
        bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
            if (to != peer_ || out_.count == 64) return false;
            Datagram& slot = out_.slots[(out_.head + out_.count) % 64];
            slot.length = static_cast<std::uint16_t>(length);
            std::memcpy(slot.bytes, bytes, length);
            ++out_.count;
            return true;
        }
        std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
            if (in_.count == 0) return 0;
            const Datagram& slot = in_.slots[in_.head];
            in_.head = (in_.head + 1) % 64;
            --in_.count;
            if (slot.length > capacity) return 0;
            std::memcpy(buffer, slot.bytes, slot.length);
            from = peer_;
            return slot.length;
        }
        bool failed() const override { return false; }

    private:
        Ring& out_;
        Ring& in_;
        Address own_;
        Address peer_;
    };

    std::unique_ptr<network::ISocket> a() { return std::make_unique<Socket>(to_b, to_a, kServer, client_address(0)); }
    std::unique_ptr<network::ISocket> b() { return std::make_unique<Socket>(to_a, to_b, client_address(0), kServer); }
};

}  // namespace

TEST_CASE("N70 10,000 messages cross a 200 ms, 30 ms jitter, 5% loss link in order, exactly once", "[network][bench]") {
    network::LinkHub hub;
    int received = 0;
    bool in_order = true;
    EndpointCallbacks server_callbacks;
    server_callbacks.on_message = [&](PeerId, const std::uint8_t* bytes, std::size_t length) {
        std::uint32_t index;
        std::memcpy(&index, bytes, 4);
        if (length != 4 + index % 500 || index != static_cast<std::uint32_t>(received)) in_order = false;
        ++received;
    };
    Endpoint server(hub.open(kServer), config(Role::Server), server_callbacks);
    Endpoint client(hub.open(client_address(0)), config(Role::Client), EndpointCallbacks{});
    for (Endpoint* e : {&server, &client}) {
        e->conditioner().latency_seconds = 0.200;
        e->conditioner().jitter_seconds = 0.030;
        e->conditioner().loss = 0.05f;
    }
    client.connect(kServer, 0.0);
    std::vector<std::uint8_t> message;
    std::uint32_t sent = 0;
    double now = 0.0;
    for (int tick = 0; tick < 60 * 600 && received < 10000; ++tick) {
        now = tick / 60.0;
        server.receive(now);
        client.receive(now);
        if (client.state(0) == ConnectionState::Connected) {
            for (int i = 0; i < 8 && sent < 10000; ++i) {
                message.assign(4 + sent % 500, static_cast<std::uint8_t>(sent));
                std::memcpy(message.data(), &sent, 4);
                if (!client.send_message(0, message.data(), message.size())) break;
                ++sent;
            }
        }
        server.send(now);
        client.send(now);
    }
    REQUIRE(received == 10000);
    REQUIRE(in_order);
    REQUIRE(server.stats(0, now).rtt > 0.35);
}

TEST_CASE("N71 pumping a connected pair allocates nothing", "[network][bench]") {
    StaticLink link;
    int payloads = 0;
    int messages = 0;
    EndpointCallbacks callbacks;
    callbacks.on_payload = [&](PeerId, std::uint16_t, const std::uint8_t*, std::size_t) { ++payloads; };
    callbacks.on_message = [&](PeerId, const std::uint8_t*, std::size_t) { ++messages; };
    Endpoint server(link.a(), config(Role::Server, 1), callbacks);
    Endpoint client(link.b(), config(Role::Client), callbacks);
    client.connect(kServer, 0.0);
    const std::uint8_t state[64] = {};
    const std::uint8_t note[8] = {};
    auto tick = [&](double now) {
        server.receive(now);
        client.receive(now);
        if (client.state(0) == ConnectionState::Connected) {
            client.send_payload(0, state, sizeof(state), 1);
            server.send_payload(0, state, sizeof(state), 2);
            if (static_cast<int>(now * 60.0) % 10 == 0) {
                client.send_message(0, note, sizeof(note));
                server.send_message(0, note, sizeof(note));
            }
        }
        server.send(now);
        client.send(now);
    };
    for (int i = 0; i < 60; ++i) tick(i / 60.0);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    alloc_counter::arm();
    for (int i = 60; i < 180; ++i) tick(i / 60.0);
    const std::size_t allocations = alloc_counter::count();
    alloc_counter::disarm();
    REQUIRE(payloads > 200);
    REQUIRE(messages > 10);
    REQUIRE(allocations == 0);
}

TEST_CASE("N72 a server pumping 64 full connections at 60 Hz spends under 1 ms a tick", "[network][bench]") {
    network::LinkHub hub;
    Endpoint server(hub.open(kServer), config(Role::Server, 64), EndpointCallbacks{});
    std::vector<std::unique_ptr<Endpoint>> clients;
    for (int i = 0; i < 64; ++i) {
        clients.push_back(std::make_unique<Endpoint>(hub.open(client_address(i)), config(Role::Client), EndpointCallbacks{}));
        clients.back()->connect(kServer, 0.0);
    }
    std::vector<std::uint8_t> full(network::kMaxPayloadBytes - 1, 3);
    auto tick = [&](double now, bool full_packets) {
        for (auto& c : clients) c->receive(now);
        if (full_packets) {
            for (auto& c : clients) c->send_payload(0, full.data(), full.size(), 0);
        }
        for (auto& c : clients) c->send(now);
        const auto start = std::chrono::steady_clock::now();
        server.receive(now);
        if (full_packets) {
            for (PeerId p = 0; p < 64; ++p) server.send_payload(p, full.data(), full.size(), 0);
        }
        server.send(now);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    for (int i = 0; i < 60; ++i) tick(i / 60.0, false);
    REQUIRE(server.peers().size() == 64);
    for (int i = 60; i < 120; ++i) tick(i / 60.0, true);   // warm up
    double total = 0.0;
    for (int i = 120; i < 420; ++i) total += tick(i / 60.0, true);
    const double mean = total / 300.0;
    WARN("server tick with 64 full connections: " << mean << " ms");
#ifdef NDEBUG
    REQUIRE(mean < 1.0);
#endif
}
