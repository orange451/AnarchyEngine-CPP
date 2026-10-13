#pragma once

#include <cstdint>

namespace network {

// 16-bit sequence numbers wrap: 0 comes after 65535. a is later than b when
// their distance the short way round points forward.
inline bool sequence_greater_than(std::uint16_t a, std::uint16_t b) {
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

// a - b, taken the short way round.
inline std::int16_t sequence_diff(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
}

}  // namespace network
