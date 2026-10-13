#pragma once

#include "Bits.hpp"
#include "PacketRing.hpp"
#include "Sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace network {

// A message over this many bytes is a block, sent as fragments of this size.
constexpr std::size_t kFragmentBytes = 1000;
// Messages in flight, and messages the receiver holds for ordering.
constexpr std::size_t kWindowSize = 256;
constexpr std::size_t kMaxBlockBytes = 16u << 20;

// Ordered messages delivered exactly once. Each is sent in every packet
// that has room, until acked, no sooner than RTT × 1.25 after its last
// send; a block's fragments are acked one by one. Small messages live
// inline; only blocks use the heap.
class ReliableChannel {
public:
    bool send(const std::uint8_t* bytes, std::size_t length);
    bool has_due(double now, double rtt) const;
    std::size_t bytes_due(double now, double rtt) const;
    // Appends the section and records what it carried in packet.refs. The
    // last reserve_bytes of the writer are left for the payload.
    void write(BitWriter& writer, double now, double rtt, SentPacket& packet, std::size_t reserve_bytes = 0);
    // Walks a section without applying it. Connection checks before it
    // records the packet as received, so a bad packet is as if it never came.
    bool validate(BitReader reader) const;
    bool read(BitReader& reader);
    template <class F>
    void deliver(F&& on_message) {
        for (;;) {
            Incoming& in = in_[next_deliver_ % kWindowSize];
            if (!in.valid || !in.complete || in.id != next_deliver_) return;
            on_message(in.data(), in.length);
            in.valid = false;
            in.block = std::vector<std::uint8_t>();
            in.fragment_present = std::vector<std::uint8_t>();
            ++next_deliver_;
        }
    }
    void acked(const MessageRef& ref);
    std::size_t unacked() const { return static_cast<std::size_t>(sequence_diff(next_send_id_, oldest_unacked_)); }

private:
    struct Outgoing {
        bool valid = false;
        bool acked = false;
        std::uint16_t id = 0;
        std::uint32_t length = 0;
        std::uint16_t fragment_count = 0;   // 0: a whole message in inline_bytes
        std::uint16_t fragments_acked = 0;
        double last_send = -1e9;
        std::uint8_t inline_bytes[kFragmentBytes];
        std::vector<std::uint8_t> block;
        std::vector<double> fragment_last_send;
        std::vector<std::uint8_t> fragment_acked;
        const std::uint8_t* data() const { return fragment_count == 0 ? inline_bytes : block.data(); }
    };
    struct Incoming {
        bool valid = false;
        bool complete = false;
        std::uint16_t id = 0;
        std::uint32_t length = 0;
        std::uint16_t fragment_count = 0;
        std::uint16_t fragments_received = 0;
        std::uint8_t inline_bytes[kFragmentBytes];
        std::vector<std::uint8_t> block;
        std::vector<std::uint8_t> fragment_present;
        const std::uint8_t* data() const { return fragment_count == 0 ? inline_bytes : block.data(); }
    };
    static bool due(double last_send, double now, double rtt) { return now - last_send >= rtt * 1.25; }
    static std::size_t entry_bits(bool fragment, std::size_t length) {
        // The continuation bit, id, fragment count, index, length, alignment, bytes, and the terminating bit.
        return 1 + 16 + 16 + (fragment ? 16 : 0) + 16 + 7 + length * 8 + 1;
    }

    // Each window is about 260 KB, made once here so a Connection stays small
    // enough for a stack and nothing allocates per packet.
    std::vector<Outgoing> out_ = std::vector<Outgoing>(kWindowSize);
    std::uint16_t next_send_id_ = 0;
    std::uint16_t oldest_unacked_ = 0;
    std::vector<Incoming> in_ = std::vector<Incoming>(kWindowSize);
    std::uint16_t next_deliver_ = 0;
};

}  // namespace network
