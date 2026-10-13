#pragma once

#include "Bits.hpp"

#include <cstddef>
#include <cstdint>

namespace network {

// Bumped whenever the wire changes. A packet with another id is dropped
// without a reply.
constexpr std::uint16_t kProtocolId = 0xA11C;
// One datagram, under every common path MTU so IP never fragments it.
constexpr std::size_t kMaxPacketBytes = 1200;
constexpr std::size_t kHeaderBytes = 13;
constexpr std::size_t kMaxPayloadBytes = kMaxPacketBytes - kHeaderBytes;

enum class PacketType : std::uint8_t { ConnectRequest, ConnectAccept, ConnectDeny, Disconnect, KeepAlive, Payload };

// Every packet starts with this. ack and ack_bits are the peer's view of what
// it has received: bit n of ack_bits means sequence (ack - n - 1) arrived.
struct PacketHeader {
    std::uint16_t protocol = kProtocolId;
    PacketType type = PacketType::Payload;
    std::uint16_t session = 0;   // low 16 bits of client salt XOR server salt; 0 while connecting
    std::uint16_t sequence = 0;
    std::uint16_t ack = 0xFFFF;
    std::uint32_t ack_bits = 0;
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
