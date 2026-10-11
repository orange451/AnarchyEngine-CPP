#include "JointFrame.hpp"

#include <cmath>

namespace engine_core {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;
// Shorter than this, a direction made perpendicular is too close to the axis to use.
constexpr float kShortest = 0.001f;

Vec3 column(const Matrix4& m, int c) { return Vec3{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 scaled(Vec3 v, float s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
Vec3 minus(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }

Vec3 unit(Vec3 v) {
    const float l = length(v);
    return l > 0.f ? scaled(v, 1.f / l) : v;
}

// v with its part along the unit axis taken out, normalized. False when what
// is left is shorter than kShortest.
bool perpendicular(Vec3 v, Vec3 axis, Vec3& out) {
    const Vec3 rest = minus(v, scaled(axis, dot(v, axis)));
    const float l = length(rest);
    if (!(l >= kShortest)) {
        return false;
    }
    out = scaled(rest, 1.f / l);
    return true;
}

Matrix4 from_columns(Vec3 x, Vec3 y, Vec3 z, Vec3 at) {
    Matrix4 out = matrix4_identity();
    const Vec3 columns[3] = {x, y, z};
    for (int c = 0; c < 3; ++c) {
        out.m[c * 4] = columns[c].x;
        out.m[c * 4 + 1] = columns[c].y;
        out.m[c * 4 + 2] = columns[c].z;
    }
    out.m[12] = at.x;
    out.m[13] = at.y;
    out.m[14] = at.z;
    return out;
}

}  // namespace

void joint_frames(JointKind kind, const Matrix4& pivot1, const Matrix4& pivot2, Matrix4& frame1, Matrix4& frame2) {
    switch (kind) {
        case JointKind::Distance: {
            const Vec3 a = matrix4_position(pivot1);
            const Vec3 b = matrix4_position(pivot2);
            frame1 = matrix4_translation(a.x, a.y, a.z);
            frame2 = matrix4_translation(b.x, b.y, b.z);
            return;
        }
        case JointKind::Revolute:
        case JointKind::Spherical:
            // -90° about X turns Z onto Y.
            frame1 = matrix4_multiply(pivot2, matrix4_axis_angle(Vec3{1.f, 0.f, 0.f}, -kHalfPi));
            break;
        case JointKind::Prismatic:
            // +90° about Z turns X onto Y.
            frame1 = matrix4_multiply(pivot2, matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, kHalfPi));
            break;
        case JointKind::Wheel: {
            const Vec3 spin = unit(column(pivot2, 1));
            Vec3 travel{1.f, 0.f, 0.f};
            if (!perpendicular(column(pivot1, 1), spin, travel) && !perpendicular(column(pivot1, 0), spin, travel)) {
                // A rotation's X and Y are never both along one axis; a degenerate pivot1 lands here.
                perpendicular(column(pivot2, 0), spin, travel);
            }
            frame1 = from_columns(travel, cross(spin, travel), spin, matrix4_position(pivot2));
            break;
        }
    }
    frame2 = frame1;
}

int joint_axes(JointKind kind, const Matrix4& frame1, Vec3 axes[2]) {
    switch (kind) {
        case JointKind::Revolute:
        case JointKind::Spherical:
            axes[0] = unit(column(frame1, 2));
            return 1;
        case JointKind::Prismatic:
            axes[0] = unit(column(frame1, 0));
            return 1;
        case JointKind::Wheel:
            axes[0] = unit(column(frame1, 2));
            axes[1] = unit(column(frame1, 0));
            return 2;
        case JointKind::Distance:
            return 0;
    }
    return 0;
}

}  // namespace engine_core
