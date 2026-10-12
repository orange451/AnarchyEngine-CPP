// The cylinder shape in Box3D (cmake/box3d/cylinder.c): its geometry and its
// contact manifolds, checked against analytic answers.

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

b3Vec3 v3(float x, float y, float z) { return b3Vec3{x, y, z}; }

// Radius radius, standing along Y from -half to +half.
b3Cylinder upright(float radius, float half) { return b3Cylinder{v3(0.f, -half, 0.f), v3(0.f, half, 0.f), radius}; }

}  // namespace

TEST_CASE("CY1 a cylinder's mass, centre and inertia match the formulas, along any axis", "[cylinder]") {
    const float density = 2.f;
    const b3Cylinder standing = upright(0.5f, 1.f);
    const b3MassData m = b3ComputeCylinderMass(&standing, density);
    const float mass = density * kPi * 0.25f * 2.f;
    const float axial = 0.5f * mass * 0.25f;
    const float across = mass * (3.f * 0.25f + 4.f) / 12.f;
    REQUIRE(near(m.mass, mass, 1e-5f * mass));
    REQUIRE(near(m.center.x, 0.f, 1e-6f));
    REQUIRE(near(m.center.y, 0.f, 1e-6f));
    REQUIRE(near(m.inertia.cy.y, axial, 1e-5f * axial));
    REQUIRE(near(m.inertia.cx.x, across, 1e-5f * across));
    REQUIRE(near(m.inertia.cz.z, across, 1e-5f * across));
    REQUIRE(near(m.inertia.cx.y, 0.f, 1e-6f));

    // Laid along X and moved: the centre follows, the axial inertia moves to X.
    const b3Cylinder lying{v3(-1.f, 3.f, 2.f), v3(1.f, 3.f, 2.f), 0.5f};
    const b3MassData l = b3ComputeCylinderMass(&lying, density);
    REQUIRE(near(l.mass, mass, 1e-5f * mass));
    REQUIRE(near(l.center.y, 3.f, 1e-6f));
    REQUIRE(near(l.center.z, 2.f, 1e-6f));
    REQUIRE(near(l.inertia.cx.x, axial, 1e-5f * axial));
    REQUIRE(near(l.inertia.cy.y, across, 1e-5f * across));

    // Turned 45 degrees in XY: the standing tensor turned, so its trace holds
    // and its XY term is (axial - across) / 2.
    const float s = std::sqrt(0.5f);
    const b3Cylinder tilted{v3(-s, -s, 0.f), v3(s, s, 0.f), 0.5f};
    const b3MassData t = b3ComputeCylinderMass(&tilted, density);
    REQUIRE(near(t.inertia.cx.x + t.inertia.cy.y + t.inertia.cz.z, axial + 2.f * across, 1e-4f));
    REQUIRE(near(t.inertia.cx.y, 0.5f * (axial - across), 1e-4f));
}

TEST_CASE("CY2 a cylinder's AABB is exact, however it is turned", "[cylinder]") {
    const b3Cylinder c{v3(0.f, -0.75f, 0.f), v3(0.f, 0.75f, 0.f), 0.4f};
    for (const float degrees : {0.f, 30.f, 90.f}) {
        INFO(degrees);
        const b3Transform xf{v3(1.f, 2.f, 3.f), b3MakeQuatFromAxisAngle(v3(0.f, 0.f, 1.f), degrees * kPi / 180.f)};
        const b3AABB box = b3ComputeCylinderAABB(&c, xf);
        // The extremes of 3600 rim points on each cap.
        b3Vec3 lo{1e9f, 1e9f, 1e9f};
        b3Vec3 hi{-1e9f, -1e9f, -1e9f};
        for (int cap = 0; cap < 2; ++cap) {
            for (int i = 0; i < 3600; ++i) {
                const float a = 2.f * kPi * static_cast<float>(i) / 3600.f;
                const b3Vec3 local{0.4f * std::cos(a), cap == 0 ? -0.75f : 0.75f, 0.4f * std::sin(a)};
                const b3Vec3 p = b3TransformPoint(xf, local);
                lo = b3Min(lo, p);
                hi = b3Max(hi, p);
            }
        }
        REQUIRE(near(box.lowerBound.x, lo.x, 1e-4f));
        REQUIRE(near(box.lowerBound.y, lo.y, 1e-4f));
        REQUIRE(near(box.lowerBound.z, lo.z, 1e-4f));
        REQUIRE(near(box.upperBound.x, hi.x, 1e-4f));
        REQUIRE(near(box.upperBound.y, hi.y, 1e-4f));
        REQUIRE(near(box.upperBound.z, hi.z, 1e-4f));
    }
}

