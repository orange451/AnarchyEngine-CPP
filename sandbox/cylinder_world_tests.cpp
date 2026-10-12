// The cylinder shape in a Box3D world: what its shape answers, and how it
// rolls, rests, and stacks.

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;
// The engine's step: 240 Hz, one substep (PhysicsWorld::simulate).
constexpr float kStep = 1.f / 240.f;

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

b3Vec3 v3(float x, float y, float z) { return b3Vec3{x, y, z}; }

struct World {
    b3WorldId id;

    World() {
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity = v3(0.f, -10.f, 0.f);
        id = b3CreateWorld(&def);
    }
    ~World() { b3DestroyWorld(id); }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    b3BodyId body(b3BodyType type, b3Vec3 at, b3Quat rotation = b3Quat_identity) {
        b3BodyDef def = b3DefaultBodyDef();
        def.type = type;
        def.position = at;
        def.rotation = rotation;
        return b3CreateBody(id, &def);
    }

    // A static box, half extents half, centred at center.
    b3BodyId box(b3Vec3 center, b3Vec3 half, float friction = 0.6f, b3Quat rotation = b3Quat_identity) {
        const b3BodyId ground = body(b3_staticBody, center, rotation);
        const b3BoxHull hull = b3MakeBoxHull(half.x, half.y, half.z);
        b3ShapeDef def = b3DefaultShapeDef();
        def.baseMaterial.friction = friction;
        b3CreateHullShape(ground, &def, &hull.base);
        return ground;
    }

    // A wide static floor, its top at y = 0.
    b3BodyId floor(float friction = 0.6f) { return box(v3(0.f, -0.5f, 0.f), v3(50.f, 0.5f, 50.f), friction); }

    // A cylinder of radius r and half height h along its body's local Y.
    b3BodyId cylinder(b3BodyType type, b3Vec3 at, b3Quat rotation, float r, float h, float friction = 0.6f) {
        const b3BodyId b = body(type, at, rotation);
        const b3Cylinder c{v3(0.f, -h, 0.f), v3(0.f, h, 0.f), r};
        b3ShapeDef def = b3DefaultShapeDef();
        def.baseMaterial.friction = friction;
        b3CreateCylinderShape(b, &def, &c);
        return b;
    }

    void step(int count = 1) {
        for (int i = 0; i < count; ++i) {
            b3World_Step(id, kStep, 1);
        }
    }
    void seconds(float time) { step(static_cast<int>(time / kStep + 0.5f)); }
};

// Lying on its side: local Y (its axis) turned onto world Z.
b3Quat on_side() { return b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi); }

// Its axis in world space.
b3Vec3 axis_of(b3BodyId body) { return b3RotateVector(b3Body_GetRotation(body), v3(0.f, 1.f, 0.f)); }

b3Vec3 position_of(b3BodyId body) { return b3Body_GetPosition(body); }

}  // namespace

TEST_CASE("CY5 a body with a cylinder shape weighs it, and the shape reads back", "[cylinder]") {
    World world;
    const b3BodyId body = world.body(b3_dynamicBody, v3(0.f, 5.f, 0.f));
    const b3Cylinder c{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.5f};
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = 2.f;
    const b3ShapeId shape = b3CreateCylinderShape(body, &def, &c);
    REQUIRE(b3Shape_IsValid(shape));
    REQUIRE(b3Shape_GetType(shape) == b3_cylinderShape);
    const b3Cylinder back = b3Shape_GetCylinder(shape);
    REQUIRE(back.radius == 0.5f);
    REQUIRE(back.center2.y == 1.f);
    REQUIRE(near(b3Body_GetMass(body), 2.f * kPi * 0.25f * 2.f, 1e-4f));
    // Made longer in place: the body weighs the new length once it applies it.
    const b3Cylinder longer{v3(0.f, -2.f, 0.f), v3(0.f, 2.f, 0.f), 0.5f};
    b3Shape_SetCylinder(shape, &longer);
    b3Body_ApplyMassFromShapes(body);
    REQUIRE(near(b3Body_GetMass(body), 2.f * kPi * 0.25f * 4.f, 1e-4f));
}

TEST_CASE("CY6 a world ray cast meets a cylinder's round side, not its stand-in's flats", "[cylinder]") {
    World world;
    // Lying along Z, radius 0.5, at the origin.
    world.cylinder(b3_staticBody, v3(0.f, 0.f, 0.f), on_side(), 0.5f, 1.f);
    // Straight down at x = 0.1: between two of the stand-in's corners.
    const b3RayResult hit =
        b3World_CastRayClosest(world.id, v3(0.1f, 2.f, 0.3f), v3(0.f, -4.f, 0.f), b3DefaultQueryFilter());
    REQUIRE(hit.hit);
    REQUIRE(near(hit.point.y, std::sqrt(0.25f - 0.01f), 1e-5f));
}

