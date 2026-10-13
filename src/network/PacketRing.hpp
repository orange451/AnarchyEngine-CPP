#pragma once

#include "Sequence.hpp"

#include <cstddef>
#include <cstdint>

namespace network {

constexpr std::size_t kRingSize = 256;
constexpr int kMaxRefsPerPacket = 64;
// A MessageRef with this fragment is a whole reliable message.
constexpr std::uint16_t kWholeMessage = 0xFFFF;

// What a packet carried of the reliable channel, so an ack can mark it.
struct MessageRef {
    std::uint16_t id = 0;
    std::uint16_t fragment = kWholeMessage;
};

// One packet we sent, until its slot is reused 256 packets later.
struct SentPacket {
    bool valid = false;
    bool acked = false;
    bool has_payload = false;
    std::uint16_t sequence = 0;
    std::uint16_t bytes = 0;
    std::uint32_t tag = 0;
    double time = 0.0;
    std::uint8_t ref_count = 0;
    MessageRef refs[kMaxRefsPerPacket];
};

// Fiedler's two sequence buffers: what we sent, to learn from the peer's acks
// which packets arrived; and what we received, to tell the peer the same.
class PacketRing {
public:
    SentPacket& record_sent(std::uint16_t sequence, double now);

    enum class Receive { Accepted, Replay, Stale };
    Receive note_received(std::uint16_t sequence);
    bool any_received() const { return any_received_; }
    std::uint16_t ack() const { return any_received_ ? latest_ : 0xFFFF; }
    std::uint32_t ack_bits() const;

    // For each sequence the peer's ack covers that we sent and had not seen
    // acked: marks it and calls on_acked(SentPacket&, rtt_sample). A late ack
    // still counts: the peer holds that packet, whatever the loss estimate said.
    template <class F>
    void process_acks(std::uint16_t ack, std::uint32_t ack_bits, double now, F&& on_acked) {
        for (int n = -1; n < 32; ++n) {
            if (n >= 0 && (ack_bits & (1u << n)) == 0) continue;
            const std::uint16_t sequence = static_cast<std::uint16_t>(n < 0 ? ack : ack - n - 1);
            SentPacket& sent = sent_[sequence % kRingSize];
            if (!sent.valid || sent.acked || sent.sequence != sequence) continue;
            sent.acked = true;
            on_acked(sent, now - sent.time);
        }
    }

    // Of the packets sent in the last 2 s that are at least timeout old, the
    // share not acked. 0 when none qualify.
    float loss(double now, double timeout) const;

private:
    SentPacket sent_[kRingSize];
    std::uint16_t received_[kRingSize] = {};
    bool received_valid_[kRingSize] = {};
    std::uint16_t latest_ = 0;
    bool any_received_ = false;
};

}  // namespace network
