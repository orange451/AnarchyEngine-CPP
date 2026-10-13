#pragma once

#include "Socket.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace network {

// How bad the network is. All zero is a perfect one.
struct ConditionerSettings {
    double latency_seconds = 0.0;
    double jitter_seconds = 0.0;
    float loss = 0.0f;
    float duplicate = 0.0f;
    double bandwidth_bytes_per_second = 0.0;
    bool active() const {
        return latency_seconds > 0.0 || jitter_seconds > 0.0 || loss > 0.0f || duplicate > 0.0f ||
               bandwidth_bytes_per_second > 0.0;
    }
};

// Wraps a socket and delays, jitters, drops, duplicates, and caps what is
// sent through it. With every setting zero it passes datagrams straight
// through and holds nothing. Only sending is conditioned; wrap both
// endpoints to condition both directions.
class Conditioner : public ISocket {
public:
    explicit Conditioner(std::unique_ptr<ISocket> inner, std::uint32_t seed = 1);

    ConditionerSettings settings;

    // Hands every held datagram that is due to the inner socket.
    void advance(double now);
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override;
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override;
    bool failed() const override { return inner_->failed(); }
    std::size_t held() const { return held_.size(); }

private:
    struct Held {
        double due;
        Address to;
        std::vector<std::uint8_t> bytes;
    };
    void hold(const Address& to, const std::uint8_t* bytes, std::size_t length);

    std::unique_ptr<ISocket> inner_;
    std::vector<Held> held_;
    std::mt19937 random_;
    double now_ = 0.0;
    double second_start_ = 0.0;
    double bytes_this_second_ = 0.0;
};

}  // namespace network
