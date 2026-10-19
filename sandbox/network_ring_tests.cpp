// The packet header, and the rings that turn piggybacked acks into per-packet
// delivery (spec sections 1 and 4).

#include "Bits.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

using Catch::Approx;
using network::AckBits;
using network::BitReader;
using network::BitWriter;
using network::PacketHeader;
using network::PacketRing;
using network::PacketType;
using network::SentPacket;

namespace {

// Ack bits with these bits set.
AckBits bits(std::initializer_list<int> set) {
    AckBits result;
    for (const int n : set) result.set(n);
    return result;
}

// Ack bits whose lowest 32 are word.
AckBits low(std::uint32_t word) {
    AckBits result;
    result.words[0] = word;
    return result;
}

}  // namespace

TEST_CASE("N10 the header is 25 bytes and round-trips, all 128 ack bits with it", "[network][ring]") {
    std::uint8_t buffer[network::kMaxPacketBytes];
    BitWriter writer(buffer, sizeof(buffer));
    PacketHeader in{network::kProtocolId, PacketType::Payload, 0xBEEF, 1234, 1230, AckBits{}};
    in.ack_bits.set(0);
    in.ack_bits.set(31);
    in.ack_bits.set(64);
    in.ack_bits.set(127);
    network::write_header(writer, in);
    REQUIRE(writer.bytes_written() == network::kHeaderBytes);
    PacketHeader out{};
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(network::read_header(reader, out));
    REQUIRE(out.type == PacketType::Payload);
    REQUIRE(out.session == 0xBEEF);
    REQUIRE(out.sequence == 1234);
    REQUIRE(out.ack == 1230);
    REQUIRE(out.ack_bits == in.ack_bits);
    REQUIRE(out.ack_bits.test(127));
    REQUIRE_FALSE(out.ack_bits.test(126));
}

TEST_CASE("N11 a wrong protocol id or a truncated header is refused", "[network][ring]") {
    std::uint8_t buffer[network::kMaxPacketBytes];
    BitWriter writer(buffer, sizeof(buffer));
    network::write_header(writer, PacketHeader{0x1234, PacketType::Payload, 0, 0, 0, AckBits{}});
    PacketHeader out{};
    BitReader wrong(buffer, writer.bytes_written());
    REQUIRE_FALSE(network::read_header(wrong, out));
    BitReader truncated(buffer, 5);
    REQUIRE_FALSE(network::read_header(truncated, out));
}

TEST_CASE("N12 encode and decode are the identity today", "[network][ring]") {
    std::uint8_t buffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(network::encode_packet(buffer, 8, sizeof(buffer)) == 8);
    REQUIRE(network::decode_packet(buffer, 8) == 8);
    REQUIRE(buffer[0] == 1);
    REQUIRE(buffer[7] == 8);
}

TEST_CASE("N13 received sequences make the ack and the bitfield", "[network][ring]") {
    PacketRing ring;
    REQUIRE_FALSE(ring.any_received());
    REQUIRE(ring.ack() == 0xFFFF);
    REQUIRE(ring.ack_bits() == AckBits{});
    REQUIRE(ring.note_received(100) == PacketRing::Receive::Accepted);
    REQUIRE(ring.note_received(98) == PacketRing::Receive::Accepted);
    REQUIRE(ring.note_received(103) == PacketRing::Receive::Accepted);
    REQUIRE(ring.ack() == 103);
    // bit n: sequence ack - n - 1. 102 no, 101 no, 100 yes (bit 2), 99 no, 98 yes (bit 4).
    REQUIRE(ring.ack_bits() == bits({2, 4}));
    REQUIRE(ring.note_received(100) == PacketRing::Receive::Replay);
    REQUIRE(ring.note_received(103) == PacketRing::Receive::Replay);
}

