#pragma once

// A RemoteEvent's message on the wire (docs/superpowers/specs/2026-10-20-remote-events-design.md):
//   u8 MessageType::Remote, u8 kRemoteCodecVersion, varuint remote netid,
//   varuint count (at most kRemoteMaxArgs), count x value
// A value is a u8 tag, then:
//   Nil, False, True   nothing
//   Float32            f32: a number a float holds exactly
//   Float64            f64: every other number
//   String             string (varuint length, bytes)
//   Instance           varuint netid
//   Engine             u8 kind, then that kind's fixed layout (below)
//   Root               nothing: game, which both sides hold
//   Table              varuint count, count x value (the array part),
//                      varuint count, count x (string key, value)
// An engine value's layout, every number an f32 (the engine keeps them as
// floats, so each arrives exact):
//   Vector3            x, y, z: 12 bytes
//   Vector2            x, y
//   Color3             r, g, b (alpha arrives as 1)
//   Matrix4            16 floats, column-major
//   EnumItem           string type name, varuint item value
//   NumberCurve        u8 mode, then Constant: value; Range: min, max;
//                      Curve: keys; CurveRange: keys, keys
//                      keys: varuint count (1..kRemoteMaxCurveKeys),
//                            count x (time, value, u8 interp)
//   ColorCurve         u8 mode, then Constant: key; RandomBetween: key, key;
//                      Gradient: keys; GradientRange: keys, keys
//                      key: r, g, b, transparency
//                      keys: varuint count (1..kRemoteMaxCurveKeys),
//                            count x (time, key)
// No other LuaSlot kind travels: a Signal, an InputObject, a TouchObject and
// a Values slot are never a script's value to send.

#include "ByteBuffer.hpp"

#include "Packet.hpp"

#include "RemoteValues.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace engine_network {

// A remote message's layout version, its second byte. 3: engine values in
// fixed binary layouts (2 sent them as JSON).
constexpr std::uint8_t kRemoteCodecVersion = 3;

// Instance values are NetIds on the wire; the two callbacks map them.
// 0: the instance is not on the other side, so it arrives as nil.
using IdToNet = std::function<std::uint32_t(engine_core::InstanceId)>;
// 0: an unknown netid, which arrives as nil.
using NetToId = std::function<engine_core::InstanceId(std::uint32_t)>;

void encode_remote(std::uint32_t remote_netid, const engine_core::RemoteValues& values, const IdToNet& to_net,
                   ByteWriter& out);

// Reads one whole message, header included. False on any malformed input,
// more than kRemoteMaxArgs arguments, tables nested deeper than 32, bytes
// left over, a message larger than max_bytes, or more than max_values values
// (every argument, table and value inside one, counted as RemoteValues counts
// them for kRemoteMaxValues); out is then empty, and nothing is made for
// values past the cap. values_read (when given) is how many it read, whether
// or not the message held up: every value counted before it failed, and all
// of max_values when a count asked for more than was left. Never throws,
// never crashes.
bool decode_remote(ByteReader& in, std::uint32_t& remote_netid, engine_core::RemoteValues& out, const NetToId& to_id,
                   std::size_t max_bytes, std::size_t max_values = engine_core::kRemoteMaxValues,
                   std::size_t* values_read = nullptr);

// A tick's unreliable messages for one peer ride its packet's payload:
//   varuint count, count x (varuint length, message bytes)
// Bytes a varuint takes.
constexpr std::size_t varuint_size(std::uint64_t value) {
    std::size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

// The largest message that fits an empty payload on its own: one byte less
// than the payload holds stays for the packet's own end, and the count and
// the length come first.
constexpr std::size_t unreliable_room(std::size_t message) {
    return varuint_size(1) + varuint_size(message) + message;
}
constexpr std::size_t largest_unreliable_message() {
    std::size_t message = network::kMaxPayloadBytes - 1;
    while (message > 0 && unreliable_room(message) > network::kMaxPayloadBytes - 1) {
        --message;
    }
    return message;
}
constexpr std::size_t kMaxUnreliableRemoteBytes = largest_unreliable_message();

using RemoteMessage = std::shared_ptr<const std::vector<std::uint8_t>>;

// Packs messages, in order, into out until the next would take it past
// budget bytes; returns how many went in (0 leaves out empty).
std::size_t pack_remote_payload(const std::vector<RemoteMessage>& messages, std::size_t budget,
                                std::vector<std::uint8_t>& out);

// Calls visit(data, size) for each message in a payload, in order. False,
// having visited nothing, when the framing is bad: a count or a length past
// the bytes there, or bytes left over.
bool read_remote_payload(const std::uint8_t* data, std::size_t size,
                         const std::function<void(const std::uint8_t*, std::size_t)>& visit);

}  // namespace engine_network
