#pragma once

#include "Bits.hpp"

#include <cstddef>
#include <cstdint>

namespace network {

// Bumped whenever the wire changes. A packet with another id is dropped
// without a reply.
constexpr std::uint16_t kProtocolId = 0xA11D;
// One datagram, under every common path MTU so IP never fragments it.
constexpr std::size_t kMaxPacketBytes = 1200;
constexpr std::size_t kHeaderBytes = 25;
constexpr std::size_t kMaxPayloadBytes = kMaxPacketBytes - kHeaderBytes;

// Sequences an ack names besides its own: a peer bursting up to 32 packets a tick is
// covered for four ticks, so a lost ack or three cost nothing.
constexpr int kAckBits = 128;

// Bit n set: sequence (ack - n - 1) arrived.
struct AckBits {
    std::uint32_t words[kAckBits / 32] = {};
    bool test(int n) const { return (words[n / 32] >> (n % 32)) & 1u; }
    void set(int n) { words[n / 32] |= 1u << (n % 32); }
    bool operator==(const AckBits& other) const {
        for (int i = 0; i < kAckBits / 32; ++i) {
            if (words[i] != other.words[i]) return false;
        }
        return true;
    }
    bool operator!=(const AckBits& other) const { return !(*this == other); }
};

enum class PacketType : std::uint8_t { ConnectRequest, ConnectAccept, ConnectDeny, Disconnect, KeepAlive, Payload };

// Every packet starts with this. ack and ack_bits are the peer's view of what
// it has received: bit n of ack_bits means sequence (ack - n - 1) arrived.
struct PacketHeader {
    std::uint16_t protocol = kProtocolId;
    PacketType type = PacketType::Payload;
    std::uint16_t session = 0;   // low 16 bits of client salt XOR server salt; 0 while connecting
    std::uint16_t sequence = 0;
    std::uint16_t ack = 0xFFFF;
    AckBits ack_bits;
};

void write_header(BitWriter& writer, const PacketHeader& header);
// False when the bytes are too short or carry another protocol id.
bool read_header(BitReader& reader, PacketHeader& header);

// Sub-project 7's hook: encryption goes here. Today both are the identity.
// encode returns the length to send; decode returns the length to read, or 0
// to drop the packet.
std::size_t encode_packet(std::uint8_t* bytes, std::size_t length, std::size_t capacity);
std::size_t decode_packet(std::uint8_t* bytes, std::size_t length);

}  // namespace network
