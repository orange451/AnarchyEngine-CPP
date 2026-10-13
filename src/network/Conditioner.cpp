#include "Conditioner.hpp"

#include <algorithm>

namespace network {

Conditioner::Conditioner(std::unique_ptr<ISocket> inner, std::uint32_t seed)
    : inner_(std::move(inner)), random_(seed) {}

void Conditioner::advance(double now) {
    now_ = now;
    if (now_ - second_start_ >= 1.0) {
        second_start_ = now_;
        bytes_this_second_ = 0.0;
    }
    if (held_.empty()) return;
    // Due ones leave in due order, so jitter reorders exactly what it should.
    std::stable_sort(held_.begin(), held_.end(), [](const Held& a, const Held& b) { return a.due < b.due; });
    std::size_t released = 0;
    while (released < held_.size() && held_[released].due <= now_) {
        inner_->send(held_[released].to, held_[released].bytes.data(), held_[released].bytes.size());
        ++released;
    }
    held_.erase(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(released));
}

void Conditioner::hold(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    double delay = settings.latency_seconds;
    if (settings.jitter_seconds > 0.0) {
        std::uniform_real_distribution<double> jitter(-settings.jitter_seconds, settings.jitter_seconds);
        delay += jitter(random_);
    }
    held_.push_back({now_ + std::max(0.0, delay), to, std::vector<std::uint8_t>(bytes, bytes + length)});
}

bool Conditioner::send(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    if (!settings.active()) return inner_->send(to, bytes, length);
    if (settings.bandwidth_bytes_per_second > 0.0) {
        if (bytes_this_second_ + static_cast<double>(length) > settings.bandwidth_bytes_per_second) return false;
        bytes_this_second_ += static_cast<double>(length);
    }
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    // A lost datagram was sent, as far as the caller can tell.
    if (settings.loss > 0.0f && chance(random_) < settings.loss) return true;
    const int copies = (settings.duplicate > 0.0f && chance(random_) < settings.duplicate) ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        if (settings.latency_seconds > 0.0 || settings.jitter_seconds > 0.0) {
            hold(to, bytes, length);
        } else {
            inner_->send(to, bytes, length);
        }
    }
    return true;
}

std::size_t Conditioner::receive(Address& from, std::uint8_t* buffer, std::size_t capacity) {
    return inner_->receive(from, buffer, capacity);
}

}  // namespace network
