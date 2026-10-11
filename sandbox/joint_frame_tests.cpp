// Where a joint's Box3D frames go, from its ends' pivots
// (docs/superpowers/specs/2026-10-11-joint-constraints-design.md §2, The frame).

#include "JointFrame.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace {

using engine_core::JointKind;
using engine_core::Matrix4;
using engine_core::Vec3;

constexpr double kPi = 3.14159265358979323846;

Vec3 column(const Matrix4& m, int c) { return Vec3{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }

bool same(Vec3 a, Vec3 b) {
    return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
}

Matrix4 placed(float x, float y, float z, Vec3 axis, double degrees) {
    return engine_core::matrix4_multiply(engine_core::matrix4_translation(x, y, z),
                                         engine_core::matrix4_axis_angle(axis, degrees * kPi / 180.0));
}

}  // namespace

TEST_CASE("JF1 a Revolute's and a Spherical's frame turns about the moving end's Y, at its pivot",
          "[joint-frame]") {
    const Matrix4 base = placed(0.f, 0.f, 0.f, Vec3{0.f, 1.f, 0.f}, 0.0);
    const Matrix4 moving = placed(1.f, 2.f, 3.f, Vec3{1.f, 0.f, 0.f}, 30.0);
    for (const JointKind kind : {JointKind::Revolute, JointKind::Spherical}) {
        Matrix4 frame1{};
        Matrix4 frame2{};
        engine_core::joint_frames(kind, base, moving, frame1, frame2);
        // Box3D turns about Z: the frame's Z is the pivot's Y.
        REQUIRE(same(column(frame1, 2), column(moving, 1)));
        REQUIRE(same(column(frame1, 0), column(moving, 0)));
        REQUIRE(same(engine_core::matrix4_position(frame1), Vec3{1.f, 2.f, 3.f}));
        REQUIRE(engine_core::same_matrix4(frame1, frame2));
    }
}

TEST_CASE("JF2 a Prismatic's frame slides along the moving end's Y", "[joint-frame]") {
    const Matrix4 moving = placed(4.f, 0.f, 0.f, Vec3{0.f, 0.f, 1.f}, 90.0);
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Prismatic, engine_core::matrix4_identity(), moving, frame1, frame2);
    // Box3D slides along X: the frame's X is the pivot's Y.
    REQUIRE(same(column(frame1, 0), column(moving, 1)));
    REQUIRE(same(engine_core::matrix4_position(frame1), Vec3{4.f, 0.f, 0.f}));
    REQUIRE(engine_core::same_matrix4(frame1, frame2));
}

TEST_CASE("JF3 a Distance's frames are each end's position, unrotated", "[joint-frame]") {
    const Matrix4 base = placed(0.f, 5.f, 0.f, Vec3{0.f, 1.f, 0.f}, 40.0);
    const Matrix4 moving = placed(0.f, 3.f, 0.f, Vec3{1.f, 0.f, 0.f}, 70.0);
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Distance, base, moving, frame1, frame2);
    REQUIRE(engine_core::same_matrix4(frame1, engine_core::matrix4_translation(0.f, 5.f, 0.f)));
    REQUIRE(engine_core::same_matrix4(frame2, engine_core::matrix4_translation(0.f, 3.f, 0.f)));
}

TEST_CASE("JF4 a Wheel spins about its own Y and travels along the chassis's up", "[joint-frame]") {
    // A cylinder wheel on its side: its Y points along world -X.
    const Matrix4 wheel = placed(2.f, 0.5f, 1.f, Vec3{0.f, 0.f, 1.f}, 90.0);
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Wheel, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(same(column(frame1, 2), Vec3{-1.f, 0.f, 0.f}));  // spin: the wheel's Y
    REQUIRE(same(column(frame1, 0), Vec3{0.f, 1.f, 0.f}));   // travel and steering: the chassis's up
    REQUIRE(same(column(frame1, 1), Vec3{0.f, 0.f, -1.f}));  // Z × X
    REQUIRE(same(engine_core::matrix4_position(frame1), Vec3{2.f, 0.5f, 1.f}));
    REQUIRE(engine_core::same_matrix4(frame1, frame2));
}

TEST_CASE("JF5 a Wheel lying flat travels along the chassis's X instead", "[joint-frame]") {
    // Its axle points up, along the chassis's up: that cannot be the travel axis.
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Wheel, engine_core::matrix4_identity(),
                              engine_core::matrix4_translation(0.f, 1.f, 0.f), frame1, frame2);
    REQUIRE(same(column(frame1, 2), Vec3{0.f, 1.f, 0.f}));
    REQUIRE(same(column(frame1, 0), Vec3{1.f, 0.f, 0.f}));
}

TEST_CASE("JF6 the axes drawn: one for a hinge, slider, or ball; two for a wheel; none for a distance",
          "[joint-frame]") {
    const Matrix4 wheel = placed(0.f, 0.f, 0.f, Vec3{0.f, 0.f, 1.f}, 90.0);
    Vec3 axes[2]{};
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Revolute, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(engine_core::joint_axes(JointKind::Revolute, frame1, axes) == 1);
    REQUIRE(same(axes[0], column(wheel, 1)));
    engine_core::joint_frames(JointKind::Prismatic, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(engine_core::joint_axes(JointKind::Prismatic, frame1, axes) == 1);
    REQUIRE(same(axes[0], column(wheel, 1)));
    engine_core::joint_frames(JointKind::Wheel, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(engine_core::joint_axes(JointKind::Wheel, frame1, axes) == 2);
    REQUIRE(same(axes[0], Vec3{-1.f, 0.f, 0.f}));
    REQUIRE(same(axes[1], Vec3{0.f, 1.f, 0.f}));
    REQUIRE(engine_core::joint_axes(JointKind::Distance, frame1, axes) == 0);
}
