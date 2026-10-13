// Particle shape sampling: every shape and EmitFrom lands where it should,
// spreads evenly, and replays from a seed.

#include "particles/ParticleMath.hpp"
#include "particles/ParticleShapes.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace engine_core;
using namespace engine_core::particles;

namespace {

constexpr float kPi = 3.14159265f;
const Vec3 kHalf{2.f, 1.f, 3.f};

bool inside_box(Vec3 p, float eps = 1e-4f) {
    return std::fabs(p.x) <= kHalf.x + eps && std::fabs(p.y) <= kHalf.y + eps && std::fabs(p.z) <= kHalf.z + eps;
}

float ellipsoid(Vec3 p) { return (p.x * p.x) / 4.f + (p.y * p.y) / 1.f + (p.z * p.z) / 9.f; }

}  // namespace

TEST_CASE("PS1 every shape and EmitFrom lands where it should", "[particle-shapes]") {
    Pcg32 rng(7);
    for (int i = 0; i < 2000; ++i) {
        REQUIRE(inside_box(sample_shape(ParticleShape::Box, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position));
        const Vec3 s = sample_shape(ParticleShape::Box, ParticleEmitFrom::Surface, kHalf, 360.f, rng).position;
        const bool on_face = std::fabs(std::fabs(s.x) - kHalf.x) < 1e-4f ||
                             std::fabs(std::fabs(s.y) - kHalf.y) < 1e-4f || std::fabs(std::fabs(s.z) - kHalf.z) < 1e-4f;
        REQUIRE(on_face);
        REQUIRE(inside_box(s));
        const Vec3 e = sample_shape(ParticleShape::Box, ParticleEmitFrom::Edge, kHalf, 360.f, rng).position;
        int on = 0;
        on += std::fabs(std::fabs(e.x) - kHalf.x) < 1e-4f;
        on += std::fabs(std::fabs(e.y) - kHalf.y) < 1e-4f;
        on += std::fabs(std::fabs(e.z) - kHalf.z) < 1e-4f;
        REQUIRE(on >= 2);
        REQUIRE(ellipsoid(sample_shape(ParticleShape::Sphere, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position) <=
                1.0001f);
        REQUIRE(ellipsoid(sample_shape(ParticleShape::Sphere, ParticleEmitFrom::Surface, kHalf, 360.f, rng).position) ==
                Catch::Approx(1.f).margin(1e-3));
        REQUIRE(sample_shape(ParticleShape::Hemisphere, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position.y >=
                -1e-4f);
        const Vec3 d = sample_shape(ParticleShape::Disc, ParticleEmitFrom::Edge, kHalf, 360.f, rng).position;
        REQUIRE(d.y == 0.f);
        REQUIRE((d.x * d.x) / 4.f + (d.z * d.z) / 9.f == Catch::Approx(1.f).margin(1e-3));
        const Vec3 cyl = sample_shape(ParticleShape::Cylinder, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position;
        REQUIRE((cyl.x * cyl.x) / 4.f + (cyl.z * cyl.z) / 9.f <= 1.0001f);
        REQUIRE(std::fabs(cyl.y) <= kHalf.y + 1e-4f);
        const Vec3 c = sample_shape(ParticleShape::Cone, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position;
        REQUIRE(std::fabs(c.y) <= kHalf.y + 1e-4f);
        // The cone widens toward +Y: its radius at height y is at most the base's share.
        const float share = (c.y + kHalf.y) / (2.f * kHalf.y);
        REQUIRE((c.x * c.x) / 4.f + (c.z * c.z) / 9.f <= share * share + 1e-3f);
        const ShapeSample point = sample_shape(ParticleShape::Point, ParticleEmitFrom::Surface, kHalf, 360.f, rng);
        REQUIRE(point.position.x == 0.f);
        REQUIRE(point.position.y == 0.f);
        REQUIRE(point.position.z == 0.f);
        // A zero box is a point, whatever the shape.
        const ShapeSample none = sample_shape(ParticleShape::Box, ParticleEmitFrom::Volume, Vec3{}, 360.f, rng);
        REQUIRE(none.position.x == 0.f);
    }
}

TEST_CASE("PS2 Arc limits the angle about Y", "[particle-shapes]") {
    Pcg32 rng(3);
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p = sample_shape(ParticleShape::Disc, ParticleEmitFrom::Volume, kHalf, 90.f, rng).position;
        const float angle = std::atan2(p.z, p.x);
        REQUIRE(angle >= -1e-4f);
        REQUIRE(angle <= kPi / 2.f + 1e-4f);
    }
}

TEST_CASE("PS3 a sphere's volume is even, not bunched at the centre", "[particle-shapes]") {
    Pcg32 rng(11);
    int inner = 0;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const Vec3 p =
            sample_shape(ParticleShape::Sphere, ParticleEmitFrom::Volume, Vec3{1.f, 1.f, 1.f}, 360.f, rng).position;
        inner += length(p) < 0.5f;
    }
    // A ball of half the radius holds 1/8 of the volume.
    REQUIRE(static_cast<float>(inner) / n == Catch::Approx(0.125f).margin(0.01f));
}

TEST_CASE("PS4 spread stays within its cone and zero spread is the direction", "[particle-shapes]") {
    Pcg32 rng(5);
    const Vec3 up{0.f, 1.f, 0.f};
    for (int i = 0; i < 1000; ++i) {
        const Vec3 d = spread_direction(up, 30.f, 30.f, rng);
        REQUIRE(length(d) == Catch::Approx(1.f).margin(1e-4));
        REQUIRE(dot(d, up) >= std::cos(45.f * kPi / 180.f) - 1e-3f);   // two 30-degree tilts stay inside 45
        const Vec3 flat = spread_direction(up, 90.f, 0.f, rng);
        REQUIRE(std::fabs(flat.z) < 1e-4f);   // a fan in the X/Y plane
    }
    const Vec3 same = spread_direction(up, 0.f, 0.f, rng);
    REQUIRE(same.y == Catch::Approx(1.f));
    const Vec3 sideways = spread_direction(Vec3{1.f, 0.f, 0.f}, 0.f, 0.f, rng);
    REQUIRE(sideways.x == Catch::Approx(1.f));
}

TEST_CASE("PS5 the same seed gives the same samples", "[particle-shapes]") {
    Pcg32 a(42);
    Pcg32 b(42);
    for (int i = 0; i < 100; ++i) {
        const Vec3 pa = sample_shape(ParticleShape::Cone, ParticleEmitFrom::Surface, kHalf, 360.f, a).position;
        const Vec3 pb = sample_shape(ParticleShape::Cone, ParticleEmitFrom::Surface, kHalf, 360.f, b).position;
        REQUIRE(pa.x == pb.x);
        REQUIRE(pa.y == pb.y);
        REQUIRE(pa.z == pb.z);
    }
    Pcg32 r(1);
    for (int i = 0; i < 1000; ++i) {
        const float u = r.uniform();
        REQUIRE(u >= 0.f);
        REQUIRE(u < 1.f);
    }
}

TEST_CASE("PS6 rotate turns a vector about an axis", "[particle-shapes]") {
    const Vec3 r = rotate(Vec3{1.f, 0.f, 0.f}, Vec3{0.f, 1.f, 0.f}, kPi / 2.f);
    REQUIRE(r.x == Catch::Approx(0.f).margin(1e-5));
    REQUIRE(r.z == Catch::Approx(-1.f).margin(1e-5));
}
