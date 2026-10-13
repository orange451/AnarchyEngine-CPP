#pragma once

#include "Vector3.hpp"

#include <cmath>

// Float Vec3 helpers for the particle simulation; Vec3 itself has no operators.
namespace engine_core::particles {

inline Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }

// a at length 1; up for a zero vector.
inline Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 0.f ? scale(a, 1.f / l) : Vec3{0.f, 1.f, 0.f};
}

// a turned by angle radians about the unit axis (Rodrigues).
inline Vec3 rotate(Vec3 a, Vec3 axis, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return add(add(scale(a, c), scale(cross(axis, a), s)), scale(axis, dot(axis, a) * (1.f - c)));
}

}  // namespace engine_core::particles
