#pragma once

#include "Enum.hpp"
#include "Matrix4.hpp"
#include "Vector3.hpp"

#include <vector>

namespace engine_core::particles {

// A selected emitter's shape as the Scene View outlines it: line segments
// in world space, two points a segment, x y z r g b a per point, appended to
// out. frame and half are emitter_extent's. All-zero half (a point) draws a
// small cross at the frame's origin.
void particle_shape_lines(ParticleShape shape, float arc_degrees, const Matrix4& frame, Vec3 half,
                          std::vector<float>& out);

}  // namespace engine_core::particles
