#pragma once

#include <cstring>

namespace engine_core {

// Column-major 4x4. Translation lives in m[12], m[13], m[14].
struct Transform {
    float m[16] = {};
};

// Bitwise, so a NaN matches itself and -0 differs from 0.
inline bool same_transform(const Transform& a, const Transform& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

inline Transform transform_identity() {
    Transform out;
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.f;
    return out;
}

inline Transform transform_translation(float x, float y, float z) {
    Transform out = transform_identity();
    out.m[12] = x;
    out.m[13] = y;
    out.m[14] = z;
    return out;
}

}  // namespace engine_core
