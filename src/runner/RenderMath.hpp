#pragma once

#include "Matrix4.hpp"

#include <cmath>

// Projections and views for the Scene View and its shadow maps. Column-major,
// as Transform and GLSL store them, right-handed and Y up, in OpenGL's clip
// space: depth -1 at the near plane, 1 at the far. Needs no GL context.
namespace runner {

constexpr float kDegree = 0.01745329252f;

// Vec3 has no operators; these are the few the matrices need.
inline engine_core::Vec3 Add(engine_core::Vec3 a, engine_core::Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline engine_core::Vec3 Sub(engine_core::Vec3 a, engine_core::Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline engine_core::Vec3 Scale(engine_core::Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline float Dot(engine_core::Vec3 a, engine_core::Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline engine_core::Vec3 Cross(engine_core::Vec3 a, engine_core::Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(engine_core::Vec3 v) { return std::sqrt(Dot(v, v)); }
// A zero vector stays zero.
inline engine_core::Vec3 Normalize(engine_core::Vec3 v) {
    const float length = Length(v);
    return length > 0.f ? Scale(v, 1.f / length) : v;
}

engine_core::Matrix4 Perspective(float fovYDegrees, float aspect, float nearZ, float farZ);
// A box in view space: x from left to right, y from bottom to top, and
// depth from nearZ to farZ down -Z.
engine_core::Matrix4 Orthographic(float left, float right, float bottom, float top, float nearZ, float farZ);
// The view from eye toward target, turned so its +Y is as near up as it can
// be. It looks down its -Z. An up along the look is swapped for +Z, or +X
// when the look is along Z, so looking straight down still has a view.
engine_core::Matrix4 LookAtView(engine_core::Vec3 eye, engine_core::Vec3 target, engine_core::Vec3 up);

}  // namespace runner
