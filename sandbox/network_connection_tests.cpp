// One peer's state: packets out and in, acks, RTT, the payload budget
// (spec sections 1 to 4).

#include "Bits.hpp"
#include "Connection.hpp"
#include "Packet.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using network::BitReader;
using network::Connection;
using network::PacketHeader;
using network::PacketType;

namespace {

struct Peer {
    Connection c{32000.0, 1.0 / 60.0};
    std::vector<std::vector<std::uint8_t>> messages;
    std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> payloads;
    std::vector<std::pair<std::uint16_t, std::uint32_t>> delivered;
    Peer() {
        c.callbacks.on_message = [this](const std::uint8_t* d, std::size_t n) { messages.emplace_back(d, d + n); };
        c.callbacks.on_payload = [this](std::uint16_t s, const std::uint8_t* d, std::size_t n) {
            payloads.emplace_back(s, std::vector<std::uint8_t>(d, d + n));
        };
        c.callbacks.on_delivered = [this](std::uint16_t s, std::uint32_t tag) { delivered.emplace_back(s, tag); };
    }
};

// Reads the header the way Endpoint will, then hands the rest to the connection.
bool feed(Peer& to, std::uint8_t* bytes, std::size_t length, double now) {
    length = network::decode_packet(bytes, length);
    BitReader reader(bytes, length);
    PacketHeader header;
    REQUIRE(network::read_header(reader, header));
    return to.c.read_packet(header, reader, bytes, length, now);
}

PacketType exchange(Peer& from, Peer& to, double now) {
    std::uint8_t buffer[network::kMaxPacketBytes];
    const std::size_t length = from.c.write_packet(buffer, sizeof(buffer), now);
    REQUIRE(length >= network::kHeaderBytes + 1);
    const PacketType type = static_cast<PacketType>(buffer[2]);
    feed(to, buffer, length, now);
    return type;
}

}  // namespace

TEST_CASE("N44 an ack that arrives before its packet's send time leaves RTT at zero, not negative", "[network][connection]") {
    Peer a, b;
    exchange(a, b, 1.0);
    // b's reply acks a's packet, delivered with a clock that went backwards.
    exchange(b, a, 0.5);
    REQUIRE(a.c.rtt() >= 0.0);
    REQUIRE(a.c.rtt() == Approx(0.0));
}

TEST_CASE("N45 a message, a payload, and its delivery report cross a round trip", "[network][connection]") {
    Peer a, b;
    const std::uint8_t hello[5] = {'h', 'e', 'l', 'l', 'o'};
    const std::uint8_t state[3] = {1, 2, 3};
    REQUIRE(a.c.send_message(hello, 5));
    const auto sequence = a.c.set_payload(state, 3, 77);
    REQUIRE(sequence.has_value());
    REQUIRE(exchange(a, b, 0.0) == PacketType::Payload);
    REQUIRE(b.messages.size() == 1);
    REQUIRE(std::memcmp(b.messages[0].data(), hello, 5) == 0);
    REQUIRE(b.payloads.size() == 1);
    REQUIRE(b.payloads[0].first == *sequence);
    REQUIRE(b.payloads[0].second == std::vector<std::uint8_t>{1, 2, 3});
    REQUIRE(a.delivered.empty());
    exchange(b, a, 0.04);
    REQUIRE(a.delivered.size() == 1);
    REQUIRE(a.delivered[0].first == *sequence);
    REQUIRE(a.delivered[0].second == 77);
    REQUIRE(a.c.rtt() == Approx(0.04));
    // A keep-alive from b carried nothing to report.
    REQUIRE(b.delivered.empty());
}

TEST_CASE("N46 a quiet connection wants a keep-alive every 100 ms and nothing sooner", "[network][connection]") {
    Peer a;
    REQUIRE(a.c.wants_packet(0.0));   // the first packet
    std::uint8_t buffer[network::kMaxPacketBytes];
    REQUIRE(a.c.write_packet(buffer, sizeof(buffer), 0.0) == network::kHeaderBytes + 1);
    REQUIRE(static_cast<PacketType>(buffer[2]) == PacketType::KeepAlive);
    REQUIRE_FALSE(a.c.wants_packet(0.05));
    REQUIRE(a.c.wants_packet(0.1));
    REQUIRE(a.c.wants_packet(6.0 / 60.0));
    const std::uint8_t byte = 1;
    a.c.send_message(&byte, 1);
    REQUIRE(a.c.wants_packet(0.0));   // a message is due at once
}

TEST_CASE("N47 a replayed packet is dropped and counted", "[network][connection]") {
    Peer a, b;
    std::uint8_t buffer[network::kMaxPacketBytes];
    const std::size_t length = a.c.write_packet(buffer, sizeof(buffer), 0.0);
    std::uint8_t copy[network::kMaxPacketBytes];
    std::memcpy(copy, buffer, length);
    REQUIRE(feed(b, buffer, length, 0.0));
    REQUIRE_FALSE(feed(b, copy, length, 0.01));
    REQUIRE(b.c.stats(0.01).dropped_replay == 1);
    REQUIRE(b.c.stats(0.01).packets_received == 1);
}

