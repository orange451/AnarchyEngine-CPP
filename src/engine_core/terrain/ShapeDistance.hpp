#pragma once

// Signed-distance shapes used to sculpt a Terrain's voxels, in Terrain-local
// space. Every shape carries its own inverse frame so shape_distance never
// recomputes it per cell; prepare_shape fills it in once per edit.

#include "Matrix4.hpp"
#include "Vector3.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::terrain {

// A shape in Terrain-local space. frame places a box, cylinder, or wedge
// (rigid); ball uses center.
struct Shape {
    enum class Kind { Ball, Block, Cylinder, Wedge };
    Kind kind = Kind::Ball;
    Vec3 center{};
    float radius = 0.f;
    Matrix4 frame = matrix4_identity();
    Matrix4 inverse = matrix4_identity();
    Vec3 size{};       // Block and Wedge: full size; Cylinder: (2r, height, 2r)
};

// Fills in what shape_distance needs, once per edit.
void prepare_shape(Shape& shape);   // sets shape.inverse = matrix4_inverse(shape.frame)

// The shape's local-space bounds, grown by margin on every side.
void shape_bounds(const Shape& shape, float margin, Vec3& min, Vec3& max);

namespace detail {

inline Vec3 shape_to_frame(const Matrix4& inverse, Vec3 p) {
    const float* m = inverse.m;
    return Vec3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

inline float shape_length3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

inline float shape_box_distance(Vec3 q, Vec3 half) {
    const float dx = std::fabs(q.x) - half.x;
    const float dy = std::fabs(q.y) - half.y;
    const float dz = std::fabs(q.z) - half.z;
    const float outside = shape_length3(std::max(dx, 0.f), std::max(dy, 0.f), std::max(dz, 0.f));
    const float inside = std::min(std::max(dx, std::max(dy, dz)), 0.f);
    return outside + inside;
}

}  // namespace detail

// Signed distance from local point p to the shape's surface, in studs;
// negative inside. Inline: called once per cell of every edit (VoxelVolume's
// hot path), so a cross-translation-unit call per cell is worth avoiding.
inline float shape_distance(const Shape& shape, Vec3 p) {
    using namespace detail;
    if (shape.kind == Shape::Kind::Ball) {
        return shape_length3(p.x - shape.center.x, p.y - shape.center.y, p.z - shape.center.z) - shape.radius;
    }
    const Vec3 q = shape_to_frame(shape.inverse, p);
    const Vec3 half{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.z * 0.5f};
    switch (shape.kind) {
    case Shape::Kind::Block:
        return shape_box_distance(q, half);
    case Shape::Kind::Cylinder: {
        const float radial = std::sqrt(q.x * q.x + q.z * q.z) - half.x;
        const float axial = std::fabs(q.y) - half.y;
        const float outside = std::sqrt(std::max(radial, 0.f) * std::max(radial, 0.f) +
                                        std::max(axial, 0.f) * std::max(axial, 0.f));
        return outside + std::min(std::max(radial, axial), 0.f);
    }
    case Shape::Kind::Wedge: {
        // Solid under the plane from the bottom front edge (y = -h, z = -d)
        // to the top back edge (y = +h, z = +d).
        const float nl = std::sqrt(half.z * half.z + half.y * half.y);
        const float plane = nl > 0.f ? (q.y * half.z - q.z * half.y) / nl : 0.f;
        return std::max(shape_box_distance(q, half), plane);
    }
    default:
        return 0.f;
    }
}

}  // namespace engine_core::terrain
