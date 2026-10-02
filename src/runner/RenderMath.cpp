#include "RenderMath.hpp"

namespace runner {

using engine_core::Matrix4;
using engine_core::Vec3;

Matrix4 Perspective(float fovYDegrees, float aspect, float nearZ, float farZ) {
    const float f = 1.f / std::tan(fovYDegrees * 0.5f * kDegree);
    Matrix4 out;
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = (farZ + nearZ) / (nearZ - farZ);
    out.m[11] = -1.f;
    out.m[14] = 2.f * farZ * nearZ / (nearZ - farZ);
    return out;
}

Matrix4 Orthographic(float left, float right, float bottom, float top, float nearZ, float farZ) {
    Matrix4 out;
    out.m[0] = 2.f / (right - left);
    out.m[5] = 2.f / (top - bottom);
    out.m[10] = -2.f / (farZ - nearZ);
    out.m[12] = -(right + left) / (right - left);
    out.m[13] = -(top + bottom) / (top - bottom);
    out.m[14] = -(farZ + nearZ) / (farZ - nearZ);
    out.m[15] = 1.f;
    return out;
}

Matrix4 LookAtView(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = Normalize(Sub(target, eye));
    Vec3 s = Cross(f, up);
    if (Dot(s, s) < 1e-8f) {
        s = Cross(f, std::fabs(f.z) < 0.9f ? Vec3{0.f, 0.f, 1.f} : Vec3{1.f, 0.f, 0.f});
    }
    s = Normalize(s);
    const Vec3 u = Cross(s, f);
    Matrix4 out = engine_core::matrix4_identity();
    out.m[0] = s.x;
    out.m[4] = s.y;
    out.m[8] = s.z;
    out.m[1] = u.x;
    out.m[5] = u.y;
    out.m[9] = u.z;
    out.m[2] = -f.x;
    out.m[6] = -f.y;
    out.m[10] = -f.z;
    out.m[12] = -Dot(s, eye);
    out.m[13] = -Dot(u, eye);
    out.m[14] = Dot(f, eye);
    return out;
}

}  // namespace runner
