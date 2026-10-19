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

TEST_CASE("N25 the link skips a zero-length datagram instead of reporting nothing waiting", "[network][socket]") {
    LinkHub hub;
    auto a = hub.open(Address::loopback(1));
    auto b = hub.open(Address::loopback(2));
    const std::uint8_t three[3] = {7, 8, 9};
    REQUIRE(a->send(Address::loopback(2), three, 0));
    REQUIRE(a->send(Address::loopback(2), three, 3));
    Address from;
    std::uint8_t buffer[16];
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 3);
    REQUIRE(std::memcmp(buffer, three, 3) == 0);
}

TEST_CASE("N26 a real UDP socket skips a zero-length datagram", "[network][socket]") {
    std::string error;
    auto a = UdpSocket::open(0, error);
    if (!a) {
        WARN("no socket: " << error);
        return;
    }
    auto b = UdpSocket::open(0, error);
    REQUIRE(b);
    const std::uint8_t three[3] = {7, 8, 9};
    REQUIRE(a->send(Address::loopback(b->port()), three, 0));
    REQUIRE(a->send(Address::loopback(b->port()), three, 3));
    Address from;
    std::uint8_t buffer[16];
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 3);
    REQUIRE(std::memcmp(buffer, three, 3) == 0);
}

TEST_CASE("N80 resolve takes literals and names", "[network][socket]") {
    std::string error;
    const auto literal = network::resolve("127.0.0.1:7777", error);
    REQUIRE(literal.has_value());
    REQUIRE(*literal == Address::loopback(7777));
    const auto bracketed = network::resolve("[::1]:7777", error);
    REQUIRE(bracketed.has_value());
    REQUIRE(bracketed->family == Address::Family::IPv6);
    const auto named = network::resolve("localhost:7777", error);
    REQUIRE(named.has_value());
    REQUIRE(named->port == 7777);
    REQUIRE((named->family == Address::Family::IPv4 || named->family == Address::Family::IPv6));
    error.clear();
    REQUIRE_FALSE(network::resolve("no-such-host.invalid:7777", error).has_value());
    REQUIRE_FALSE(error.empty());
    error.clear();
    REQUIRE_FALSE(network::resolve("localhost", error).has_value());
    REQUIRE_FALSE(error.empty());
    error.clear();
    REQUIRE_FALSE(network::resolve("localhost:99999", error).has_value());
    REQUIRE_FALSE(error.empty());
    error.clear();
    REQUIRE_FALSE(network::resolve(":7777", error).has_value());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("N82 an IPv4-only socket sends and receives on loopback", "[network][socket]") {
    std::string error;
    auto a = UdpSocket::open_ipv4(0, error);
    REQUIRE(a);
    auto b = UdpSocket::open_ipv4(0, error);
    REQUIRE(b);
    REQUIRE(a->port() != 0);
    const std::uint8_t data[4] = {1, 2, 3, 4};
    REQUIRE(a->send(Address::loopback(b->port()), data, 4));
    Address from;
    std::uint8_t buffer[16];
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 4);
    REQUIRE(from == Address::loopback(a->port()));
    REQUIRE(std::memcmp(buffer, data, 4) == 0);
    // An IPv6 address cannot be reached from an IPv4 socket: the send fails, the socket does not.
    REQUIRE_FALSE(a->send(*Address::parse("[::1]:5"), data, 4));
    REQUIRE_FALSE(a->failed());
    REQUIRE(a->send(Address::loopback(b->port()), data, 4));
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 4);
}
