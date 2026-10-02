// ShadowMath: the shadow maps' cameras, with no GL context. shadow.glsl reads
// the maps back with the same projections.

#include "runner/RenderMath.hpp"
#include "runner/ShadowMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using Catch::Approx;
using engine_core::Matrix4;
using engine_core::Vec3;
using engine_core::matrix4_point;
using namespace runner;

namespace {

// The eight corners of the camera's frustum between depths nearZ and farZ, in world space.
std::vector<Vec3> SliceCorners(const Matrix4& camera, float fov, float aspect, float nearZ, float farZ) {
    const float t = std::tan(fov * 0.5f * kDegree);
    std::vector<Vec3> corners;
    for (const float depth : {nearZ, farZ}) {
        for (const float sx : {-1.f, 1.f}) {
            for (const float sy : {-1.f, 1.f}) {
                corners.push_back(matrix4_point(camera, {sx * depth * t * aspect, sy * depth * t, -depth}));
            }
        }
    }
    return corners;
}

}  // namespace

TEST_CASE("CS1 cascades split the shadow distance from near to far", "[shadow]") {
    float splits[kMaxCascades + 1];
    CascadeSplits(0.1f, 100.f, 4, 0.75f, splits);
    REQUIRE(splits[0] == 0.1f);
    REQUIRE(splits[4] == Approx(100.f));
    for (int i = 0; i < 4; ++i) {
        REQUIRE(splits[i] < splits[i + 1]);
    }
    // Lambda 0 is even.
    CascadeSplits(0.f, 100.f, 4, 0.f, splits);
    REQUIRE(splits[2] == Approx(50.f));
}

TEST_CASE("CS2 a cascade's sphere holds its slice, and turning the camera never changes its size", "[shadow]") {
    const Matrix4 ahead = engine_core::matrix4_look_at({1.f, 2.f, 3.f}, {1.f, 2.f, -10.f}, {0.f, 1.f, 0.f});
    const Matrix4 turned = engine_core::matrix4_look_at({1.f, 2.f, 3.f}, {7.f, -1.f, 5.f}, {0.f, 1.f, 0.f});
    for (const auto& [nearZ, farZ] : {std::pair{0.1f, 6.f}, std::pair{6.f, 25.f}, std::pair{25.f, 100.f}}) {
        const Sphere a = FrustumSliceSphere(ahead, 70.f, 16.f / 9.f, nearZ, farZ);
        const Sphere b = FrustumSliceSphere(turned, 70.f, 16.f / 9.f, nearZ, farZ);
        // Bit for bit: the radius depends only on the angles and depths.
        REQUIRE(a.radius == b.radius);
        for (const Vec3 corner : SliceCorners(turned, 70.f, 16.f / 9.f, nearZ, farZ)) {
            REQUIRE(Length(Sub(corner, b.center)) <= b.radius * 1.0001f);
        }
    }
}

TEST_CASE("CS3 a cascade slides by whole texels as the camera moves and turns", "[shadow]") {
    const Vec3 shine = Normalize({-1.f, -2.f, -0.5f});
    constexpr int kSize = 1024;
    const Vec3 fixedPoint{3.f, 0.f, 5.f};
    float firstU = 0.f;
    float firstV = 0.f;
    float firstTexel = 0.f;
    for (int step = 0; step < 50; ++step) {
        const float yaw = static_cast<float>(step) * 0.37f;
        const Vec3 eye{static_cast<float>(step) * 0.123f, 2.f + static_cast<float>(step) * 0.01f,
                       static_cast<float>(step) * -0.071f};
        const Matrix4 camera = engine_core::matrix4_look_at(
            eye, {eye.x + std::sin(yaw), eye.y - 0.3f, eye.z + std::cos(yaw)}, {0.f, 1.f, 0.f});
        const CascadeFit fit = FitCascade(FrustumSliceSphere(camera, 70.f, 16.f / 9.f, 0.1f, 20.f), shine, kSize);
        const Vec3 p = matrix4_point(fit.viewProjection, fixedPoint);
        const float u = (p.x * 0.5f + 0.5f) * kSize;
        const float v = (p.y * 0.5f + 0.5f) * kSize;
        if (step == 0) {
            firstU = u - std::floor(u);
            firstV = v - std::floor(v);
            firstTexel = fit.texelWorld;
            continue;
        }
        REQUIRE(fit.texelWorld == firstTexel);
        // The same place within its texel, across the wrap from 0.999 to 0.
        float du = (u - std::floor(u)) - firstU;
        float dv = (v - std::floor(v)) - firstV;
        du -= std::round(du);
        dv -= std::round(dv);
        REQUIRE(std::fabs(du) < 2e-3f);
        REQUIRE(std::fabs(dv) < 2e-3f);
    }
}

