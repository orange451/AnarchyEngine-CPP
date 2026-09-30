// Matrix4's C++ math: composition, inverse, rotations and their conversions.
#include "Matrix4.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace {

using engine_core::Matrix4;
using engine_core::RotationOrder;
using engine_core::Vec3;

bool near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }

bool near(const Matrix4& a, const Matrix4& b, float tolerance = 1e-5f) {
    for (int index = 0; index < 16; ++index) {
        if (!near(a.m[index], b.m[index], tolerance)) {
            return false;
        }
    }
    return true;
}

bool near(Vec3 a, Vec3 b, float tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

// A turned, moved, and scaled matrix, so nothing below leans on orthonormality.
Matrix4 posed() {
    Matrix4 value = engine_core::matrix4_multiply(engine_core::matrix4_translation(3.f, -2.f, 7.f),
                                                  engine_core::matrix4_from_euler(0.4, -1.1, 2.3, RotationOrder::XYZ));
    for (int row = 0; row < 3; ++row) {
        value.m[row] *= 2.f;
        value.m[8 + row] *= 0.5f;
    }
    return value;
}

}  // namespace

TEST_CASE("Matrix4 times its inverse is the identity", "[matrix4]") {
    const Matrix4 value = posed();
    REQUIRE(near(engine_core::matrix4_multiply(value, engine_core::matrix4_inverse(value)), engine_core::matrix4_identity()));
    REQUIRE(near(engine_core::matrix4_multiply(engine_core::matrix4_inverse(value), value), engine_core::matrix4_identity()));
}

TEST_CASE("Matrix4 composes right to left and moves points and vectors", "[matrix4]") {
    const Matrix4 turn = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, 3.14159265358979323846 / 2);
    const Matrix4 move = engine_core::matrix4_translation(1.f, 2.f, 3.f);
    const Matrix4 both = engine_core::matrix4_multiply(move, turn);
    // A quarter turn about +Y takes +X to -Z, then the move applies.
    REQUIRE(near(engine_core::matrix4_point(both, Vec3{1.f, 0.f, 0.f}), Vec3{1.f, 2.f, 2.f}));
    REQUIRE(near(engine_core::matrix4_vector(both, Vec3{1.f, 0.f, 0.f}), Vec3{0.f, 0.f, -1.f}));
    REQUIRE(near(engine_core::matrix4_point(engine_core::matrix4_inverse(both), Vec3{1.f, 2.f, 2.f}), Vec3{1.f, 0.f, 0.f}));
}

TEST_CASE("Matrix4 point transform divides by w for a projective matrix", "[matrix4]") {
    Matrix4 value = engine_core::matrix4_identity();
    value.m[15] = 2.f;
    REQUIRE(near(engine_core::matrix4_point(value, Vec3{2.f, 4.f, 6.f}), Vec3{1.f, 2.f, 3.f}));
}

TEST_CASE("Matrix4 Euler angles round-trip in every rotation order", "[matrix4]") {
    for (int order = 0; order < 6; ++order) {
        const auto rotation_order = static_cast<RotationOrder>(order);
        const Matrix4 value = engine_core::matrix4_from_euler(0.3, -0.7, 1.2, rotation_order);
        double angles[3];
        engine_core::matrix4_to_euler(value, rotation_order, angles);
        INFO("order " << order << ": " << angles[0] << ", " << angles[1] << ", " << angles[2]);
        REQUIRE(near(static_cast<float>(angles[0]), 0.3f));
        REQUIRE(near(static_cast<float>(angles[1]), -0.7f));
        REQUIRE(near(static_cast<float>(angles[2]), 1.2f));
    }
}

TEST_CASE("Matrix4 Euler angles at gimbal lock still rebuild the rotation", "[matrix4]") {
    const Matrix4 value = engine_core::matrix4_from_euler(0.5, 3.14159265358979323846 / 2, 0.25, RotationOrder::XYZ);
    double angles[3];
    engine_core::matrix4_to_euler(value, RotationOrder::XYZ, angles);
    REQUIRE(near(engine_core::matrix4_from_euler(angles[0], angles[1], angles[2], RotationOrder::XYZ), value));
}

TEST_CASE("Matrix4 quaternions round-trip and ignore scale", "[matrix4]") {
    const Matrix4 value = posed();
    double quaternion[4];
    engine_core::matrix4_to_quaternion(value, quaternion);
    REQUIRE(quaternion[3] >= 0);
    const Matrix4 rebuilt = engine_core::matrix4_from_quaternion(quaternion[0], quaternion[1], quaternion[2], quaternion[3]);
    Matrix4 expected = engine_core::matrix4_orthonormalize(value);
    expected.m[12] = expected.m[13] = expected.m[14] = 0.f;
    REQUIRE(near(rebuilt, expected));
}

TEST_CASE("Matrix4 look_at points -Z at the target with +Y up", "[matrix4]") {
    const Matrix4 value = engine_core::matrix4_look_at(Vec3{1.f, 1.f, 1.f}, Vec3{1.f, 1.f, -4.f}, Vec3{0.f, 1.f, 0.f});
    REQUIRE(near(value, engine_core::matrix4_translation(1.f, 1.f, 1.f)));
    // Straight up, with up along the look, still gives an orthonormal matrix.
    const Matrix4 up = engine_core::matrix4_look_at(Vec3{}, Vec3{0.f, 5.f, 0.f}, Vec3{0.f, 1.f, 0.f});
    REQUIRE(near(engine_core::matrix4_vector(up, Vec3{0.f, 0.f, -1.f}), Vec3{0.f, 1.f, 0.f}));
    REQUIRE(near(engine_core::matrix4_multiply(up, engine_core::matrix4_inverse(up)), engine_core::matrix4_identity()));
}

TEST_CASE("Matrix4 lerp keeps its ends and blends rotation, position, and scale", "[matrix4]") {
    const Matrix4 from = engine_core::matrix4_identity();
    Matrix4 to = engine_core::matrix4_multiply(engine_core::matrix4_translation(4.f, 0.f, 0.f),
                                               engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 2.0));
    for (int row = 0; row < 3; ++row) {
        to.m[row] *= 3.f;
    }
    REQUIRE(engine_core::same_matrix4(engine_core::matrix4_lerp(from, to, 0.0), from));
    REQUIRE(engine_core::same_matrix4(engine_core::matrix4_lerp(from, to, 1.0), to));
    Matrix4 half = engine_core::matrix4_multiply(engine_core::matrix4_translation(2.f, 0.f, 0.f),
                                                 engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 1.0));
    for (int row = 0; row < 3; ++row) {
        half.m[row] *= 2.f;
    }
    REQUIRE(near(engine_core::matrix4_lerp(from, to, 0.5), half));
}
