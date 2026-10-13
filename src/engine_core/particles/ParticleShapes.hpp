#pragma once

#include "Enum.hpp"
#include "Vector3.hpp"
#include "particles/ParticleRandom.hpp"

namespace engine_core::particles {

// A point on an emitter's shape, in the shape's own space.
struct ShapeSample {
    Vec3 position;   // the shape's centre is the origin
    Vec3 normal;     // unit, outward there; up for a point or a volume
};

// A random point on shape. half is half the box the shape fits in: a
// sphere is the ellipsoid that fills it, a cylinder, cone, or disc takes X
// and Z as its width and Y as its height, and a cone opens along +Y. All
// zero is a point. arc_degrees keeps Sphere, Hemisphere, Cylinder, Cone,
// and Disc to that much of a turn about +Y, from +X toward +Z.
ShapeSample sample_shape(ParticleShape shape, ParticleEmitFrom from, Vec3 half, float arc_degrees, Pcg32& rng);

// A direction within spread degrees of the unit dir on two axes. For +Y the
// first tilts toward X and the second toward Z.
Vec3 spread_direction(Vec3 dir, float spread_x_deg, float spread_y_deg, Pcg32& rng);

}  // namespace engine_core::particles
