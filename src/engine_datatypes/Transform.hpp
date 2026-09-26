#pragma once

namespace engine_core {

// Column-major 4x4. Translation lives in m[12], m[13], m[14].
struct Transform {
    float m[16] = {};
};

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