TEST_CASE("N48 a malformed reliable section drops the packet without marking its sequence received", "[network][connection]") {
    Peer a, b;
    const std::uint8_t byte = 9;
    a.c.send_message(&byte, 1);
    std::uint8_t buffer[network::kMaxPacketBytes];
    const std::size_t length = a.c.write_packet(buffer, sizeof(buffer), 0.0);
    // Corrupt the entry's length field (bits 49..64 after the 13-byte header and the continuation bit).
    std::uint8_t broken[network::kMaxPacketBytes];
    std::memcpy(broken, buffer, length);
    broken[network::kHeaderBytes + 4] = 0xFF;
    broken[network::kHeaderBytes + 5] = 0xFF;
    REQUIRE_FALSE(feed(b, broken, length, 0.0));
    REQUIRE(b.c.stats(0.0).dropped_malformed == 1);
    REQUIRE(b.messages.empty());
    // The good copy of the same sequence is still accepted.
    REQUIRE(feed(b, buffer, length, 0.01));
    REQUIRE(b.messages.size() == 1);
}

TEST_CASE("N49 the payload budget is the tick's share of the rate, less what the reliable channel needs", "[network][connection]") {
    Peer a;
    REQUIRE(a.c.payload_budget(0.0) == 533);   // 32000 / 60
    std::vector<std::uint8_t> message(100, 1);
    a.c.send_message(message.data(), message.size());
    REQUIRE(a.c.payload_budget(0.0) == 533);   // min(1186 - 107, 533)
    Peer blocked;
    std::vector<std::uint8_t> block(100 * 1024, 3);
    REQUIRE(blocked.c.send_message(block.data(), block.size()));
    REQUIRE(blocked.c.payload_budget(0.0) > 0);
    REQUIRE(blocked.c.payload_budget(0.0) <= 1186 - 1000);
    Connection fast(10'000'000.0, 1.0 / 60.0);
    REQUIRE(fast.payload_budget(0.0) == network::kMaxPayloadBytes - 1);
}

TEST_CASE("N50 stats count packets and bytes per second", "[network][connection]") {
    Peer a, b;
    // Both directions, so a's packets are acked and its budget stays full.
    for (int i = 0; i < 60; ++i) {
        exchange(a, b, i / 60.0);
        exchange(b, a, i / 60.0);
    }
    exchange(a, b, 1.0);
    exchange(b, a, 1.0);
    REQUIRE(a.c.stats(1.0).packets_sent == 61);
    REQUIRE(b.c.stats(1.0).packets_received == 61);
    REQUIRE(a.c.stats(1.0).bytes_out_per_second == Approx(60.0 * 14.0).epsilon(0.05));
    REQUIRE(b.c.stats(1.0).bytes_in_per_second == Approx(60.0 * 14.0).epsilon(0.05));
    REQUIRE(a.c.stats(1.0).budget_bytes_per_second == Approx(32000.0));
}

TEST_CASE("N51 a payload keeps its room beside a full reliable section, and oversized or doubled payloads are refused", "[network][connection]") {
    Peer a, b;
    std::vector<std::uint8_t> big(1000, 5);
    std::vector<std::uint8_t> state(1000, 6);
    REQUIRE(a.c.send_message(big.data(), big.size()));
    REQUIRE(a.c.set_payload(state.data(), state.size(), 1).has_value());
    REQUIRE_FALSE(a.c.set_payload(state.data(), state.size(), 2).has_value());   // one per packet
    exchange(a, b, 0.0);
    REQUIRE(b.payloads.size() == 1);
    REQUIRE(b.payloads[0].second == state);
    REQUIRE(b.messages.empty());   // the message did not fit beside it and waits
    exchange(a, b, 0.2);
    REQUIRE(b.messages.size() == 1);
    REQUIRE(b.messages[0] == big);
    std::vector<std::uint8_t> huge(network::kMaxPayloadBytes + 1, 0);
    REQUIRE_FALSE(a.c.set_payload(huge.data(), huge.size(), 3).has_value());
}

TEST_CASE("N51b a packet's reliable messages land before its payload", "[network][connection]") {
    Peer a, b;
    std::string order;
    b.c.callbacks.on_message = [&](const std::uint8_t*, std::size_t) { order += "m"; };
    b.c.callbacks.on_payload = [&](std::uint16_t, const std::uint8_t*, std::size_t) { order += "p"; };
    const std::uint8_t byte = 1;
    REQUIRE(a.c.send_message(&byte, 1));
    REQUIRE(a.c.set_payload(&byte, 1, 0).has_value());
    exchange(a, b, 0.0);
    REQUIRE(order == "mp");
}
