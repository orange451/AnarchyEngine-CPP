#include "Packet.hpp"

namespace network {

void write_header(BitWriter& writer, const PacketHeader& header) {
    writer.bits(header.protocol, 16);
    writer.bits(static_cast<std::uint32_t>(header.type), 8);
    writer.bits(header.session, 16);
    writer.bits(header.sequence, 16);
    writer.bits(header.ack, 16);
    writer.bits(header.ack_bits, 32);
}

bool read_header(BitReader& reader, PacketHeader& header) {
    header.protocol = static_cast<std::uint16_t>(reader.bits(16));
    const std::uint32_t type = reader.bits(8);
    header.session = static_cast<std::uint16_t>(reader.bits(16));
    header.sequence = static_cast<std::uint16_t>(reader.bits(16));
    header.ack = static_cast<std::uint16_t>(reader.bits(16));
    header.ack_bits = reader.bits(32);
    if (reader.failed() || header.protocol != kProtocolId || type > static_cast<std::uint32_t>(PacketType::Payload)) {
        return false;
    }
    header.type = static_cast<PacketType>(type);
    return true;
}

std::size_t encode_packet(std::uint8_t*, std::size_t length, std::size_t) { return length; }

std::size_t decode_packet(std::uint8_t*, std::size_t length) { return length; }

}  // namespace network
