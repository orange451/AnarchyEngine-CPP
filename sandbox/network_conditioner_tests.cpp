// Latency, jitter, loss, duplication, and a bandwidth cap on the way out
// (spec section 7).

#include "Conditioner.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

using network::Address;
using network::Conditioner;
using network::LinkHub;

namespace {

struct Pair {
    LinkHub hub;
    Conditioner a{hub.open(Address::loopback(1)), 7};
    std::unique_ptr<network::ISocket> b = hub.open(Address::loopback(2));
    std::uint8_t buffer[64];
    Address from;
    int drain() {
        int count = 0;
        while (b->receive(from, buffer, sizeof(buffer)) > 0) ++count;
        return count;
    }
};

const std::uint8_t kByte[1] = {42};

}  // namespace

TEST_CASE("N25 with every setting zero the conditioner passes datagrams straight through", "[network][conditioner]") {
    Pair pair;
    REQUIRE_FALSE(pair.a.settings.active());
    pair.a.advance(0.0);
    REQUIRE(pair.a.send(Address::loopback(2), kByte, 1));
    REQUIRE(pair.a.held() == 0);
    REQUIRE(pair.drain() == 1);
}

TEST_CASE("N26 latency holds a datagram until it is due", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.latency_seconds = 0.150;
    pair.a.advance(0.0);
    REQUIRE(pair.a.send(Address::loopback(2), kByte, 1));
    REQUIRE(pair.a.held() == 1);
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.100);
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.150);
    REQUIRE(pair.a.held() == 0);
    REQUIRE(pair.drain() == 1);
}

TEST_CASE("N27 jitter releases datagrams out of order within the window", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.latency_seconds = 0.100;
    pair.a.settings.jitter_seconds = 0.050;
    pair.a.advance(0.0);
    for (std::uint8_t i = 0; i < 40; ++i) {
        const std::uint8_t byte[1] = {i};
        pair.a.send(Address::loopback(2), byte, 1);
    }
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.049);
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.151);
    bool reordered = false;
    std::uint8_t previous = 0;
    int count = 0;
    while (pair.b->receive(pair.from, pair.buffer, sizeof(pair.buffer)) > 0) {
        if (count > 0 && pair.buffer[0] < previous) reordered = true;
        previous = pair.buffer[0];
        ++count;
    }
    REQUIRE(count == 40);
    REQUIRE(reordered);
}

TEST_CASE("N28 loss and duplication hit their rates over many datagrams", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.loss = 0.25f;
    pair.a.advance(0.0);
    for (int i = 0; i < 4000; ++i) pair.a.send(Address::loopback(2), kByte, 1);
    const int delivered = pair.drain();
    REQUIRE(delivered > 2800);
    REQUIRE(delivered < 3200);
    pair.a.settings.loss = 0.0f;
    pair.a.settings.duplicate = 0.25f;
    for (int i = 0; i < 4000; ++i) pair.a.send(Address::loopback(2), kByte, 1);
    const int doubled = pair.drain();
    REQUIRE(doubled > 4800);
    REQUIRE(doubled < 5200);
}

TEST_CASE("N29 a bandwidth cap drops what exceeds it each second", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.bandwidth_bytes_per_second = 100.0;
    std::uint8_t ten[10] = {};
    pair.a.advance(0.0);
    int accepted = 0;
    for (int i = 0; i < 20; ++i) accepted += pair.a.send(Address::loopback(2), ten, 10) ? 1 : 0;
    REQUIRE(accepted == 10);
    REQUIRE(pair.drain() == 10);
    pair.a.advance(1.0);
    REQUIRE(pair.a.send(Address::loopback(2), ten, 10));
}
