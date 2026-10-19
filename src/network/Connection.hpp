#pragma once

#include "Bits.hpp"
#include "FlowControl.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"
#include "ReliableChannel.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace network {

struct ConnectionStats {
    double rtt = 0.0;
    float loss = 0.0f;
    double bytes_in_per_second = 0.0;
    double bytes_out_per_second = 0.0;
    double budget_bytes_per_second = 0.0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t dropped_replay = 0;
    std::uint64_t dropped_malformed = 0;
    // Reliable messages and fragments the peer has acked so far: it goes up while a backlog drains.
    std::uint64_t reliable_acked = 0;
    // Reliable messages and fragments sent again because no ack came in time.
    std::uint64_t reliable_resent = 0;
};

// Set once at connect; none is called from inside another.
struct ConnectionCallbacks {
    std::function<void(const std::uint8_t*, std::size_t)> on_message;
    std::function<void(std::uint16_t sequence, const std::uint8_t*, std::size_t)> on_payload;
    std::function<void(std::uint16_t sequence, std::uint32_t tag)> on_delivered;
};

// One peer: what we sent it and what it sent us. Endpoint routes packets
// to it and asks it for the packet to send each tick.
class Connection {
public:
    Connection(double bytes_per_second, double send_interval);

    ConnectionCallbacks callbacks;

    void set_session(std::uint16_t session) { session_ = session; }
    std::uint16_t session() const { return session_; }

    bool send_message(const std::uint8_t* bytes, std::size_t length) { return reliable_.send(bytes, length); }
    bool send_message(SharedMessage message) { return reliable_.send(std::move(message)); }
    // Payload bytes this tick's packet may carry: the rate's share of a tick
    // (the reliable section is not rate-limited by it), less at most one
    // packet's share of the reliable backlog, never more than fits.
    std::size_t payload_budget(double now) const;
    // This tick's unreliable payload, once; returns the sequence it will ride in.
    std::optional<std::uint16_t> set_payload(const std::uint8_t* bytes, std::size_t length, std::uint32_t tag);
    // The tick's first packet: a payload or reliable data is due, the peer sent
    // something worth acking since our last packet, or a keep-alive is due. It goes
    // whatever the credit holds.
    bool wants_packet(double now) const;
    // Called once per Endpoint::send for a connected peer: adds
    // bytes_per_second() × (now − last refill) to the credit, capped at two ticks' worth
    // and never below one packet (kMaxPacketBytes).
    void refill(double now);
    // True while this tick may carry one more packet: reliable data is due, the
    // credit holds a full packet, and fewer than max_packets_per_tick() went this tick.
    bool wants_extra_packet(double now) const;
    // At most kMaxPacketsInFlight packets in one round trip, never fewer than 1 or more than 32 a tick:
    // clamp(kMaxPacketsInFlight / ceil(max(rtt(), latest sample) / send_interval_), 1, 32).
    std::size_t max_packets_per_tick() const;
    std::size_t write_packet(std::uint8_t* out, std::size_t capacity, double now);
    bool read_packet(const PacketHeader& header, BitReader& reader, const std::uint8_t* bytes, std::size_t length, double now);

    double last_receive() const { return last_receive_; }
    double rtt() const { return has_rtt_ ? rtt_ : 0.1; }
    ConnectionStats stats(double now) const;

private:
    double loss_timeout() const { return rtt() * 2.0 > 0.1 ? rtt() * 2.0 : 0.1; }
    void sample_rtt(double sample);
    // Two ticks' worth of the flow-controlled rate, never below one packet.
    double credit_cap() const;
    void account(double& window_bytes, double& rate, double& window_start, std::size_t bytes, double now);

    PacketRing ring_;
    ReliableChannel reliable_;
    FlowControl flow_;
    double send_interval_;
    std::uint16_t session_ = 0;
    std::uint16_t next_sequence_ = 0;
    double rtt_ = 0.0;
    double last_rtt_sample_ = 0.0;
    bool has_rtt_ = false;
    double last_send_ = -1.0;
    double last_receive_ = -1.0;
    // Bytes the next packets may spend; every packet written takes its length, so it may go
    // negative, but never below -credit_cap(): a peer that always has data due sends one
    // packet a tick whatever the budget, and that debt must not grow without end.
    double credit_ = 0.0;
    double last_refill_ = -1.0;
    // The time of the tick being written and how many packets it has carried.
    double tick_time_ = -1.0;
    std::size_t tick_packets_ = 0;
    // A packet with a payload or reliable entries arrived since our last send: the next tick acks it.
    bool heard_since_send_ = false;
    bool has_payload_ = false;
    std::uint16_t payload_length_ = 0;
    std::uint32_t payload_tag_ = 0;
    std::uint8_t payload_[kMaxPayloadBytes];
    ConnectionStats stats_;
    double in_window_bytes_ = 0.0, in_rate_ = 0.0, in_window_start_ = -1.0;
    double out_window_bytes_ = 0.0, out_rate_ = 0.0, out_window_start_ = -1.0;
};

}  // namespace network
