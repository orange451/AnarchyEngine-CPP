#pragma once

#include "types.hpp"

namespace engine_core {

// The Box3D joint a JointConstraint makes
// (docs/superpowers/specs/2026-10-11-joint-constraints-design.md).
enum class JointKind { Revolute, Prismatic, Distance, Spherical, Wheel };

// Where a joint's frame is on each side when it starts, in world space, as
// Box3D's joint of kind wants it (spec §2, The frame). pivot1 and pivot2 are
// the ends' pivots (Assemblies::end_pivot), scale removed. The engine's axis
// is a pivot's Y: a Revolute's and a Spherical's frame turns Box3D's Z onto
// pivot2's Y, a Prismatic's turns Box3D's X onto it, and a Wheel's spins
// about pivot2's Y and travels along pivot1's Y made perpendicular to that
// (pivot1's X when the two are parallel). Those put both frames at one
// frame W at pivot2. A Distance puts each at its own end's position,
// unrotated. Pure.
void joint_frames(JointKind kind, const Matrix4& pivot1, const Matrix4& pivot2, Matrix4& frame1, Matrix4& frame2);

// The axes to draw at a joint's frame1, as unit vectors: the turning or
// sliding axis for a Revolute, Spherical, or Prismatic; the spin axis, then
// the suspension and steering axis, for a Wheel; none for a Distance.
// Returns how many it wrote.
int joint_axes(JointKind kind, const Matrix4& frame1, Vec3 axes[2]);

}  // namespace engine_core