TEST_CASE("N14 a sequence older than the ring is stale, and the ring survives the wrap", "[network][ring]") {
    PacketRing ring;
    ring.note_received(65530);
    ring.note_received(65535);
    REQUIRE(ring.note_received(0) == PacketRing::Receive::Accepted);
    REQUIRE(ring.note_received(3) == PacketRing::Receive::Accepted);
    REQUIRE(ring.ack() == 3);
    // 2 no, 1 no, 0 yes (bit 2), 65535 yes (bit 3), 65534..65531 no, 65530 yes (bit 8).
    REQUIRE(ring.ack_bits() == bits({2, 3, 8}));
    REQUIRE(ring.note_received(65535) == PacketRing::Receive::Replay);
    ring.note_received(static_cast<std::uint16_t>(3 + network::kRingSize));
    REQUIRE(ring.note_received(3) == PacketRing::Receive::Stale);
}

TEST_CASE("N15 process_acks fires once per newly acked packet; only the named ack has an RTT sample", "[network][ring]") {
    PacketRing ring;
    for (std::uint16_t s = 0; s < 5; ++s) {
        SentPacket& sent = ring.record_sent(s, 1.0 + s * 0.01);
        sent.tag = 100 + s;
    }
    std::vector<std::uint16_t> acked;
    std::vector<double> samples;
    // The peer saw 4, 2, and 0: ack 4, bits for 2 (bit 1) and 0 (bit 3).
    ring.process_acks(4, low((1u << 1) | (1u << 3)), 1.2, [&](SentPacket& p, double rtt) {
        acked.push_back(p.sequence);
        samples.push_back(rtt);
    });
    REQUIRE(acked == std::vector<std::uint16_t>{4, 2, 0});
    REQUIRE(samples[0] == Approx(1.2 - 1.04));
    REQUIRE(samples[1] == -1.0);
    REQUIRE(samples[2] == -1.0);
    // Again: nothing new.
    acked.clear();
    ring.process_acks(4, low((1u << 1) | (1u << 3)), 1.3, [&](SentPacket& p, double) { acked.push_back(p.sequence); });
    REQUIRE(acked.empty());
    // A later ack covering 1 and 3 fires those.
    ring.process_acks(4, low(0xF), 1.4, [&](SentPacket& p, double) { acked.push_back(p.sequence); });
    REQUIRE(acked == std::vector<std::uint16_t>{3, 1});
    // The last of the 128 bits names the sequence 128 before the ack.
    for (std::uint16_t s = 200; s <= 328; ++s) ring.record_sent(s, 2.0);
    acked.clear();
    ring.process_acks(328, bits({127}), 2.1, [&](SentPacket& p, double) { acked.push_back(p.sequence); });
    REQUIRE(acked == std::vector<std::uint16_t>{328, 200});
}

TEST_CASE("N16 an ack of a sequence never sent, or whose slot was reused, does nothing", "[network][ring]") {
    PacketRing ring;
    ring.record_sent(7, 1.0);
    ring.record_sent(static_cast<std::uint16_t>(7 + network::kRingSize), 2.0);   // reuses slot 7
    int fired = 0;
    ring.process_acks(7, AckBits{}, 2.1, [&](SentPacket&, double) { ++fired; });
    REQUIRE(fired == 0);
    ring.process_acks(0xFFFF, AckBits{}, 2.1, [&](SentPacket&, double) { ++fired; });
    REQUIRE(fired == 0);
    ring.process_acks(static_cast<std::uint16_t>(7 + network::kRingSize), AckBits{}, 2.1, [&](SentPacket&, double) { ++fired; });
    REQUIRE(fired == 1);
}

TEST_CASE("N17 loss counts packets older than the timeout and not acked, over the last two seconds", "[network][ring]") {
    PacketRing ring;
    for (std::uint16_t s = 0; s < 10; ++s) ring.record_sent(s, 10.0 + s * 0.1);   // 10.0 .. 10.9
    // Ack the even ones.
    ring.process_acks(8, bits({1, 3, 5, 7}), 11.0, [](SentPacket&, double) {});
    // At 11.5 with a 0.3 s timeout, packets sent at or before 11.2 count: 0..9 all. Five lost of ten.
    REQUIRE(ring.loss(11.5, 0.3) == Approx(0.5f));
    // At 11.05, only those sent at or before 10.75 count: 0..7, four lost of eight.
    REQUIRE(ring.loss(11.05, 0.3) == Approx(0.5f));
    // At 13.5, those sent before 11.5 are outside the two-second window: none count.
    REQUIRE(ring.loss(13.5, 0.3) == Approx(0.0f));
}
