#include "PacketRing.hpp"

namespace network {

SentPacket& PacketRing::record_sent(std::uint16_t sequence, double now) {
    SentPacket& sent = sent_[sequence % kRingSize];
    sent.valid = true;
    sent.acked = false;
    sent.has_payload = false;
    sent.sequence = sequence;
    sent.bytes = 0;
    sent.tag = 0;
    sent.time = now;
    sent.ref_count = 0;
    SentSummary& summary = summary_[sequence % kRingSize];
    summary.time = now;
    summary.sequence = sequence;
    summary.valid = 1;
    summary.acked = 0;
    latest_sent_ = sequence;
    return sent;
}

PacketRing::Receive PacketRing::note_received(std::uint16_t sequence) {
    if (!any_received_) {
        any_received_ = true;
        latest_ = sequence;
        received_[sequence % kRingSize] = sequence;
        received_valid_[sequence % kRingSize] = 1;
        return Receive::Accepted;
    }
    if (sequence_greater_than(sequence, latest_)) {
        // The slots between the old latest and the new are not received.
        const int gap = sequence_diff(sequence, latest_);
        if (gap >= static_cast<int>(kRingSize)) {
            for (std::size_t i = 0; i < kRingSize; ++i) received_valid_[i] = 0;
        } else {
            for (int i = 1; i < gap; ++i) {
                received_valid_[static_cast<std::uint16_t>(latest_ + i) % kRingSize] = 0;
            }
        }
        latest_ = sequence;
    } else {
        const int age = sequence_diff(latest_, sequence);
        if (age >= static_cast<int>(kRingSize)) return Receive::Stale;
        const std::size_t slot = sequence % kRingSize;
        if (received_valid_[slot] && received_[slot] == sequence) return Receive::Replay;
    }
    received_[sequence % kRingSize] = sequence;
    received_valid_[sequence % kRingSize] = 1;
    return Receive::Accepted;
}

AckBits PacketRing::ack_bits() const {
    AckBits bits;
    if (!any_received_) return bits;
    for (int n = 0; n < kAckBits; ++n) {
        const std::uint16_t sequence = static_cast<std::uint16_t>(latest_ - n - 1);
        const std::size_t slot = sequence % kRingSize;
        if (received_valid_[slot] && received_[slot] == sequence) bits.set(n);
    }
    return bits;
}

float PacketRing::loss(double now, double timeout) const {
    int counted = 0;
    int lost = 0;
    // Newest first, so the walk stops at the first packet older than the window: a
    // quiet peer reads a few slots, not the whole ring.
    for (std::size_t back = 0; back < kRingSize; ++back) {
        const std::uint16_t sequence = static_cast<std::uint16_t>(latest_sent_ - back);
        const SentSummary& sent = summary_[sequence % kRingSize];
        if (!sent.valid || sent.sequence != sequence) break;
        if (sent.time < now - 2.0) break;
        if (sent.time > now - timeout) continue;
        ++counted;
        if (!sent.acked) ++lost;
    }
    return counted == 0 ? 0.0f : static_cast<float>(lost) / static_cast<float>(counted);
}

}  // namespace network