TEST_CASE("CY7 overlaps and shape casts find a cylinder through its stand-in hull", "[cylinder]") {
    World world;
    world.cylinder(b3_staticBody, v3(0.f, 0.f, 0.f), b3Quat_identity, 0.5f, 1.f);
    auto overlaps = [&](b3Vec3 at) {
        const b3Vec3 point = v3(0.f, 0.f, 0.f);
        const b3ShapeProxy proxy{&point, 1, 0.05f};
        int found = 0;
        b3World_OverlapShape(
            world.id, at, &proxy, b3DefaultQueryFilter(),
            [](b3ShapeId, void* context) {
                ++*static_cast<int*>(context);
                return true;
            },
            &found);
        return found;
    };
    REQUIRE(overlaps(v3(0.5f, 0.f, 0.f)) == 1);
    REQUIRE(overlaps(v3(0.6f, 0.f, 0.f)) == 0);

    // A small box swept at it from +X stops within 0.5 % of the radius of the
    // side, short of it by Box3D's linear slop, as every shape cast stops.
    const b3BoxHull small = b3MakeBoxHull(0.1f, 0.1f, 0.1f);
    const b3ShapeProxy proxy{b3GetHullPoints(&small.base), small.base.vertexCount, 0.f};
    float fraction = -1.f;
    b3World_CastShape(
        world.id, v3(3.f, 0.f, 0.f), &proxy, v3(-4.f, 0.f, 0.f), b3DefaultQueryFilter(),
        [](b3ShapeId, b3Pos, b3Vec3, float f, uint64_t, int, int, void* context) {
            *static_cast<float*>(context) = f;
            return f;
        },
        &fraction);
    REQUIRE(fraction > 0.f);
    const float face = 3.f - 0.1f - 4.f * fraction;
    INFO("fraction " << fraction << " face " << face);
    REQUIRE(near(face, 0.5f + 0.005f, 0.0025f));
}

TEST_CASE("CY8 a sensor cylinder notices a body passing through it", "[cylinder]") {
    World world;
    const b3BodyId holder = world.body(b3_staticBody, v3(0.f, 0.f, 0.f));
    const b3Cylinder c{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.5f};
    b3ShapeDef sensor = b3DefaultShapeDef();
    sensor.isSensor = true;
    sensor.enableSensorEvents = true;
    b3CreateCylinderShape(holder, &sensor, &c);
    const b3BodyId ball = world.body(b3_dynamicBody, v3(0.f, 3.f, 0.f));
    b3ShapeDef def = b3DefaultShapeDef();
    def.enableSensorEvents = true;
    const b3Sphere sphere{v3(0.f, 0.f, 0.f), 0.1f};
    b3CreateSphereShape(ball, &def, &sphere);
    bool began = false;
    for (int i = 0; i < 480 && !began; ++i) {
        world.step();
        began = b3World_GetSensorEvents(world.id).beginCount > 0;
    }
    REQUIRE(began);
}

TEST_CASE("CY9 a cylinder shape made, reshaped, and destroyed again and again frees its stand-in hull", "[cylinder]") {
    World world;
    const b3BodyId body = world.body(b3_dynamicBody, v3(0.f, 5.f, 0.f));
    const b3Cylinder c{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.5f};
    const b3Cylinder wider{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.75f};
    b3ShapeDef def = b3DefaultShapeDef();
    // Once first, so the world's own arrays have grown.
    b3DestroyShape(b3CreateCylinderShape(body, &def, &c), true);
    const int64_t before = b3World_GetCounters(world.id).byteCount;
    for (int i = 0; i < 200; ++i) {
        const b3ShapeId shape = b3CreateCylinderShape(body, &def, &c);
        b3Shape_SetCylinder(shape, &wider);
        b3DestroyShape(shape, true);
    }
    REQUIRE(b3World_GetCounters(world.id).byteCount == before);
}

TEST_CASE("CY13 a sphere rests on a cylinder's cap, a capsule along its side", "[cylinder]") {
    World world;
    // Standing, radius 0.5, its top at y = 1.
    world.cylinder(b3_staticBody, v3(0.f, 0.f, 0.f), b3Quat_identity, 0.5f, 1.f);
    const b3BodyId ball = world.body(b3_dynamicBody, v3(0.1f, 1.5f, 0.f));
    b3ShapeDef def = b3DefaultShapeDef();
    const b3Sphere sphere{v3(0.f, 0.f, 0.f), 0.2f};
    b3CreateSphereShape(ball, &def, &sphere);
    // Lying along Z at x = 5, its top at y = 0.5: a capsule laid along it.
    world.cylinder(b3_staticBody, v3(5.f, 0.f, 0.f), on_side(), 0.5f, 2.f);
    const b3BodyId stick = world.body(b3_dynamicBody, v3(5.f, 0.8f, 0.f));
    const b3Capsule capsule{v3(0.f, 0.f, -1.f), v3(0.f, 0.f, 1.f), 0.2f};
    b3CreateCapsuleShape(stick, &def, &capsule);
    world.seconds(2.f);
    REQUIRE(near(position_of(ball).y, 1.2f, 0.01f));
    REQUIRE(near(position_of(ball).x, 0.1f, 0.01f));
    REQUIRE(near(position_of(stick).y, 0.7f, 0.01f));
}
