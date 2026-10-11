#include "ConstraintLines.hpp"

namespace engine_core {
namespace {

// How many pixels each arm of a cross reaches from its pivot.
constexpr float kCrossPixels = 8.f;

struct Rgb {
    float r;
    float g;
    float b;
};

void point(std::vector<float>& out, Vec3 at, Rgb color) {
    out.insert(out.end(), {at.x, at.y, at.z, color.r, color.g, color.b, 1.f});
}

void line(std::vector<float>& out, Vec3 from, Vec3 to, Rgb color) {
    point(out, from, color);
    point(out, to, color);
}

void cross(std::vector<float>& out, const Matrix4& pivot, const DraggerView& view, Rgb color) {
    const Vec3 at = matrix4_position(pivot);
    const float arm = kCrossPixels * handle_scale(view, at);
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3 along = matrix4_vector(pivot, Vec3{axis == 0 ? arm : 0.f, axis == 1 ? arm : 0.f,
                                                      axis == 2 ? arm : 0.f});
        line(out, Vec3{at.x - along.x, at.y - along.y, at.z - along.z},
             Vec3{at.x + along.x, at.y + along.y, at.z + along.z}, color);
    }
}

}  // namespace

void constraint_lines(const ConstraintPivots& constraint, const DraggerView& view, std::vector<float>& out) {
    const Rgb color = !constraint.active ? Rgb{0.55f, 0.55f, 0.55f}
                      : constraint.rigid ? Rgb{1.f, 0.7f, 0.2f}
                                         : Rgb{0.35f, 0.75f, 1.f};
    if (constraint.has1 && constraint.has2) {
        line(out, matrix4_position(constraint.pivot1), matrix4_position(constraint.pivot2), color);
    }
    if (constraint.has1) {
        cross(out, constraint.pivot1, view, color);
    }
    if (constraint.has2) {
        cross(out, constraint.pivot2, view, color);
    }
}

}  // namespace engine_core
