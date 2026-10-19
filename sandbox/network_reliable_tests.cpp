// Ordered, exactly-once messages and blocks, sent until acked (spec section 2).

#include "Bits.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"
#include "ReliableChannel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
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

namespace {

// Appends one hand-built entry; the bytes are zeros.
void put_entry(BitWriter& writer, std::uint16_t id, std::uint16_t fragment_count, std::uint16_t index, std::size_t length) {
    writer.boolean(true);
    writer.bits(id, 16);
    writer.bits(fragment_count, 16);
    if (fragment_count > 0) writer.bits(index, 16);
    writer.bits(static_cast<std::uint32_t>(length), 16);
    writer.align();
    const std::vector<std::uint8_t> zeros(length, 0);
    writer.bytes(zeros.data(), length);
}

}  // namespace

TEST_CASE("N37a the largest block validates and one fragment more does not", "[network][reliable]") {
    ReliableChannel channel;
    std::vector<std::uint8_t> buffer(1100);
    BitWriter ok(buffer.data(), buffer.size());
    put_entry(ok, 0, 16778, 0, 1000);
    ok.boolean(false);
    REQUIRE(channel.validate(BitReader(buffer.data(), ok.bytes_written())));
    BitWriter over(buffer.data(), buffer.size());
    put_entry(over, 0, 16779, 0, 1000);
    over.boolean(false);
    REQUIRE_FALSE(channel.validate(BitReader(buffer.data(), over.bytes_written())));
}

TEST_CASE("N37b blocks not yet next in line cannot hold more than 8 MB; the next one is exempt", "[network][reliable]") {
    ReliableChannel channel;
    std::vector<std::uint8_t> buffer(4000);
    BitWriter many(buffer.data(), buffer.size());
    for (std::uint16_t id = 1; id <= 3; ++id) put_entry(many, id, 4000, 0, 1000);
    many.boolean(false);
    BitReader reader(buffer.data(), many.bytes_written());
    REQUIRE_FALSE(channel.validate(reader));
    REQUIRE_FALSE(channel.read(reader));
    int delivered = 0;
    channel.deliver([&](const std::uint8_t*, std::size_t) { ++delivered; });
    REQUIRE(delivered == 0);
    BitWriter next(buffer.data(), buffer.size());
    put_entry(next, 0, 16778, 0, 1000);
    next.boolean(false);
    BitReader next_reader(buffer.data(), next.bytes_written());
    REQUIRE(channel.validate(next_reader));
    REQUIRE(channel.read(next_reader));
}

TEST_CASE("N37c a one-fragment block and an empty last fragment are refused", "[network][reliable]") {
    ReliableChannel channel;
    std::vector<std::uint8_t> buffer(1100);
    BitWriter one(buffer.data(), buffer.size());
    put_entry(one, 0, 1, 0, 10);
    one.boolean(false);
    REQUIRE_FALSE(channel.validate(BitReader(buffer.data(), one.bytes_written())));
    BitWriter empty(buffer.data(), buffer.size());
    put_entry(empty, 0, 3, 2, 0);
    empty.boolean(false);
    REQUIRE_FALSE(channel.validate(BitReader(buffer.data(), empty.bytes_written())));
}

TEST_CASE("N37d blocks queued behind one another all arrive, in order, within the receiver's budget", "[network][reliable]") {
    Side a, b;
    std::vector<std::vector<std::uint8_t>> sent;
    sent.push_back(message(11, 5u << 20));
    sent.push_back(message(12, 5u << 20));
    sent.push_back(message(13, 10));
    sent.push_back(message(14, 5u << 20));
    sent.push_back(message(15, 20));
    for (const auto& m : sent) REQUIRE(a.channel.send(m.data(), m.size()));
    for (int tick = 0; tick < 60 * 600 && b.received.size() < sent.size(); ++tick) {
        const double now = tick / 60.0;
        b.take(a.build(now, 0.05));
        a.learn_acks(b, now);
    }
    REQUIRE(b.received.size() == sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) REQUIRE(b.received[i] == sent[i]);
}

TEST_CASE("N38 one shared block goes to two channels without a copy each, and both receive it whole", "[network][reliable]") {
    Side a, b, c, d;
    const auto block = std::make_shared<const std::vector<std::uint8_t>>(message(11, 5 * 1000 + 17));
    REQUIRE(a.channel.send(block));
    REQUIRE(c.channel.send(block));
    REQUIRE(block.use_count() == 3);   // both channels hold the one buffer
    // A message that fits inline is copied, so the channel holds no reference to it.
    const auto small = std::make_shared<const std::vector<std::uint8_t>>(message(12, 40));
    REQUIRE(a.channel.send(small));
    REQUIRE(small.use_count() == 1);
    REQUIRE_FALSE(a.channel.send(network::SharedMessage{}));
    for (int tick = 0; tick < 200 && (b.received.size() < 2 || d.received.empty()); ++tick) {
        const double now = tick / 60.0;
        b.take(a.build(now, 0.05));
        a.learn_acks(b, now);
        d.take(c.build(now, 0.05));
        c.learn_acks(d, now);
    }
    REQUIRE(b.received.size() == 2);
    REQUIRE(b.received[0] == *block);
    REQUIRE(b.received[1] == *small);
    REQUIRE(d.received.size() == 1);
    REQUIRE(d.received[0] == *block);
    REQUIRE(block.use_count() == 1);   // acked, the channels let it go
}

TEST_CASE("N77 has_due and write start at the first open fragment", "[network][reliable]") {
    Side a;
    const std::vector<std::uint8_t> block = message(11, 50 * network::kFragmentBytes);
    REQUIRE(a.channel.send(block.data(), block.size()));
    // One fragment entry: its header bits, alignment, a full fragment, and the terminating bit.
    const std::size_t entry = (1 + 16 + 16 + 16 + 16 + 7 + network::kFragmentBytes * 8 + 1) / 8;
    REQUIRE(a.channel.bytes_due(0.0, 0.1) == 50 * entry);
    for (std::uint16_t f = 0; f < 40; ++f) a.channel.acked(MessageRef{0, f});
    REQUIRE(a.channel.first_open_fragment(0) == 40);
    REQUIRE(a.channel.has_due(0.0, 0.1));
    REQUIRE(a.channel.bytes_due(0.0, 0.1) == 10 * entry);
    SentPacket& sent = a.ring.record_sent(a.next_sequence++, 0.0);
    std::vector<std::uint8_t> bytes(network::kMaxPayloadBytes);
    BitWriter writer(bytes.data(), bytes.size());
    a.channel.write(writer, 0.0, 0.1, sent);
    REQUIRE(sent.ref_count >= 1);
    REQUIRE(sent.refs[0].id == 0);
    REQUIRE(sent.refs[0].fragment == 40);
    // An ack out of order leaves the cursor at the lowest fragment still open.
    a.channel.acked(MessageRef{0, 45});
    REQUIRE(a.channel.first_open_fragment(0) == 40);
    a.channel.acked(MessageRef{0, 40});
    REQUIRE(a.channel.first_open_fragment(0) == 41);
}
