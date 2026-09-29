#pragma once

#include <cmath>

// Per-component rules Vector2 and Vector3 share.
namespace engine_core {
namespace vector_math {

// Roblox scales the tolerance by |component| + 1, using the receiver's component.
// An exact match is equal, including infinities.
inline bool fuzzy_component(float left, float right, double epsilon) {
    const double a = left;
    const double b = right;
    return a == b || std::fabs(a - b) <= (std::fabs(a) + 1.0) * epsilon;
}

inline float sign_of(float value) {
    if (value > 0.f) {
        return 1.f;
    }
    if (value < 0.f) {
        return -1.f;
    }
    return 0.f;
}

// Exactly to at alpha 1, where from + (to - from) could round.
inline float lerp_component(float from, float to, double alpha) {
    if (alpha == 1.0) {
        return to;
    }
    return static_cast<float>(from + (to - from) * alpha);
}

}  // namespace vector_math
}  // namespace engine_core
