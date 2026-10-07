#include "terrain/ShapeDistance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine_core::terrain {
namespace {

Vec3 to_frame(const Matrix4& inverse, Vec3 p) {
    const float* m = inverse.m;
    return Vec3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

float length3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

float box_distance(Vec3 q, Vec3 half) {
    const float dx = std::fabs(q.x) - half.x;
    const float dy = std::fabs(q.y) - half.y;
    const float dz = std::fabs(q.z) - half.z;
    const float outside = length3(std::max(dx, 0.f), std::max(dy, 0.f), std::max(dz, 0.f));
    const float inside = std::min(std::max(dx, std::max(dy, dz)), 0.f);
    return outside + inside;
}

}  // namespace

void prepare_shape(Shape& shape) { shape.inverse = matrix4_inverse(shape.frame); }

float shape_distance(const Shape& shape, Vec3 p) {
    if (shape.kind == Shape::Kind::Ball) {
        return length3(p.x - shape.center.x, p.y - shape.center.y, p.z - shape.center.z) - shape.radius;
    }
    const Vec3 q = to_frame(shape.inverse, p);
    const Vec3 half{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.z * 0.5f};
    switch (shape.kind) {
    case Shape::Kind::Block:
        return box_distance(q, half);
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
        return std::max(box_distance(q, half), plane);
    }
    default:
        return 0.f;
    }
}

void shape_bounds(const Shape& shape, float margin, Vec3& min, Vec3& max) {
    if (shape.kind == Shape::Kind::Ball) {
        const float r = shape.radius + margin;
        min = Vec3{shape.center.x - r, shape.center.y - r, shape.center.z - r};
        max = Vec3{shape.center.x + r, shape.center.y + r, shape.center.z + r};
        return;
    }
    const Vec3 half = shape.kind == Shape::Kind::Cylinder
                           ? Vec3{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.x * 0.5f}
                           : Vec3{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.z * 0.5f};
    min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
    max = Vec3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
               std::numeric_limits<float>::lowest()};
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
        const Vec3 world = matrix4_point(shape.frame, corner);
        min.x = std::min(min.x, world.x);
        min.y = std::min(min.y, world.y);
        min.z = std::min(min.z, world.z);
        max.x = std::max(max.x, world.x);
        max.y = std::max(max.y, world.y);
        max.z = std::max(max.z, world.z);
    }
    min.x -= margin;
    min.y -= margin;
    min.z -= margin;
    max.x += margin;
    max.y += margin;
    max.z += margin;
}

}  // namespace engine_core::terrain
