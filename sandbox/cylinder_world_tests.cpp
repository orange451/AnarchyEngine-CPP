// The cylinder shape in a Box3D world: what its shape answers, and how it
// rolls, rests, and stacks.

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

namespace {

// Lying along Z on the floor at x, set rolling toward +X at speed without
// slipping. capsule: Box3D's own capsule of the same radius and length instead,
// the baseline a two-point rolling contact in Box3D gets.
b3BodyId rolling_wheel(World& world, float x, float speed, float radius = 0.5f, float half = 0.25f,
                       bool capsule = false) {
    b3BodyId wheel;
    if (capsule) {
        wheel = world.body(b3_dynamicBody, v3(x, radius, 0.f), on_side());
        const b3Capsule shape{v3(0.f, -half, 0.f), v3(0.f, half, 0.f), radius};
        b3ShapeDef def = b3DefaultShapeDef();
        b3CreateCapsuleShape(wheel, &def, &shape);
    } else {
        wheel = world.cylinder(b3_dynamicBody, v3(x, radius, 0.f), on_side(), radius, half);
    }
    world.seconds(0.25f);
    b3Body_SetLinearVelocity(wheel, v3(speed, 0.f, 0.f));
    b3Body_SetAngularVelocity(wheel, v3(0.f, 0.f, -speed / radius));
    return wheel;
}

}  // namespace

namespace {

struct Roll {
    float rise;   // the centre's height range once rolling
    float drift;  // the farthest it strays along its axis
    float tilt;   // the most its axis leans
    float reached;
};

// Rolled from x = -10 toward x = 10 at 4 units a second, measured from half a
// second after the push: the one-time settle from resting into rolling is not
// rolling.
Roll roll_across(bool capsule) {
    World world;
    world.floor();
    const b3BodyId wheel = rolling_wheel(world, -10.f, 4.f, 0.5f, 0.25f, capsule);
    Roll out{0.f, 0.f, 0.f, 0.f};
    float low = 1e9f;
    float high = -1e9f;
    for (int i = 0; i < 240 * 8 && position_of(wheel).x < 10.f; ++i) {
        world.step();
        const b3Vec3 at = position_of(wheel);
        if (i >= 120) {
            low = std::min(low, at.y);
            high = std::max(high, at.y);
        }
        out.drift = std::max(out.drift, std::fabs(at.z));
        out.tilt = std::max(out.tilt, std::fabs(axis_of(wheel).y));
    }
    out.rise = high - low;
    out.reached = position_of(wheel).x;
    return out;
}

}  // namespace

TEST_CASE("CY17 a cylinder rolls 20 units with its centre level and upright, as straight as Box3D's capsule",
          "[cylinder]") {
    const Roll cylinder = roll_across(false);
    const Roll capsule = roll_across(true);
    INFO("cylinder rise " << cylinder.rise << " drift " << cylinder.drift << " tilt " << cylinder.tilt);
    INFO("capsule drift " << capsule.drift);
    REQUIRE(cylinder.reached >= 10.f);
    REQUIRE(cylinder.rise < 1e-4f);
    REQUIRE(cylinder.tilt < 1.75e-4f);  // sin 0.01 degrees
    // A two-point rolling contact creeps along its axis in Box3D's central
    // friction; Box3D's own capsule does. No more than twice that.
    REQUIRE(cylinder.drift <= 2.f * capsule.drift);
}

TEST_CASE("CY18 a cylinder on its side rolls down a 5 degree slope, and lies still on the flat", "[cylinder]") {
    World world;
    // A slope falling toward +X, its surface through the origin.
    const float slope = 5.f * kPi / 180.f;
    const b3Quat tilt = b3MakeQuatFromAxisAngle(v3(0.f, 0.f, 1.f), -slope);
    world.box(b3RotateVector(tilt, v3(0.f, -0.5f, 0.f)), v3(20.f, 0.5f, 5.f), 1.f, tilt);
    const b3BodyId down = world.cylinder(b3_dynamicBody, v3(0.f, 0.51f, 0.f), on_side(), 0.5f, 0.25f, 1.f);
    world.seconds(2.f);
    REQUIRE(position_of(down).x > 1.f);

    World flat;
    flat.floor();
    const b3BodyId still = flat.cylinder(b3_dynamicBody, v3(0.f, 0.5f, 0.f), on_side(), 0.5f, 0.25f);
    flat.seconds(1.f);
    const b3Vec3 before = position_of(still);
    flat.seconds(10.f);
    REQUIRE(b3Distance(before, position_of(still)) < 1e-4f);
    REQUIRE_FALSE(b3Body_IsAwake(still));
}

