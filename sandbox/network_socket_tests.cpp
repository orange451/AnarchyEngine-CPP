// Addresses, the in-process link, and the real UDP socket (spec sections 6 and 7).

#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

using network::Address;
using network::LinkHub;
using network::UdpSocket;

namespace {

// Polls a socket for up to half a second, since loopback delivery is not instant.
std::size_t wait_receive(network::ISocket& socket, Address& from, std::uint8_t* buffer, std::size_t capacity) {
    for (int i = 0; i < 100; ++i) {
        const std::size_t got = socket.receive(from, buffer, capacity);
        if (got > 0) return got;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

}  // namespace

TEST_CASE("N20 addresses parse, print, and compare", "[network][socket]") {
    const auto v4 = Address::parse("192.168.1.20:7777");
    REQUIRE(v4.has_value());
    REQUIRE(v4->family == Address::Family::IPv4);
    REQUIRE(v4->port == 7777);
    REQUIRE(v4->to_string() == "192.168.1.20:7777");
    REQUIRE(*v4 == Address::ipv4(192, 168, 1, 20, 7777));
    REQUIRE(*v4 != Address::ipv4(192, 168, 1, 20, 7778));
    const auto v6 = Address::parse("[::1]:9000");
    REQUIRE(v6.has_value());
    REQUIRE(v6->family == Address::Family::IPv6);
    REQUIRE(v6->port == 9000);
    REQUIRE(v6->to_string() == "[::1]:9000");
    REQUIRE_FALSE(Address::parse("example.com:1").has_value());
    REQUIRE_FALSE(Address::parse("1.2.3.4").has_value());
    REQUIRE_FALSE(Address::parse("1.2.3.4:70000").has_value());
    REQUIRE(Address::loopback(5) == Address::ipv4(127, 0, 0, 1, 5));
    for (const char* bad : {"4294967297.0.0.1:1", "256.1.1.1:1", "1.2.3:1", "1.2.3.4.5:1", "1..3.4:1", "1.2.3.4:"}) {
        CAPTURE(bad);
        REQUIRE_FALSE(Address::parse(bad).has_value());
    }
    for (const char* text : {"[::]:1", "[1::]:2", "[fe80::1:2]:3"}) {
        CAPTURE(text);
        const auto parsed = Address::parse(text);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->to_string() == text);
    }
}

TEST_CASE("N21 the link hub delivers datagrams by address, in order, and nothing to a stranger", "[network][socket]") {
    LinkHub hub;
    auto a = hub.open(Address::loopback(1));
    auto b = hub.open(Address::loopback(2));
    const std::uint8_t one[3] = {1, 2, 3};
    const std::uint8_t two[2] = {9, 9};
    REQUIRE(a->send(Address::loopback(2), one, 3));
    REQUIRE(a->send(Address::loopback(2), two, 2));
    REQUIRE_FALSE(a->send(Address::loopback(3), one, 3));   // no such socket
    REQUIRE(hub.pending() == 2);
    Address from;
    std::uint8_t buffer[16];
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 3);
    REQUIRE(from == Address::loopback(1));
    REQUIRE(std::memcmp(buffer, one, 3) == 0);
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 2);
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE(a->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE_FALSE(a->failed());
}

TEST_CASE("N22 a datagram larger than the receive buffer is dropped by the link, not truncated", "[network][socket]") {
    LinkHub hub;
    auto a = hub.open(Address::loopback(1));
    auto b = hub.open(Address::loopback(2));
    std::uint8_t big[1500] = {};
    REQUIRE(a->send(Address::loopback(2), big, sizeof(big)));
    Address from;
    std::uint8_t buffer[1200];
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE(hub.pending() == 0);
}

TEST_CASE("N23 a real UDP socket sends to itself over loopback and drops an oversized datagram", "[network][socket]") {
    std::string error;
    auto a = UdpSocket::open(0, error);
    if (!a) {
        WARN("no socket: " << error);
        return;
    }
    auto b = UdpSocket::open(0, error);
    REQUIRE(b);
    REQUIRE(a->port() != 0);
    REQUIRE(b->port() != 0);
    const std::uint8_t data[5] = {5, 4, 3, 2, 1};
    REQUIRE(a->send(Address::loopback(b->port()), data, 5));
    Address from;
    std::uint8_t buffer[1200];
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 5);
    REQUIRE(from.port == a->port());
    REQUIRE(std::memcmp(buffer, data, 5) == 0);
    REQUIRE_FALSE(a->failed());
    std::uint8_t big[1500] = {};
    REQUIRE(a->send(Address::loopback(b->port()), big, sizeof(big)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE_FALSE(b->failed());
    // The socket goes on working after the drop.
    REQUIRE(a->send(Address::loopback(b->port()), data, 5));
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 5);
}

TEST_CASE("N24 opening a port twice fails with a message", "[network][socket]") {
    std::string error;
    auto a = UdpSocket::open(0, error);
    if (!a) return;
    auto b = UdpSocket::open(a->port(), error);
    REQUIRE_FALSE(b);
    REQUIRE_FALSE(error.empty());
}
