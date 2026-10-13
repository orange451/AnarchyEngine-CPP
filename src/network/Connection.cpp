#include "Connection.hpp"

#include <algorithm>
#include <cstring>

namespace network {

Connection::Connection(double bytes_per_second, double send_interval)
    : flow_(bytes_per_second), send_interval_(send_interval) {}

std::size_t Connection::payload_budget(double now) const {
    // At most one packet's share of the reliable backlog, so a block in flight
    // cannot starve the payload.
    const std::size_t due = std::min(reliable_.bytes_due(now, rtt()), kFragmentBytes + 16);
    // One byte stays for the section's terminating bit.
    const std::size_t room = kMaxPayloadBytes - 1;
    const std::size_t space = due >= room ? 0 : room - due;
    const std::size_t by_rate = static_cast<std::size_t>(flow_.bytes_per_second() * send_interval_);
    return std::min(space, by_rate);
}

std::optional<std::uint16_t> Connection::set_payload(const std::uint8_t* bytes, std::size_t length, std::uint32_t tag) {
    if (has_payload_ || length == 0 || length > kMaxPayloadBytes - 1) return std::nullopt;
    if (length > 0) std::memcpy(payload_, bytes, length);
    payload_length_ = static_cast<std::uint16_t>(length);
    payload_tag_ = tag;
    has_payload_ = true;
    return next_sequence_;
}

bool Connection::wants_packet(double now) const {
    return has_payload_ || reliable_.has_due(now, rtt()) || last_send_ < 0.0 || now - last_send_ >= 0.1 - 1e-9;
}

std::size_t Connection::write_packet(std::uint8_t* out, std::size_t capacity, double now) {
    PacketHeader header;
    header.type = has_payload_ ? PacketType::Payload : PacketType::KeepAlive;
    header.session = session_;
    header.sequence = next_sequence_;
    header.ack = ring_.ack();
    header.ack_bits = ring_.ack_bits();
    BitWriter writer(out, capacity);
    write_header(writer, header);
    SentPacket& sent = ring_.record_sent(next_sequence_, now);
    // The payload was budgeted first; the reliable section takes what is left.
    reliable_.write(writer, now, rtt(), sent, has_payload_ ? payload_length_ + 1u : 0u);
    writer.align();
    if (has_payload_) {
        writer.bytes(payload_, payload_length_);
        sent.has_payload = true;
        sent.tag = payload_tag_;
    }
    const std::size_t length = writer.bytes_written();
    sent.bytes = static_cast<std::uint16_t>(length);
    has_payload_ = false;
    ++next_sequence_;
    last_send_ = now;
    ++stats_.packets_sent;
    account(out_window_bytes_, out_rate_, out_window_start_, length, now);
    flow_.update(now, rtt(), ring_.loss(now, loss_timeout()));
    return encode_packet(out, length, capacity);
}

bool Connection::read_packet(const PacketHeader& header, BitReader& reader, const std::uint8_t* bytes, std::size_t length, double now) {
    // Checked before anything is recorded, so a bad packet is as if it never came.
    if (!reliable_.validate(reader)) {
        ++stats_.dropped_malformed;
        return false;
    }
    if (ring_.note_received(header.sequence) != PacketRing::Receive::Accepted) {
        ++stats_.dropped_replay;
        return false;
    }
    ring_.process_acks(header.ack, header.ack_bits, now, [&](SentPacket& sent, double sample) {
        sample_rtt(sample);
        for (int i = 0; i < sent.ref_count; ++i) reliable_.acked(sent.refs[i]);
        if (sent.has_payload && callbacks.on_delivered) callbacks.on_delivered(sent.sequence, sent.tag);
    });
    reliable_.read(reader);
    reader.align();
    const std::size_t offset = reader.byte_offset();
    last_receive_ = now;
    ++stats_.packets_received;
    account(in_window_bytes_, in_rate_, in_window_start_, length, now);
    flow_.update(now, rtt(), ring_.loss(now, loss_timeout()));
    // Messages first: a "create instance" message lands before that instance's state.
    if (callbacks.on_message) {
        reliable_.deliver(callbacks.on_message);
    } else {
        reliable_.deliver([](const std::uint8_t*, std::size_t) {});
    }
    if (header.type == PacketType::Payload && offset < length && callbacks.on_payload) {
        callbacks.on_payload(header.sequence, bytes + offset, length - offset);
    }
    return true;
}

void Connection::sample_rtt(double sample) {
    sample = std::max(0.0, sample);
    rtt_ = has_rtt_ ? rtt_ + (sample - rtt_) * 0.1 : sample;
    has_rtt_ = true;
}

// One-second windows; the rate reported is the last whole window's.
void Connection::account(double& window_bytes, double& rate, double& window_start, std::size_t bytes, double now) {
    if (window_start < 0.0) window_start = now;
    if (now - window_start >= 1.0) {
        rate = window_bytes / (now - window_start);
        window_bytes = 0.0;
        window_start = now;
    }
    window_bytes += static_cast<double>(bytes);
}

ConnectionStats Connection::stats(double now) const {
    ConnectionStats stats = stats_;
    stats.rtt = rtt();
    stats.loss = ring_.loss(now, loss_timeout());
    stats.bytes_in_per_second = in_rate_;
    stats.bytes_out_per_second = out_rate_;
    stats.budget_bytes_per_second = flow_.bytes_per_second();
    return stats;
}

}  // namespace network
