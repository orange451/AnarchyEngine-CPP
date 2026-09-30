#pragma once

#include "Vector3.hpp"

#include <cstring>

struct lua_State;

namespace engine_core {

// Column-major 4x4, as GLSL stores one: m[column * 4 + row]. Translation lives
// in m[12], m[13], m[14]. The upper 3x3's columns are the right, up, and back
// axes, so the look direction is the third column negated, as in Roblox.
struct Matrix4 {
    float m[16] = {};
};

// Bitwise, so a NaN matches itself and -0 differs from 0.
inline bool same_matrix4(const Matrix4& a, const Matrix4& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

inline Matrix4 matrix4_identity() {
    Matrix4 out;
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.f;
    return out;
}

inline Matrix4 matrix4_translation(float x, float y, float z) {
    Matrix4 out = matrix4_identity();
    out.m[12] = x;
    out.m[13] = y;
    out.m[14] = z;
    return out;
}

inline Vec3 matrix4_position(const Matrix4& value) { return Vec3{value.m[12], value.m[13], value.m[14]}; }

// Roblox's Enum.RotationOrder, by value. XYZ is Rx * Ry * Rz: Z applies first.
enum class RotationOrder { XYZ = 0, XZY = 1, YZX = 2, YXZ = 3, ZXY = 4, ZYX = 5 };

// a * b: b's transform first, then a's.
Matrix4 matrix4_multiply(const Matrix4& a, const Matrix4& b);
// The full 4x4 inverse. A singular matrix gives components that are not finite.
Matrix4 matrix4_inverse(const Matrix4& value);
// value * (point, 1), divided by w when the bottom row is not (0, 0, 0, 1).
Vec3 matrix4_point(const Matrix4& value, Vec3 point);
// The upper 3x3 times direction: rotation and scale, no translation.
Vec3 matrix4_vector(const Matrix4& value, Vec3 direction);

// A rotation of angle radians about axis, right-handed. A zero axis is no rotation.
Matrix4 matrix4_axis_angle(Vec3 axis, double angle);
// A rotation from a quaternion (x, y, z, w), normalized here. A zero quaternion is no rotation.
Matrix4 matrix4_from_quaternion(double x, double y, double z, double w);
// The rotation of value, with any scale taken out, as a unit quaternion (x, y, z, w) with w >= 0.
void matrix4_to_quaternion(const Matrix4& value, double out[4]);
// Rotations about X, Y, and Z by rx, ry, and rz, composed in order.
Matrix4 matrix4_from_euler(double rx, double ry, double rz, RotationOrder order);
// The angles matrix4_from_euler would take to build value's rotation, as rx, ry, rz.
void matrix4_to_euler(const Matrix4& value, RotationOrder order, double out[3]);
// At at, looking toward target, with up as near to up as it can be. When the
// look is along up, the right axis comes from +Z instead. target at at looks down -Z.
Matrix4 matrix4_look_at(Vec3 at, Vec3 target, Vec3 up);
// Rotation, scale, and position, with the rotation made orthonormal: the look
// direction is kept, then up is squared to it. The bottom row is (0, 0, 0, 1).
Matrix4 matrix4_orthonormalize(const Matrix4& value);
// From a at alpha 0 to b at alpha 1: position and scale linearly, rotation
// along the shortest arc. alpha 1 is exactly b.
Matrix4 matrix4_lerp(const Matrix4& a, const Matrix4& b, double alpha);

// Roblox's CFrame as Matrix4, installed into the same state as the rest of the
// script API. A Matrix4 is a userdata: typeof is "Matrix4", * composes two
// or moves a Vector3, + and - move it by a Vector3, and == compares it.
// open_vector3 and open_enum must have run on the state.
void open_matrix4(lua_State* state);

// Pushes a new Matrix4. open_matrix4 must have run on the state.
void push_matrix4(lua_State* state, const Matrix4& value);

// Null when the value at index is not a Matrix4.
const Matrix4* to_matrix4(lua_State* state, int index);

}  // namespace engine_core
