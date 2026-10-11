// The Scene View's lines for a selected constraint: a segment between its
// pivots, and a small cross at each.

#include "ConstraintLines.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

using engine_core::ConstraintPivots;

constexpr std::size_t kPoint = 7;  // xyz rgba

engine_core::DraggerView view() {
    engine_core::DraggerView out;
    out.camera = engine_core::matrix4_identity();
    out.fov_degrees = 70.f;
    out.size = engine_core::Vec2{800.f, 600.f};
    return out;
}

ConstraintPivots weld() {
    ConstraintPivots pivots;
    pivots.active = true;
    pivots.has1 = true;
    pivots.pivot1 = engine_core::matrix4_translation(0.f, 0.f, -10.f);
    pivots.has2 = true;
    pivots.pivot2 = engine_core::matrix4_translation(2.f, 0.f, -10.f);
    return pivots;
}

bool colored(const std::vector<float>& lines, float r, float g, float b) {
    for (std::size_t at = 0; at + kPoint <= lines.size(); at += kPoint) {
        if (std::fabs(lines[at + 3] - r) > 1e-4f || std::fabs(lines[at + 4] - g) > 1e-4f ||
            std::fabs(lines[at + 5] - b) > 1e-4f) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("VZ1 an active weld is a segment between its pivots and a cross at each", "[assembly][viz]") {
    std::vector<float> lines;
    engine_core::constraint_lines(weld(), view(), lines);
    // One segment and three axes per cross, two points each.
    REQUIRE(lines.size() == kPoint * 2 * (1 + 3 + 3));
    REQUIRE(lines[0] == 0.f);
    REQUIRE(lines[2] == -10.f);
    REQUIRE(lines[kPoint] == 2.f);
    REQUIRE(colored(lines, 0.35f, 0.75f, 1.f));
    ConstraintPivots rigid = weld();
    rigid.rigid = true;
    // It appends, as the Scene View gathers every selected constraint's lines.
    engine_core::constraint_lines(rigid, view(), lines);
    REQUIRE(lines.size() == kPoint * 2 * 7 * 2);
    lines.erase(lines.begin(), lines.begin() + kPoint * 2 * 7);
    REQUIRE(colored(lines, 1.f, 0.7f, 0.2f));
}

TEST_CASE("VZ2 an inactive constraint draws the same lines in gray", "[assembly][viz]") {
    ConstraintPivots off = weld();
    off.active = false;
    std::vector<float> lines;
    engine_core::constraint_lines(off, view(), lines);
    REQUIRE(lines.size() == kPoint * 2 * 7);
    REQUIRE(colored(lines, 0.55f, 0.55f, 0.55f));
}

TEST_CASE("VZ3 with one end unset, only that end's cross is drawn", "[assembly][viz]") {
    ConstraintPivots half = weld();
    half.has2 = false;
    std::vector<float> lines;
    engine_core::constraint_lines(half, view(), lines);
    REQUIRE(lines.size() == kPoint * 2 * 3);
    // The cross is a few pixels across at its depth, not a fixed world size.
    const float arm = std::fabs(lines[kPoint] - lines[0]);
    REQUIRE(arm > 0.01f);
    REQUIRE(arm < 1.f);
}

TEST_CASE("VZ4 an active NoCollisionConstraint is drawn red", "[assembly][viz]") {
    ConstraintPivots apart = weld();
    apart.no_collide = true;
    std::vector<float> lines;
    engine_core::constraint_lines(apart, view(), lines);
    REQUIRE(lines.size() == kPoint * 2 * 7);
    REQUIRE(colored(lines, 1.f, 0.35f, 0.35f));
}