TEST_CASE("CS4 a fitted cascade holds its whole sphere", "[shadow]") {
    const Sphere sphere{{10.3f, -2.f, 47.9f}, 12.5f};
    const CascadeFit fit = FitCascade(sphere, Normalize({0.3f, -1.f, 0.2f}), 2048);
    for (const Vec3 offset : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1},
                              Vec3{0, 0, -1}, Normalize({1, 1, 1}), Normalize({-1, 1, -1})}) {
        const Vec3 p = matrix4_point(fit.viewProjection, Add(sphere.center, Scale(offset, sphere.radius)));
        REQUIRE(std::fabs(p.x) <= 1.f);
        REQUIRE(std::fabs(p.y) <= 1.f);
        REQUIRE(std::fabs(p.z) <= 1.0001f);
    }
}

TEST_CASE("CS5 a sun shining straight down still fits", "[shadow]") {
    const CascadeFit fit = FitCascade({{0.f, 0.f, 0.f}, 5.f}, {0.f, -1.f, 0.f}, 1024);
    for (const float value : fit.viewProjection.m) {
        REQUIRE(std::isfinite(value));
    }
}

TEST_CASE("CS6 pulling a cascade's near plane reaches casters toward the sun", "[shadow]") {
    const Sphere sphere{{0.f, 0.f, 0.f}, 5.f};
    const CascadeFit plain = FitCascade(sphere, {0.f, -1.f, 0.f}, 1024);
    const CascadeFit pulled = FitCascade(sphere, {0.f, -1.f, 0.f}, 1024, 10.f);
    // The sun is above: the top of the sphere is the near plane, and 10 above it once pulled.
    REQUIRE(matrix4_point(plain.viewProjection, {0.f, 5.f, 0.f}).z == Approx(-1.f));
    REQUIRE(matrix4_point(pulled.viewProjection, {0.f, 15.f, 0.f}).z == Approx(-1.f));
    REQUIRE(pulled.nearDepth == plain.nearDepth);
    // The far plane does not move, and neither do x and y.
    REQUIRE(matrix4_point(pulled.viewProjection, {0.f, -5.f, 0.f}).z == Approx(1.f));
    REQUIRE(matrix4_point(pulled.viewProjection, {2.f, 0.f, 1.f}).x ==
            Approx(matrix4_point(plain.viewProjection, {2.f, 0.f, 1.f}).x));
}

TEST_CASE("SP1 a SpotLight's map looks down its cone, from a part of its Radius out to it", "[shadow]") {
    const SpotShadow shadow = SpotShadowFor({2.f, 5.f, -1.f}, {0.f, -1.f, 0.f}, 60.f, 10.f, 1024);
    for (const float value : shadow.viewProjection.m) {
        REQUIRE(std::isfinite(value));
    }
    const Vec3 axis = matrix4_point(shadow.viewProjection, {2.f, 0.f, -1.f});
    REQUIRE(axis.x == Approx(0.f).margin(1e-5));
    REQUIRE(axis.y == Approx(0.f).margin(1e-5));
    REQUIRE(matrix4_point(shadow.viewProjection, {2.f, -5.f, -1.f}).z == Approx(1.f));
    REQUIRE(matrix4_point(shadow.viewProjection, {2.f, 5.f - ShadowNear(10.f), -1.f}).z == Approx(-1.f));
    // The cone's edge, 30 degrees off the axis, is on the map.
    const Vec3 rim = matrix4_point(shadow.viewProjection, {2.f + std::tan(30.f * kDegree) * 5.f, 0.f, -1.f});
    REQUIRE(std::fabs(rim.x) < 1.f);
    REQUIRE(std::fabs(rim.y) < 1.f);
    REQUIRE(shadow.texelPerDistance == Approx(2.f * std::tan(31.f * kDegree) / 1024.f));
}

TEST_CASE("SP2 the near plane is a part of the Radius, at any scale", "[shadow]") {
    REQUIRE(ShadowNear(10.f) == Approx(0.1f));
    REQUIRE(ShadowNear(1000.f) == Approx(10.f));
    REQUIRE(ShadowNear(0.f) > 0.f);
}

TEST_CASE("CU1 a cube face looks down its axis, and CubeDepth is the depth it draws", "[shadow]") {
    const Vec3 light{1.f, 2.f, 3.f};
    const auto faces = CubeFaceViewProjections(light, 8.f);
    const Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int face = 0; face < 6; ++face) {
        INFO(face);
        const Vec3 along = Scale(axes[face], 3.f);
        const Vec3 p = matrix4_point(faces[face], Add(light, along));
        REQUIRE(p.x == Approx(0.f).margin(1e-5));
        REQUIRE(p.y == Approx(0.f).margin(1e-5));
        REQUIRE(CubeDepth(along, 8.f) == Approx(p.z * 0.5f + 0.5f));
    }
    // Off the axis, the dominant axis's face still matches.
    const Vec3 off{3.f, 1.f, -0.5f};
    REQUIRE(CubeDepth(off, 8.f) == Approx(matrix4_point(faces[0], Add(light, off)).z * 0.5f + 0.5f));
}

