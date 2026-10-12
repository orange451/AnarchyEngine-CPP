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

namespace {

struct Cloud {
    const b3Vec3* points;
    int count;
};

b3Vec3 cloud_support(const void* data, b3Vec3 direction) {
    const Cloud* cloud = static_cast<const Cloud*>(data);
    int best = 0;
    for (int i = 1; i < cloud->count; ++i) {
        if (b3Dot(cloud->points[i], direction) > b3Dot(cloud->points[best], direction)) {
            best = i;
        }
    }
    return cloud->points[best];
}

}  // namespace

TEST_CASE("CY10 GJK on support functions finds what Box3D's point GJK finds", "[cylinder]") {
    const b3BoxHull a = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
    const b3BoxHull b = b3MakeBoxHull(0.3f, 0.6f, 0.2f);
    const Cloud cloudA{b3GetHullPoints(&a.base), a.base.vertexCount};
    const Cloud cloudB{b3GetHullPoints(&b.base), b.base.vertexCount};
    for (const b3Vec3 offset : {v3(2.f, 0.4f, -0.3f), v3(0.f, 1.5f, 0.2f), v3(0.1f, 0.2f, 0.f)}) {
        const b3Transform xf{offset, b3MakeQuatFromAxisAngle(b3Normalize(v3(1.f, 2.f, 3.f)), 0.7f)};
        b3DistanceInput input{};
        input.proxyA = b3ShapeProxy{cloudA.points, cloudA.count, 0.f};
        input.proxyB = b3ShapeProxy{cloudB.points, cloudB.count, 0.f};
        input.transform = xf;
        input.useRadii = false;
        b3SimplexCache cache{};
        const b3DistanceOutput expected = b3ShapeDistance(&input, &cache, nullptr, 0);
        const b3DistanceOutput got = b3SupportDistance(cloud_support, &cloudA, cloud_support, &cloudB, xf);
        INFO(offset.x << " " << offset.y << " " << offset.z);
        REQUIRE(near(got.distance, expected.distance, 1e-5f));
        if (expected.distance > 0.f) {
            REQUIRE(near(got.normal.x, expected.normal.x, 1e-4f));
            REQUIRE(near(got.normal.y, expected.normal.y, 1e-4f));
            REQUIRE(near(got.normal.z, expected.normal.z, 1e-4f));
        }
    }
}

TEST_CASE("CY11 a sphere touches a cylinder's side, cap, and rim at the true surface, at any depth", "[cylinder]") {
    const b3Cylinder c = upright(0.5f, 1.f);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    auto collide = [&](b3Vec3 at, float radius) {
        const b3Sphere sphere{v3(0.f, 0.f, 0.f), radius};
        b3CollideCylinderAndSphere(&m, 4, &c, &sphere, b3Transform{at, b3Quat_identity});
    };
    // Beside the side, 0.01 away.
    collide(v3(0.71f, 0.3f, 0.f), 0.2f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.x, 1.f, 1e-6f));
    REQUIRE(near(m.points[0].separation, 0.01f, 1e-5f));
    // Above the top cap, 0.05 into it.
    collide(v3(0.1f, 1.15f, 0.2f), 0.2f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.y, 1.f, 1e-6f));
    REQUIRE(near(m.points[0].separation, -0.05f, 1e-5f));
    // Off the rim, diagonally: the rim is nearest.
    collide(v3(0.6f, 1.1f, 0.f), 0.13f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.x, std::sqrt(0.5f), 1e-5f));
    REQUIRE(near(m.normal.y, std::sqrt(0.5f), 1e-5f));
    REQUIRE(near(m.points[0].separation, std::sqrt(0.02f) - 0.13f, 1e-5f));
    // Deep inside, nearer the side than a cap: out through the side.
    collide(v3(0.3f, 0.f, 0.f), 0.1f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.x, 1.f, 1e-6f));
    REQUIRE(near(m.points[0].separation, -0.3f, 1e-5f));
    // Far off: nothing.
    collide(v3(3.f, 0.f, 0.f), 0.1f);
    REQUIRE(m.pointCount == 0);
}