TEST_CASE("CY19 a cylinder on its cap does not creep, and sleeps", "[cylinder]") {
    World world;
    world.floor();
    const b3BodyId drum = world.cylinder(b3_dynamicBody, v3(0.f, 1.f, 0.f), b3Quat_identity, 0.5f, 1.f);
    world.seconds(1.f);
    const b3Vec3 before = position_of(drum);
    world.seconds(10.f);
    REQUIRE(b3Distance(before, position_of(drum)) < 1e-4f);
    REQUIRE_FALSE(b3Body_IsAwake(drum));
}

TEST_CASE("CY20 a coin spun on its rim settles on a cap", "[cylinder]") {
    World world;
    world.floor();
    // A coin, radius 0.5 and 0.05 thick, on its rim, leaning 30 degrees, spinning.
    const b3Quat lean = b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi - 30.f * kPi / 180.f);
    const b3BodyId coin = world.cylinder(b3_dynamicBody, v3(0.f, 0.47f, 0.f), lean, 0.5f, 0.025f);
    b3Body_SetAngularVelocity(coin, v3(0.f, 6.f, 0.f));
    world.seconds(10.f);
    REQUIRE(std::fabs(axis_of(coin).y) > 0.99f);
    REQUIRE(position_of(coin).y < 0.05f);
}

TEST_CASE("CY21 the same rolling run twice gives the same poses, bit for bit", "[cylinder]") {
    auto run = [] {
        World world;
        world.floor();
        const b3BodyId wheel = rolling_wheel(world, 0.f, 4.f);
        world.seconds(2.f);
        const b3Vec3 at = position_of(wheel);
        const b3Quat q = b3Body_GetRotation(wheel);
        return std::vector<float>{at.x, at.y, at.z, q.v.x, q.v.y, q.v.z, q.s};
    };
    const std::vector<float> first = run();
    const std::vector<float> second = run();
    REQUIRE(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) == 0);
}

namespace {

// The centre's height range rolling across the seam between two boxes, from
// x = -3 to 3, measured from a sixth of a second after the push.
float seam_rise(bool capsule) {
    World world;
    world.box(v3(-5.f, -0.5f, 0.f), v3(5.f, 0.5f, 5.f));
    world.box(v3(5.f, -0.5f, 0.f), v3(5.f, 0.5f, 5.f));
    const b3BodyId wheel = rolling_wheel(world, -3.f, 3.f, 0.5f, 0.25f, capsule);
    float low = 1e9f;
    float high = -1e9f;
    for (int i = 0; i < 240 * 3 && position_of(wheel).x < 3.f; ++i) {
        world.step();
        if (i >= 40) {
            low = std::min(low, position_of(wheel).y);
            high = std::max(high, position_of(wheel).y);
        }
    }
    REQUIRE(position_of(wheel).x >= 3.f);
    return high - low;
}

}  // namespace

TEST_CASE("CY22 a cylinder rolls across the seam between two boxes with no bigger jump than Box3D's capsule",
          "[cylinder]") {
    // Separate boxes have no internal-edge smoothing in Box3D: every shape
    // feels the edge. Box3D's capsule is the bar.
    const float cylinder = seam_rise(false);
    const float capsule = seam_rise(true);
    INFO("cylinder " << cylinder << " capsule " << capsule);
    REQUIRE(cylinder <= capsule);
    REQUIRE(cylinder < 2e-3f);
}

TEST_CASE("CY22b a long thin pipe lying on a box rests still", "[cylinder]") {
    World world;
    world.floor();
    const b3BodyId pipe = world.cylinder(b3_dynamicBody, v3(0.f, 0.1f, 0.f), on_side(), 0.1f, 5.f);
    world.seconds(2.f);
    const b3Vec3 before = position_of(pipe);
    world.seconds(3.f);
    REQUIRE(b3Distance(before, position_of(pipe)) < 1e-3f);
    REQUIRE(near(position_of(pipe).y, 0.1f, 2e-3f));
}

