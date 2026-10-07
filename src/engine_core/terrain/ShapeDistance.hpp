#pragma once

// Signed-distance shapes used to sculpt a Terrain's voxels, in Terrain-local
// space. Every shape carries its own inverse frame so shape_distance never
// recomputes it per cell; prepare_shape fills it in once per edit.

#include "Matrix4.hpp"
#include "Vector3.hpp"

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

// Signed distance from local point p to the shape's surface, in studs; negative inside.
float shape_distance(const Shape& shape, Vec3 p);
// The shape's local-space bounds, grown by margin on every side.
void shape_bounds(const Shape& shape, float margin, Vec3& min, Vec3& max);

}  // namespace engine_core::terrain
