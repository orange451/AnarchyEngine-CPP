// Ordered, exactly-once messages and blocks, sent until acked (spec section 2).

#include "Bits.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"
#include "ReliableChannel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using network::BitReader;
using network::BitWriter;
using network::MessageRef;
using network::PacketRing;
using network::ReliableChannel;
using network::SentPacket;

namespace {

struct Packet {
    std::uint16_t sequence;
    std::vector<std::uint8_t> bytes;
};

// One side of a link: a channel and the ring that tracks its packets.
struct Side {
    ReliableChannel channel;
    PacketRing ring;
    std::uint16_t next_sequence = 0;
    std::vector<std::vector<std::uint8_t>> received;

    Packet build(double now, double rtt) {
        Packet packet;
        packet.sequence = next_sequence++;
        packet.bytes.resize(network::kMaxPayloadBytes);
        BitWriter writer(packet.bytes.data(), packet.bytes.size());
        SentPacket& sent = ring.record_sent(packet.sequence, now);
        channel.write(writer, now, rtt, sent);
        REQUIRE_FALSE(writer.overflowed());
        packet.bytes.resize(writer.bytes_written());
        return packet;
    }
    void take(const Packet& packet) {
        REQUIRE(ring.note_received(packet.sequence) == PacketRing::Receive::Accepted);
        BitReader reader(packet.bytes.data(), packet.bytes.size());
        REQUIRE(channel.read(reader));
        channel.deliver([&](const std::uint8_t* data, std::size_t length) { received.emplace_back(data, data + length); });
    }
    // Learns from the other side's ack state which of our packets arrived.
    void learn_acks(const Side& other, double now) {
        ring.process_acks(other.ring.ack(), other.ring.ack_bits(), now, [&](SentPacket& sent, double) {
            for (int i = 0; i < sent.ref_count; ++i) channel.acked(sent.refs[i]);
        });
    }
};

std::vector<std::uint8_t> message(std::uint32_t seed, std::size_t length) {
    std::vector<std::uint8_t> bytes(length);
    std::mt19937 random(seed);
    for (std::uint8_t& b : bytes) b = static_cast<std::uint8_t>(random());
    return bytes;
}

}  // namespace

TEST_CASE("N30 an empty message and a full one round-trip in order; one over 16 MB is refused", "[network][reliable]") {
    Side a, b;
    const std::vector<std::uint8_t> full = message(1, 1000);
    REQUIRE(a.channel.send(nullptr, 0));
    REQUIRE(a.channel.send(full.data(), full.size()));
    REQUIRE_FALSE(a.channel.send(full.data(), (16u << 20) + 1));
    REQUIRE(a.channel.unacked() == 2);
    b.take(a.build(0.0, 0.1));
    REQUIRE(b.received.size() == 2);
    REQUIRE(b.received[0].empty());
    REQUIRE(b.received[1] == full);
    a.learn_acks(b, 0.05);
    REQUIRE(a.channel.unacked() == 0);
}

TEST_CASE("N31 the send window holds 256 messages and refuses the 257th until one is acked", "[network][reliable]") {
    Side a, b;
    const std::uint8_t byte = 1;
    for (int i = 0; i < 256; ++i) REQUIRE(a.channel.send(&byte, 1));
    REQUIRE_FALSE(a.channel.send(&byte, 1));
    b.take(a.build(0.0, 0.1));   // the first 64 fit in one packet
    a.learn_acks(b, 0.01);
    REQUIRE(a.channel.send(&byte, 1));
}