TEST_CASE("CY22c a box rests on two anchored logs", "[cylinder]") {
    World world;
    // Two static logs along Z at x = -0.5 and +0.5, their tops at y = 0.3.
    world.cylinder(b3_staticBody, v3(-0.5f, 0.f, 0.f), on_side(), 0.3f, 2.f);
    world.cylinder(b3_staticBody, v3(0.5f, 0.f, 0.f), on_side(), 0.3f, 2.f);
    const b3BodyId crate = world.body(b3_dynamicBody, v3(0.f, 0.6f, 0.f));
    const b3BoxHull hull = b3MakeBoxHull(1.f, 0.2f, 1.f);
    b3ShapeDef def = b3DefaultShapeDef();
    b3CreateHullShape(crate, &def, &hull.base);
    world.seconds(3.f);
    REQUIRE(near(position_of(crate).y, 0.5f, 0.01f));
    REQUIRE(near(position_of(crate).x, 0.f, 0.01f));
}

namespace {

// A static mesh floor at y = 0: cells 1 unit across from -half to +half, two
// triangles each, its edges identified so Box3D smooths over the seams.
b3BodyId mesh_floor(World& world, int half, std::vector<b3Vec3>& vertices, std::vector<int32_t>& indices,
                    b3MeshData*& mesh) {
    const int side = 2 * half + 1;
    for (int z = -half; z <= half; ++z) {
        for (int x = -half; x <= half; ++x) {
            vertices.push_back(v3(static_cast<float>(x), 0.f, static_cast<float>(z)));
        }
    }
    for (int z = 0; z < side - 1; ++z) {
        for (int x = 0; x < side - 1; ++x) {
            const int i = z * side + x;
            // Counter-clockwise seen from above.
            indices.insert(indices.end(), {i, i + side, i + 1, i + 1, i + side, i + side + 1});
        }
    }
    b3MeshDef def{};
    def.vertices = vertices.data();
    def.indices = indices.data();
    def.vertexCount = static_cast<int>(vertices.size());
    def.triangleCount = static_cast<int>(indices.size() / 3);
    def.identifyEdges = true;
    mesh = b3CreateMesh(&def, nullptr, 0);
    const b3BodyId ground = world.body(b3_staticBody, v3(0.f, 0.f, 0.f));
    b3ShapeDef shape = b3DefaultShapeDef();
    b3CreateMeshShape(ground, &shape, mesh, v3(1.f, 1.f, 1.f));
    return ground;
}

}  // namespace

TEST_CASE("CY24 a cylinder rolls across a mesh's triangle seams without a jump", "[cylinder]") {
    std::vector<b3Vec3> vertices;
    std::vector<int32_t> indices;
    b3MeshData* mesh = nullptr;
    {
        World world;
        mesh_floor(world, 15, vertices, indices, mesh);
        REQUIRE(mesh != nullptr);
        // Off the grid lines, so it crosses diagonals and edges both.
        const b3BodyId wheel = world.cylinder(b3_dynamicBody, v3(-10.f, 0.5f, 0.3f), on_side(), 0.5f, 0.25f);
        world.seconds(0.25f);
        b3Body_SetLinearVelocity(wheel, v3(4.f, 0.f, 0.f));
        b3Body_SetAngularVelocity(wheel, v3(0.f, 0.f, -8.f));
        float low = 1e9f;
        float high = -1e9f;
        for (int i = 0; i < 240 * 6 && position_of(wheel).x < 10.f; ++i) {
            world.step();
            low = std::min(low, position_of(wheel).y);
            high = std::max(high, position_of(wheel).y);
        }
        INFO(low << " .. " << high);
        REQUIRE(position_of(wheel).x >= 10.f);
        REQUIRE(high - low < 5e-4f);
    }
    b3DestroyMesh(mesh);
}

TEST_CASE("CY25 a cylinder lying on a mesh does not creep, and sleeps", "[cylinder]") {
    std::vector<b3Vec3> vertices;
    std::vector<int32_t> indices;
    b3MeshData* mesh = nullptr;
    {
        World world;
        mesh_floor(world, 5, vertices, indices, mesh);
        const b3BodyId wheel = world.cylinder(b3_dynamicBody, v3(0.3f, 0.5f, 0.2f), on_side(), 0.5f, 0.25f);
        world.seconds(1.f);
        const b3Vec3 before = position_of(wheel);
        world.seconds(10.f);
        REQUIRE(b3Distance(before, position_of(wheel)) < 1e-4f);
        REQUIRE_FALSE(b3Body_IsAwake(wheel));
    }
    b3DestroyMesh(mesh);
}