TEST_CASE("CY12 a capsule along a cylinder's side or across its cap touches at two points, else one", "[cylinder]") {
    const b3Cylinder c = upright(0.5f, 1.f);
    b3HullData* standIn = b3MakeCylinderHull(&c);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SimplexCache cache{};
    // Along the side: parallel to the axis, 0.01 off it.
    const b3Capsule along{v3(0.f, -0.5f, 0.f), v3(0.f, 0.5f, 0.f), 0.1f};
    b3CollideCylinderAndCapsule(&m, 4, &c, standIn, &along, b3Transform{v3(0.61f, 0.f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 2);
    REQUIRE(near(m.normal.x, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, 0.01f, 1e-5f));
    REQUIRE(near(m.points[1].separation, 0.01f, 1e-5f));
    // Lying across the top cap, 0.01 into it.
    const b3Capsule across{v3(-0.3f, 0.f, 0.f), v3(0.3f, 0.f, 0.f), 0.1f};
    b3CollideCylinderAndCapsule(&m, 4, &c, standIn, &across, b3Transform{v3(0.f, 1.09f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 2);
    REQUIRE(near(m.normal.y, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, -0.01f, 1e-5f));
    REQUIRE(near(m.points[1].separation, -0.01f, 1e-5f));
    // Slanting, its near end about 0.016 off the side: one point, at that end.
    // (Box3D's quaternion from an angle uses its own approximate sine, so the
    // end is where the transform puts it, not where exact trig would.)
    const b3Transform slanted{v3(0.9f, 0.f, 0.f), b3MakeQuatFromAxisAngle(v3(0.f, 0.f, 1.f), 0.6f)};
    b3CollideCylinderAndCapsule(&m, 4, &c, standIn, &along, slanted, &cache);
    REQUIRE(m.pointCount == 1);
    const b3Vec3 nearEnd = b3TransformPoint(slanted, along.center2);
    REQUIRE(near(m.points[0].separation, std::sqrt(nearEnd.x * nearEnd.x + nearEnd.z * nearEnd.z) - 0.6f, 1e-4f));
    b3DestroyHull(standIn);
}

TEST_CASE("CY14 a cylinder on its side on a box: two points along its tread, the same two however it has turned",
          "[cylinder]") {
    const b3BoxHull box = b3MakeBoxHull(5.f, 0.5f, 5.f);  // top at y = 0.5
    const b3Cylinder wheel = upright(0.5f, 0.25f);
    b3HullData* standIn = b3MakeCylinderHull(&wheel);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    const b3Quat lie = b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi);
    for (const float spin : {0.f, 0.37f, 1.3f, 2.9f}) {
        INFO(spin);
        // Lying along Z, 0.005 into the box, turned spin about its own axis.
        const b3Quat turned = b3MulQuat(lie, b3MakeQuatFromAxisAngle(v3(0.f, 1.f, 0.f), spin));
        b3CollideHullAndCylinder(&m, 4, &box.base, &wheel, standIn, b3Transform{v3(0.f, 0.995f, 0.f), turned}, &cache);
        REQUIRE(m.pointCount == 2);
        REQUIRE(near(m.normal.y, 1.f, 1e-6f));
        REQUIRE(near(m.points[0].separation, -0.005f, 1e-5f));
        REQUIRE(near(m.points[1].separation, -0.005f, 1e-5f));
        // The ends of its tread, by name, whatever the spin.
        const int first = std::min(m.points[0].pair.index1, m.points[1].pair.index1);
        const int second = std::max(m.points[0].pair.index1, m.points[1].pair.index1);
        REQUIRE(first == 0);
        REQUIRE(second == 1);
    }
    b3DestroyHull(standIn);
}

TEST_CASE("CY15 a cylinder on its cap on a box: four points, normal up", "[cylinder]") {
    const b3BoxHull box = b3MakeBoxHull(5.f, 0.5f, 5.f);
    const b3Cylinder drum = upright(0.5f, 1.f);
    b3HullData* standIn = b3MakeCylinderHull(&drum);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    b3CollideHullAndCylinder(&m, 4, &box.base, &drum, standIn, b3Transform{v3(0.f, 1.495f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 4);
    REQUIRE(near(m.normal.y, 1.f, 1e-6f));
    for (int i = 0; i < 4; ++i) {
        REQUIRE(near(m.points[i].separation, -0.005f, 1e-5f));
    }
    b3DestroyHull(standIn);
}

TEST_CASE("CY16 a leaning wheel's tread rises from its low end, with no change of radius", "[cylinder]") {
    const b3BoxHull box = b3MakeBoxHull(5.f, 0.5f, 5.f);
    const b3Cylinder wheel = upright(0.5f, 0.25f);
    b3HullData* standIn = b3MakeCylinderHull(&wheel);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    // Lying along Z, then leaned 2 degrees about X; its low rim 0.005 into the box.
    // The lean as Box3D's quaternion makes it: its approximate sine, not exact trig.
    const b3Quat q = b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi + 2.f * kPi / 180.f);
    const float sinLean = std::fabs(b3RotateVector(q, v3(0.f, 1.f, 0.f)).y);
    const float cosLean = std::sqrt(1.f - sinLean * sinLean);
    const float y = 0.5f + 0.5f * cosLean + 0.25f * sinLean - 0.005f;
    b3CollideHullAndCylinder(&m, 4, &box.base, &wheel, standIn, b3Transform{v3(0.f, y, 0.f), q}, &cache);
    REQUIRE(m.pointCount == 2);
    const float low = std::min(m.points[0].separation, m.points[1].separation);
    const float high = std::max(m.points[0].separation, m.points[1].separation);
    REQUIRE(near(low, -0.005f, 1e-5f));
    REQUIRE(near(high, -0.005f + 0.5f * sinLean, 1e-5f));
    b3DestroyHull(standIn);
}

TEST_CASE("CY23 a cylinder on its side on one triangle: two points, the triangle's face", "[cylinder]") {
    // The triangle, counter-clockwise from above, in the world.
    const b3Vec3 world[3] = {v3(-5.f, 0.f, 5.f), v3(5.f, 0.f, 5.f), v3(0.f, 0.f, -5.f)};
    // The cylinder lies along Z, 0.005 into the triangle; Box3D gives the triangle in its frame.
    const b3Cylinder wheel = upright(0.5f, 0.25f);
    b3HullData* standIn = b3MakeCylinderHull(&wheel);
    const b3Transform xf{v3(0.f, 0.495f, 0.f), b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi)};
    b3Vec3 local[3];
    for (int i = 0; i < 3; ++i) {
        local[i] = b3InvTransformPoint(xf, world[i]);
    }
    b3LocalManifoldPoint points[8];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    b3CollideTriangleAndCylinder(&m, 8, local, 0, &wheel, standIn, &cache, true);
    REQUIRE(m.pointCount == 2);
    REQUIRE(m.feature == b3_featureTriangleFace);
    const b3Vec3 up = b3RotateVector(xf.q, m.normal);
    REQUIRE(near(up.y, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, -0.005f, 1e-5f));
    REQUIRE(near(m.points[1].separation, -0.005f, 1e-5f));
    b3DestroyHull(standIn);
}
