#pragma once

#include <cstdint>

namespace engine_core::particles {

// PCG32 (O'Neill): small, fast, and the same sequence on every platform, so
// a seeded emitter replays exactly.
class Pcg32 {
public:
    explicit Pcg32(std::uint64_t seed = 0x853c49e6748fea9bULL, std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
        reseed(seed, stream);
    }

    void reseed(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
        state_ = 0u;
        inc_ = (stream << 1u) | 1u;
        next();
        state_ += seed;
        next();
    }

    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    // From 0 up to, not including, 1.
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float low, float high) { return low + (high - low) * uniform(); }

private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 0;
};

}  // namespace engine_core::particles