TEST_CASE("CU2 CubeFaceUv lands where the widened face matrices draw", "[shadow]") {
    const Vec3 light{-2.f, 1.f, 4.f};
    constexpr float kScale = 0.9f;
    const auto faces = CubeFaceViewProjections(light, 10.f, kScale);
    for (const Vec3 direction : {Vec3{1.f, 0.3f, -0.2f}, Vec3{-1.f, -0.4f, 0.6f}, Vec3{0.2f, 1.f, 0.5f},
                                 Vec3{-0.3f, -1.f, -0.7f}, Vec3{0.6f, 0.1f, 1.f}, Vec3{-0.5f, 0.8f, -1.f}}) {
        INFO(direction.x << " " << direction.y << " " << direction.z);
        const CubeTexel texel = CubeFaceUv(direction, kScale);
        const Vec3 p = matrix4_point(faces[texel.face], Add(light, Scale(direction, 3.f)));
        REQUIRE(texel.u == Approx(p.x * 0.5f + 0.5f).margin(1e-5));
        REQUIRE(texel.v == Approx(p.y * 0.5f + 0.5f).margin(1e-5));
    }
    // A face's 45 degree edge stays the guard's part inside its tile.
    REQUIRE(CubeFaceUv({1.f, 1.f, 0.999f}, kScale).u <= 0.5f + 0.5f * kScale + 1e-4f);
}

TEST_CASE("CV1 a cube face the camera cannot see is skipped", "[shadow]") {
    const Matrix4 camera = engine_core::matrix4_multiply(Perspective(60.f, 1.f, 0.1f, 1000.f),
                                                         LookAtView({0, 0, 0}, {0, 0, -1}, {0, 1, 0}));
    // Just behind the camera: only the face toward -Z reaches anything it sees.
    for (int face = 0; face < 6; ++face) {
        INFO(face);
        REQUIRE(CubeFaceVisible(camera, {0.f, 0.f, 3.f}, 5.f, face) == (face == 5));
    }
    // In plain view, every face is.
    for (int face = 0; face < 6; ++face) {
        REQUIRE(CubeFaceVisible(camera, {0.f, 0.f, -10.f}, 2.f, face));
    }
}

TEST_CASE("PL1 how big a light looks is the same at any scale", "[shadow]") {
    const float small = ProjectedReach({0.f, 0.f, -20.f}, 2.f, {0.f, 0.f, 0.f}, 70.f);
    const float big = ProjectedReach({0.f, 0.f, -200.f}, 20.f, {0.f, 0.f, 0.f}, 70.f);
    REQUIRE(small == Approx(big));
    REQUIRE(ProjectedReach({0.f, 0.f, -40.f}, 2.f, {0.f, 0.f, 0.f}, 70.f) < small);
    REQUIRE(std::isinf(ProjectedReach({0.f, 0.f, -1.f}, 2.f, {0.f, 0.f, 0.f}, 70.f)));
}

TEST_CASE("CL1 casters are culled by their bounds", "[shadow]") {
    Matrix4 model = engine_core::matrix4_translation(5.f, 0.f, 0.f);
    model.m[0] = model.m[5] = model.m[10] = 2.f;
    const float low[3] = {-0.5f, -0.5f, -0.5f};
    const float high[3] = {0.5f, 0.5f, 0.5f};
    const Sphere bounds = WorldBounds(model, low, high);
    REQUIRE(bounds.center.x == Approx(5.f));
    REQUIRE(bounds.radius == Approx(std::sqrt(0.75f) * 2.f));

    const Matrix4 camera = engine_core::matrix4_multiply(Perspective(60.f, 1.f, 0.1f, 50.f),
                                                         LookAtView({0, 0, 0}, {0, 0, -1}, {0, 1, 0}));
    REQUIRE(SphereInFrustum(camera, {{0.f, 0.f, -10.f}, 1.f}));
    REQUIRE_FALSE(SphereInFrustum(camera, {{0.f, 0.f, 10.f}, 1.f}));
    REQUIRE(SphereInFrustum(camera, {{0.f, 0.f, 0.5f}, 1.f}));  // across the near plane
    REQUIRE_FALSE(SphereInFrustum(camera, {{0.f, 0.f, -60.f}, 1.f}));

    // A sun's box: a caster between it and the light counts when the near plane is ignored.
    const Matrix4 box = engine_core::matrix4_multiply(Orthographic(-5, 5, -5, 5, 0, 10),
                                                      LookAtView({0, 0, 0}, {0, 0, -1}, {0, 1, 0}));
    REQUIRE_FALSE(SphereInFrustum(box, {{0.f, 0.f, 20.f}, 1.f}));
    REQUIRE(SphereInFrustum(box, {{0.f, 0.f, 20.f}, 1.f}, true));
    REQUIRE_FALSE(SphereInFrustum(box, {{20.f, 0.f, 20.f}, 1.f}, true));

    REQUIRE(SpheresTouch({{0, 0, 0}, 1.f}, {{2.5f, 0, 0}, 2.f}));
    REQUIRE_FALSE(SpheresTouch({{0, 0, 0}, 1.f}, {{3.5f, 0, 0}, 2.f}));
}
