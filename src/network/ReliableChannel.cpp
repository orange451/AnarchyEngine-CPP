#include "ReliableChannel.hpp"

#include <algorithm>
#include <cstring>

namespace network {

bool ReliableChannel::send(const std::uint8_t* bytes, std::size_t length) {
    if (length > kMaxBlockBytes) return false;
    if (unacked() >= kWindowSize) return false;
    Outgoing& out = out_[next_send_id_ % kWindowSize];
    out.valid = true;
    out.acked = false;
    out.id = next_send_id_;
    out.length = static_cast<std::uint32_t>(length);
    out.fragments_acked = 0;
    out.last_send = -1e9;
    if (length <= kFragmentBytes) {
        out.fragment_count = 0;
        if (length > 0) std::memcpy(out.inline_bytes, bytes, length);
        out.block = std::vector<std::uint8_t>();
    } else {
        out.fragment_count = static_cast<std::uint16_t>((length + kFragmentBytes - 1) / kFragmentBytes);
        out.block.assign(bytes, bytes + length);
        out.fragment_last_send.assign(out.fragment_count, -1e9);
        out.fragment_acked.assign(out.fragment_count, 0);
    }
    ++next_send_id_;
    return true;
}

bool ReliableChannel::has_due(double now, double rtt) const { return bytes_due(now, rtt) > 0; }

std::size_t ReliableChannel::bytes_due(double now, double rtt) const {
    std::size_t total = 0;
    for (std::uint16_t id = oldest_unacked_; id != next_send_id_; ++id) {
        const Outgoing& out = out_[id % kWindowSize];
        if (!out.valid || out.acked) continue;
        if (out.fragment_count == 0) {
            if (due(out.last_send, now, rtt)) total += entry_bits(false, out.length) / 8;
            continue;
        }
        for (std::uint16_t f = 0; f < out.fragment_count; ++f) {
            if (out.fragment_acked[f] || !due(out.fragment_last_send[f], now, rtt)) continue;
            total += entry_bits(true, std::min(kFragmentBytes, out.length - f * kFragmentBytes)) / 8;
        }
    }
    return total;
}

void ReliableChannel::write(BitWriter& writer, double now, double rtt, SentPacket& packet, std::size_t reserve_bytes) {
    const std::size_t reserved = reserve_bytes * 8;
    const auto room = [&]() { return writer.bits_left() > reserved ? writer.bits_left() - reserved : 0; };
    for (std::uint16_t id = oldest_unacked_; id != next_send_id_; ++id) {
        Outgoing& out = out_[id % kWindowSize];
        if (!out.valid || out.acked) continue;
        if (packet.ref_count >= kMaxRefsPerPacket) break;
        if (out.fragment_count == 0) {
            if (!due(out.last_send, now, rtt)) continue;
            if (room() < entry_bits(false, out.length)) continue;   // a smaller one later may still fit
            writer.boolean(true);
            writer.bits(out.id, 16);
            writer.bits(0, 16);
            writer.bits(out.length, 16);
            writer.bytes(out.inline_bytes, out.length);
            out.last_send = now;
            packet.refs[packet.ref_count++] = MessageRef{out.id, kWholeMessage};
            continue;
        }
        for (std::uint16_t f = 0; f < out.fragment_count; ++f) {
            if (out.fragment_acked[f] || !due(out.fragment_last_send[f], now, rtt)) continue;
            if (packet.ref_count >= kMaxRefsPerPacket) break;
            const std::size_t length = std::min(kFragmentBytes, out.length - f * kFragmentBytes);
            if (room() < entry_bits(true, length)) break;
            writer.boolean(true);
            writer.bits(out.id, 16);
            writer.bits(out.fragment_count, 16);
            writer.bits(f, 16);
            writer.bits(static_cast<std::uint32_t>(length), 16);
            writer.bytes(out.block.data() + f * kFragmentBytes, length);
            out.fragment_last_send[f] = now;
            packet.refs[packet.ref_count++] = MessageRef{out.id, f};
        }
    }
    writer.boolean(false);
}

namespace {

// One entry's fields. False when they cannot be right.
bool read_entry(BitReader& reader, std::uint16_t& id, std::uint16_t& fragment_count, std::uint16_t& fragment_index,
                std::uint32_t& length) {
    id = static_cast<std::uint16_t>(reader.bits(16));
    fragment_count = static_cast<std::uint16_t>(reader.bits(16));
    fragment_index = fragment_count > 0 ? static_cast<std::uint16_t>(reader.bits(16)) : 0;
    length = reader.bits(16);
    if (reader.failed() || length > kFragmentBytes) return false;
    if (fragment_count > 0) {
        if (fragment_index >= fragment_count) return false;
        if (static_cast<std::size_t>(fragment_count) * kFragmentBytes > kMaxBlockBytes) return false;
        if (fragment_index + 1 < fragment_count && length != kFragmentBytes) return false;
    }
    reader.align();
    return reader.bytes_left() >= length;
}

}  // namespace

bool ReliableChannel::validate(BitReader reader) const {
    std::uint8_t scratch[kFragmentBytes];
    while (reader.boolean()) {
        if (reader.failed()) return false;
        std::uint16_t id, fragment_count, fragment_index;
        std::uint32_t length;
        if (!read_entry(reader, id, fragment_count, fragment_index, length)) return false;
        reader.bytes(scratch, length);
    }
    return !reader.failed();
}

bool ReliableChannel::read(BitReader& reader) {
    if (!validate(reader)) return false;
    std::uint8_t scratch[kFragmentBytes];
    while (reader.boolean()) {
        std::uint16_t id, fragment_count, fragment_index;
        std::uint32_t length;
        read_entry(reader, id, fragment_count, fragment_index, length);
        const int offset = sequence_diff(id, next_deliver_);
        if (offset < 0 || offset >= static_cast<int>(kWindowSize)) {
            reader.bytes(scratch, length);   // already delivered, or beyond the window: skip it
            continue;
        }
        Incoming& in = in_[id % kWindowSize];
        if (!in.valid || in.id != id) {
            in.valid = true;
            in.complete = false;
            in.id = id;
            in.length = 0;
            in.fragment_count = fragment_count;
            in.fragments_received = 0;
            if (fragment_count > 0) {
                in.block.assign(static_cast<std::size_t>(fragment_count) * kFragmentBytes, 0);
                in.fragment_present.assign(fragment_count, 0);
            }
        }
        if (in.complete || in.fragment_count != fragment_count) {
            reader.bytes(scratch, length);
            continue;
        }
        if (fragment_count == 0) {
            reader.bytes(in.inline_bytes, length);
            in.length = length;
            in.complete = true;
            continue;
        }
        if (in.fragment_present[fragment_index]) {
            reader.bytes(scratch, length);
            continue;
        }
        reader.bytes(in.block.data() + static_cast<std::size_t>(fragment_index) * kFragmentBytes, length);
        in.fragment_present[fragment_index] = 1;
        ++in.fragments_received;
        if (fragment_index + 1 == fragment_count) in.length = fragment_index * kFragmentBytes + length;
        if (in.fragments_received == fragment_count) in.complete = true;
    }
    return !reader.failed();
}

void ReliableChannel::acked(const MessageRef& ref) {
    Outgoing& out = out_[ref.id % kWindowSize];
    if (!out.valid || out.id != ref.id || out.acked) return;
    if (ref.fragment == kWholeMessage) {
        if (out.fragment_count != 0) return;
        out.acked = true;
    } else {
        if (ref.fragment >= out.fragment_count || out.fragment_acked[ref.fragment]) return;
        out.fragment_acked[ref.fragment] = 1;
        if (++out.fragments_acked == out.fragment_count) out.acked = true;
    }
    // The window slides past every acked message at its front.
    while (sequence_diff(next_send_id_, oldest_unacked_) > 0) {
        Outgoing& front = out_[oldest_unacked_ % kWindowSize];
        if (!front.acked) break;
        front.valid = false;
        front.block = std::vector<std::uint8_t>();
        front.fragment_last_send = std::vector<double>();
        front.fragment_acked = std::vector<std::uint8_t>();
        ++oldest_unacked_;
    }
}

}  // namespace network
