#pragma once

#include "DraggerMath.hpp"
#include "types.hpp"

#include <vector>

namespace engine_core {

// What the Scene View reads under the lock for one selected constraint: its
// kind, whether it is Active, and the pivots of the ends that resolve
// (Assemblies::end_pivot).
struct ConstraintPivots {
    bool rigid = false;
    // A NoCollisionConstraint: drawn red.
    bool no_collide = false;
    // A JointConstraint: drawn green, with a line along each of its axes
    // (joint_axes) through axis_origin, its frame's position.
    bool joint = false;
    int axis_count = 0;
    Vec3 axis_origin{};
    Vec3 axes[2]{};
    bool active = false;
    bool has1 = false;
    Matrix4 pivot1{};
    bool has2 = false;
    Matrix4 pivot2{};
};

// Its lines, appended to out as Renderer::setToolLines takes them (7 floats a
// point, xyz rgba, two points a line): a segment between the two pivots when
// both resolve, and a cross of three short axes at each that does, a few
// pixels across at its depth (handle_scale). A WeldConstraint is light blue,
// a RigidConstraint orange, a NoCollisionConstraint red, a joint green with
// its axes, and an inactive one gray. Pure: no DataModel.
void constraint_lines(const ConstraintPivots& constraint, const DraggerView& view, std::vector<float>& out);

}  // namespace engine_core
