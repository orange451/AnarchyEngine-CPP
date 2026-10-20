#pragma once

// A RemoteEvent's message on the wire (docs/superpowers/specs/2026-10-20-remote-events-design.md):
//   u8 MessageType::Remote, u8 kCodecVersion, varuint remote netid,
//   varuint count, count x value
// A value is a u8 tag, then:
//   Nil, False, True   nothing
//   Float32            f32: a number a float holds exactly
//   Float64            f64: every other number
//   String             string (varuint length, bytes)
//   Instance           varuint netid
//   Engine             u8 kind, [string enum type: Enum only], json value
//                      (slot_to_json's value, as JsonWire writes it)
//   Root               nothing: game, which both sides hold
//   Table              varuint count, count x value (the array part),
//                      varuint count, count x (string key, value)

#include "ByteBuffer.hpp"

#include "RemoteValues.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace engine_network {

// Instance values are NetIds on the wire; the two callbacks map them.
// 0: the instance is not on the other side, so it arrives as nil.
using IdToNet = std::function<std::uint32_t(engine_core::InstanceId)>;
// 0: an unknown netid, which arrives as nil.
using NetToId = std::function<engine_core::InstanceId(std::uint32_t)>;

void encode_remote(std::uint32_t remote_netid, const engine_core::RemoteValues& values, const IdToNet& to_net,
                   ByteWriter& out);

// Reads one whole message, header included. False on any malformed input,
// tables nested deeper than 32, bytes left over, a message larger than
// max_bytes, or more than max_values values (every argument, table and value
// inside one, counted as RemoteValues counts them for kRemoteMaxValues); out
// is then unspecified, and nothing is made for values past the cap. On
// success, values_read (when given) is how many it read. Never throws, never
// crashes.
bool decode_remote(ByteReader& in, std::uint32_t& remote_netid, engine_core::RemoteValues& out, const NetToId& to_id,
                   std::size_t max_bytes, std::size_t max_values = engine_core::kRemoteMaxValues,
                   std::size_t* values_read = nullptr);

}  // namespace engine_network