TEST_CASE("N32 messages arrive in order, exactly once, under 30% loss and reordering", "[network][reliable]") {
    Side a, b;
    std::mt19937 random(3);
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<Packet> in_flight;
    const double rtt = 0.05;
    double now = 0.0;
    int next_message = 0;
    for (int tick = 0; tick < 2000 && (b.received.size() < 500); ++tick) {
        now = tick / 60.0;
        if (next_message < 500 && tick % 2 == 0) {
            sent.push_back(message(static_cast<std::uint32_t>(next_message), 1 + next_message % 200));
            if (a.channel.send(sent.back().data(), sent.back().size())) ++next_message;
            else sent.pop_back();
        }
        Packet packet = a.build(now, rtt);
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(random) >= 0.3f) in_flight.push_back(std::move(packet));
        // Deliver in a shuffled order, a few per tick.
        std::shuffle(in_flight.begin(), in_flight.end(), random);
        for (int d = 0; d < 2 && !in_flight.empty(); ++d) {
            b.take(in_flight.back());
            in_flight.pop_back();
        }
        a.learn_acks(b, now);
    }
    REQUIRE(b.received.size() == 500);
    for (int i = 0; i < 500; ++i) REQUIRE(b.received[i] == sent[i]);
}

TEST_CASE("N33 a 100 KB block arrives whole under 10% loss", "[network][reliable]") {
    Side a, b;
    std::mt19937 random(5);
    const std::vector<std::uint8_t> block = message(9, 100 * 1024);
    REQUIRE(a.channel.send(block.data(), block.size()));
    for (int tick = 0; tick < 3000 && b.received.empty(); ++tick) {
        const double now = tick / 60.0;
        Packet packet = a.build(now, 0.05);
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(random) >= 0.1f) b.take(packet);
        a.learn_acks(b, now);
    }
    REQUIRE(b.received.size() == 1);
    REQUIRE(b.received[0] == block);
    REQUIRE(a.channel.unacked() == 0);
}

TEST_CASE("N34 a message is resent only after RTT times 1.25", "[network][reliable]") {
    Side a;
    const std::uint8_t byte = 7;
    REQUIRE(a.channel.send(&byte, 1));
    REQUIRE(a.channel.has_due(0.0, 0.1));
    REQUIRE(a.build(0.0, 0.1).bytes.size() > 1);
    REQUIRE_FALSE(a.channel.has_due(0.05, 0.1));
    REQUIRE(a.build(0.05, 0.1).bytes.size() == 1);   // just the terminating bit
    REQUIRE(a.channel.has_due(0.13, 0.1));
    REQUIRE(a.build(0.13, 0.1).bytes.size() > 1);
}

TEST_CASE("N35 a packet carries at most 64 entries and never overflows its space", "[network][reliable]") {
    Side a, b;
    const std::vector<std::uint8_t> ten = message(2, 10);
    for (int i = 0; i < 200; ++i) REQUIRE(a.channel.send(ten.data(), ten.size()));
    const Packet first = a.build(0.0, 0.1);
    REQUIRE(first.bytes.size() <= network::kMaxPayloadBytes);
    b.take(first);
    REQUIRE(b.received.size() == 64);
    // Big messages: as many 1000-byte entries as fit, which is one.
    Side c, d;
    const std::vector<std::uint8_t> big = message(4, 1000);
    for (int i = 0; i < 3; ++i) REQUIRE(c.channel.send(big.data(), big.size()));
    d.take(c.build(0.0, 0.1));
    REQUIRE(d.received.size() == 1);
}

TEST_CASE("N36 a malformed section is refused and bytes_due counts what is waiting", "[network][reliable]") {
    ReliableChannel channel;
    REQUIRE_FALSE(channel.has_due(0.0, 0.1));
    REQUIRE(channel.bytes_due(0.0, 0.1) == 0);
    const std::uint8_t byte = 1;
    REQUIRE(channel.send(&byte, 1));
    REQUIRE(channel.bytes_due(0.0, 0.1) >= 1);
    // An entry that claims 1,001 bytes.
    std::uint8_t buffer[16];
    BitWriter writer(buffer, sizeof(buffer));
    writer.boolean(true);
    writer.bits(0, 16);
    writer.bits(0, 16);
    writer.bits(1001, 16);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE_FALSE(channel.read(reader));
    // An entry cut off before its bytes.
    BitWriter cut(buffer, sizeof(buffer));
    cut.boolean(true);
    cut.bits(0, 16);
    cut.bits(0, 16);
    cut.bits(100, 16);
    BitReader short_reader(buffer, cut.bytes_written());
    REQUIRE_FALSE(channel.read(short_reader));
}