TEST_CASE("CY3 a ray hits a cylinder's side, cap and rim exactly, and misses past it", "[cylinder]") {
    const b3Cylinder c = upright(0.5f, 1.f);
    auto cast = [&](b3Vec3 origin, b3Vec3 translation) {
        const b3RayCastInput input{origin, translation, 1.f};
        return b3RayCastCylinder(&c, &input);
    };
    // The side, head on from +X.
    const b3CastOutput side = cast(v3(3.f, 0.2f, 0.f), v3(-4.f, 0.f, 0.f));
    REQUIRE(side.hit);
    REQUIRE(near(side.fraction, 2.5f / 4.f, 1e-6f));
    REQUIRE(near(side.point.x, 0.5f, 1e-5f));
    REQUIRE(near(side.normal.x, 1.f, 1e-5f));
    // The top cap, from above.
    const b3CastOutput cap = cast(v3(0.1f, 3.f, 0.2f), v3(0.f, -4.f, 0.f));
    REQUIRE(cap.hit);
    REQUIRE(near(cap.fraction, 0.5f, 1e-6f));
    REQUIRE(near(cap.normal.y, 1.f, 1e-6f));
    // Up the axis from below: the bottom cap.
    const b3CastOutput axis = cast(v3(0.f, -3.f, 0.f), v3(0.f, 4.f, 0.f));
    REQUIRE(axis.hit);
    REQUIRE(near(axis.point.y, -1.f, 1e-5f));
    REQUIRE(near(axis.normal.y, -1.f, 1e-6f));
    // Just inside the rim, straight down: the cap's edge.
    const b3CastOutput rim = cast(v3(0.49f, 2.f, 0.f), v3(0.f, -2.f, 0.f));
    REQUIRE(rim.hit);
    REQUIRE(near(rim.point.y, 1.f, 1e-5f));
    // Just outside the rim, straight down: past the side, never touching.
    REQUIRE_FALSE(cast(v3(0.51f, 2.f, 0.f), v3(0.f, -4.f, 0.f)).hit);
    // Level with the top but just over it.
    REQUIRE_FALSE(cast(v3(3.f, 1.01f, 0.f), v3(-6.f, 0.f, 0.f)).hit);
    // Short of the side.
    REQUIRE_FALSE(cast(v3(3.f, 0.f, 0.f), v3(-2.f, 0.f, 0.f)).hit);
    // Starting inside: a hit at the origin, as a capsule reports one.
    const b3CastOutput inside = cast(v3(0.f, 0.f, 0.f), v3(1.f, 0.f, 0.f));
    REQUIRE(inside.hit);
    REQUIRE(inside.fraction == 0.f);
}

TEST_CASE("CY4 a cylinder's stand-in hull is 32 sides on its true surface, along its axis", "[cylinder]") {
    const b3Cylinder lying{v3(-1.f, 0.f, 0.f), v3(1.f, 0.f, 0.f), 0.5f};
    b3HullData* hull = b3MakeCylinderHull(&lying);
    REQUIRE(hull != nullptr);
    REQUIRE(hull->vertexCount == 2 * B3_CYLINDER_HULL_SIDES);
    REQUIRE(hull->faceCount == B3_CYLINDER_HULL_SIDES + 2);
    const b3Vec3* points = b3GetHullPoints(hull);
    for (int i = 0; i < hull->vertexCount; ++i) {
        REQUIRE(near(std::fabs(points[i].x), 1.f, 1e-5f));
        REQUIRE(near(std::sqrt(points[i].y * points[i].y + points[i].z * points[i].z), 0.5f, 1e-5f));
    }
    b3DestroyHull(hull);
}
