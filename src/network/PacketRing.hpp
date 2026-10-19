#pragma once

#include "Packet.hpp"
#include "Sequence.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace network {

// Packets remembered each way: at the in-flight cap, about eight round trips, so the
// loss estimate still sees packets old enough to judge while a peer bursts.
constexpr std::size_t kRingSize = 1024;
// Packets one round trip may carry: an eighth of the ring, so acks always name live slots
// and the loss estimate has history beyond the 2 x RTT it waits before judging a packet.
constexpr std::size_t kMaxPacketsInFlight = 128;
constexpr int kMaxRefsPerPacket = 64;
// A MessageRef with this fragment is a whole reliable message.
constexpr std::uint16_t kWholeMessage = 0xFFFF;

// What a packet carried of the reliable channel, so an ack can mark it.
struct MessageRef {
    std::uint16_t id = 0;
    std::uint16_t fragment = kWholeMessage;
};

// One packet we sent, until its slot is reused kRingSize packets later.
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
    AckBits ack_bits() const;

    // For each sequence the peer's ack covers that we sent and had not seen
    // acked: marks it and calls on_acked(SentPacket&, rtt_sample). Only the
    // packet named by ack yields a sample (others waited on the peer up to its
    // send interval; they get -1). A late ack
    // still counts: the peer holds that packet, whatever the loss estimate said.
    template <class F>
    void process_acks(std::uint16_t ack, const AckBits& ack_bits, double now, F&& on_acked) {
        for (int n = -1; n < kAckBits; ++n) {
            if (n >= 0 && !ack_bits.test(n)) continue;
            const std::uint16_t sequence = static_cast<std::uint16_t>(n < 0 ? ack : ack - n - 1);
            SentPacket& sent = sent_[sequence % kRingSize];
            if (!sent.valid || sent.acked || sent.sequence != sequence) continue;
            sent.acked = true;
            summary_[sequence % kRingSize].acked = 1;
            on_acked(sent, n < 0 ? std::max(0.0, now - sent.time) : -1.0);
        }
    }

    // Of the packets sent in the last 2 s that are at least timeout old, the
    // share not acked. 0 when none qualify.
    float loss(double now, double timeout) const;

private:
    // What loss() reads of each sent packet, packed apart from the refs so its walk stays in cache.
    struct SentSummary {
        double time = 0.0;
        std::uint16_t sequence = 0;
        std::uint8_t valid = 0;
        std::uint8_t acked = 0;
    };

    // On the heap, made once: a Connection stays small enough for a stack.
    std::vector<SentPacket> sent_ = std::vector<SentPacket>(kRingSize);
    std::vector<std::uint16_t> received_ = std::vector<std::uint16_t>(kRingSize, 0);
    std::vector<std::uint8_t> received_valid_ = std::vector<std::uint8_t>(kRingSize, 0);
    std::vector<SentSummary> summary_ = std::vector<SentSummary>(kRingSize);
    std::uint16_t latest_sent_ = 0;
    std::uint16_t latest_ = 0;
    bool any_received_ = false;
};

}  // namespace network
