// ShadowPlanner: which PointLight and SpotLight shadow tiles are drawn each
// frame, with no GL. Every test commits a frame's plan as if it was drawn.

#include "runner/RenderMath.hpp"
#include "runner/ShadowPlanner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

using engine_core::Vec3;
using namespace runner;

namespace {

// A small atlas, so the tests fill it: 256 growing to 512, tiles 32 to 128,
// and no redraw cap unless a test sets one.
ShadowSettings Small() {
    ShadowSettings settings;
    settings.atlasMinSize = 256;
    settings.atlasMaxSize = 512;
    settings.minTile = 32;
    settings.maxTile = 128;
    settings.maxTexelsPerFrame = std::numeric_limits<std::int64_t>::max();
    return settings;
}

CameraView Camera(Vec3 eye, Vec3 target) {
    CameraView camera;
    camera.world = engine_core::matrix4_look_at(eye, target, {0.f, 1.f, 0.f});
    camera.viewProjection = engine_core::matrix4_multiply(Perspective(60.f, 1.f, 0.1f, 1000.f),
                                                          LookAtView(eye, target, {0.f, 1.f, 0.f}));
    camera.fovYDegrees = 60.f;
    camera.aspect = 1.f;
    camera.nearZ = 0.1f;
    camera.paneHeight = 512;
    return camera;
}

ShadowRequest Point(std::uint64_t key, Vec3 at, float radius) {
    ShadowRequest request;
    request.key = key;
    request.owner = key;
    request.kind = ShadowKind::Point;
    request.position = at;
    request.radius = radius;
    return request;
}

ShadowCaster Box(std::uint64_t mesh, Vec3 at, std::uint64_t owner = 0, std::uint64_t revision = 0) {
    ShadowCaster caster;
    caster.mesh = mesh;
    caster.revision = revision;
    caster.owner = owner;
    caster.model = engine_core::matrix4_translation(at.x, at.y, at.z);
    caster.bounds = {at, 0.87f};
    return caster;
}

int Draws(const ShadowPlan& plan, std::uint64_t key) {
    return static_cast<int>(
        std::count_if(plan.draws.begin(), plan.draws.end(), [&](const TileDraw& draw) { return draw.key == key; }));
}

ShadowPlan Frame(ShadowPlanner& planner, const std::vector<ShadowRequest>& requests,
                 const std::vector<ShadowCaster>& casters, const CameraView& camera, const ShadowSettings& settings) {
    ShadowPlan plan = planner.plan(requests, casters, camera, settings);
    planner.commit();
    return plan;
}

}  // namespace

TEST_CASE("TS1 a light's tile is about as many texels as the pixels it covers, and holds at a boundary",
          "[shadow]") {
    const ShadowSettings s;  // tiles 64 to 1024, 1 texel per pixel, minReach 0.02, 15%
    REQUIRE(TileSizeFor(300.f / 512.f, 0, 512, s) == 512);
    REQUIRE(TileSizeFor(20.f / 512.f, 0, 512, s) == 64);
    REQUIRE(TileSizeFor(std::numeric_limits<float>::infinity(), 0, 512, s) == 1024);
    REQUIRE(TileSizeFor(0.019f, 0, 512, s) == 0);  // too small for a shadow
    // 560 is past 512 but within 15%, so it holds; 600 is not.
    REQUIRE(TileSizeFor(560.f / 512.f, 512, 512, s) == 512);
    REQUIRE(TileSizeFor(600.f / 512.f, 512, 512, s) == 1024);
    REQUIRE(TileSizeFor(480.f / 512.f, 1024, 512, s) == 1024);
    REQUIRE(TileSizeFor(400.f / 512.f, 1024, 512, s) == 512);
    // A light with a shadow keeps it a little below minReach.
    REQUIRE(TileSizeFor(0.019f, 64, 512, s) == 64);
    REQUIRE(TileSizeFor(0.015f, 64, 512, s) == 0);
}

TEST_CASE("PN1 a light's map is drawn once and reused while nothing changes", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 1.f, 0.f}, 5.f)};
    const std::vector<ShadowCaster> casters = {Box(100, {1.f, 0.f, 0.f})};
    const ShadowPlan first = Frame(planner, lights, casters, camera, Small());
    REQUIRE(Draws(first, 1) > 0);
    REQUIRE(first.atlasResized);
    REQUIRE(planner.find(1) != nullptr);
    REQUIRE(planner.find(1)->tiles[0].size > 0);
    const ShadowPlan second = Frame(planner, lights, casters, camera, Small());
    REQUIRE(second.draws.empty());
    REQUIRE_FALSE(second.atlasResized);
    REQUIRE(planner.find(1) != nullptr);
}

TEST_CASE("PN2 a caster in reach that moves or changes redraws; one out of reach, or the light's own, does not",
          "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 1.f, 0.f}, 5.f)};
    Frame(planner, lights, {Box(100, {1.f, 0.f, 0.f})}, camera, Small());
    REQUIRE(Draws(Frame(planner, lights, {Box(100, {2.f, 0.f, 0.f})}, camera, Small()), 1) > 0);
    // Uploaded again in place: the same mesh at a new revision.
    REQUIRE(Draws(Frame(planner, lights, {Box(100, {2.f, 0.f, 0.f}, 0, 1)}, camera, Small()), 1) > 0);
    // Far outside its Radius.
    REQUIRE(Frame(planner, lights, {Box(100, {2.f, 0.f, 0.f}, 0, 1), Box(101, {50.f, 0.f, 0.f})}, camera, Small())
                .draws.empty());
    // The light's own Prefab, right on it, moving.
    REQUIRE(Frame(planner, lights,
                  {Box(100, {2.f, 0.f, 0.f}, 0, 1), Box(101, {50.f, 0.f, 0.f}), Box(102, {0.f, 1.f, 0.f}, 1)},
                  camera, Small())
                .draws.empty());
    const ShadowPlan own = Frame(
        planner, lights, {Box(100, {2.f, 0.f, 0.f}, 0, 1), Box(101, {50.f, 0.f, 0.f}), Box(102, {0.f, 1.2f, 0.f}, 1)},
        camera, Small());
    REQUIRE(own.draws.empty());
}

TEST_CASE("PN3 a moved light redraws, and a plan that is never drawn changes nothing", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowCaster> casters = {Box(100, {1.f, 0.f, 0.f})};
    Frame(planner, {Point(1, {0.f, 1.f, 0.f}, 5.f)}, casters, camera, Small());
    REQUIRE(Draws(Frame(planner, {Point(1, {0.f, 1.5f, 0.f}, 5.f)}, casters, camera, Small()), 1) > 0);
    REQUIRE(planner.find(1)->position.y == 1.5f);
    // Planned, but the frame could not draw: the old map stays, and the light stays due.
    const std::vector<ShadowRequest> movedAgain = {Point(1, {0.f, 2.f, 0.f}, 5.f)};
    REQUIRE_FALSE(planner.plan(movedAgain, casters, camera, Small()).draws.empty());
    REQUIRE(planner.find(1)->position.y == 1.5f);
    REQUIRE_FALSE(Frame(planner, movedAgain, casters, camera, Small()).draws.empty());
    REQUIRE(planner.find(1)->position.y == 2.f);
}

TEST_CASE("PN4 a light that is not cached is drawn every frame", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    ShadowRequest light = Point(1, {0.f, 1.f, 0.f}, 5.f);
    light.cached = false;
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(Draws(Frame(planner, {light}, {}, camera, Small()), 1) > 0);
    }
}

TEST_CASE("PN5 under the redraw cap, every light still gets its shadow, one more each frame", "[shadow]") {
    ShadowPlanner planner;
    ShadowSettings settings = Small();
    settings.maxTexelsPerFrame = 1;  // one light a frame, the least it ever draws
    // The camera is inside all three, so each looks infinitely big.
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 0.f, 0.f}, 50.f), Point(2, {1.f, 0.f, 0.f}, 50.f),
                                               Point(3, {2.f, 0.f, 0.f}, 50.f)};
    for (int frame = 1; frame <= 3; ++frame) {
        Frame(planner, lights, {}, camera, settings);
        int ready = 0;
        for (std::uint64_t key = 1; key <= 3; ++key) {
            ready += planner.find(key) != nullptr ? 1 : 0;
        }
        REQUIRE(ready == frame);
    }
}

TEST_CASE("PN6 a cube face the camera cannot see waits until it can", "[shadow]") {
    ShadowPlanner planner;
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 0.f, 3.f}, 5.f)};
    // The light is just behind the camera: only its -Z face reaches what the camera sees.
    const ShadowPlan ahead = Frame(planner, lights, {}, Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}), Small());
    REQUIRE(ahead.draws.size() == 1);
    REQUIRE(ahead.draws[0].face == 5);
    // Turned around, the faces it skipped are drawn, and the -Z face, already drawn, is not.
    const ShadowPlan turned = Frame(planner, lights, {}, Camera({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}), Small());
    REQUIRE_FALSE(turned.draws.empty());
    for (const TileDraw& draw : turned.draws) {
        REQUIRE(draw.face != 5);
    }
}

TEST_CASE("PN7 a light's tile follows how big it looks, and it has none when tiny", "[shadow]") {
    ShadowPlanner planner;
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 0.f, 0.f}, 1.f)};
    Frame(planner, lights, {}, Camera({0.f, 0.f, 100.f}, {0.f, 0.f, 0.f}), Small());
    REQUIRE(planner.find(1) == nullptr);
    Frame(planner, lights, {}, Camera({0.f, 0.f, 20.f}, {0.f, 0.f, 0.f}), Small());
    REQUIRE(planner.find(1)->tiles[0].size == 64);
    Frame(planner, lights, {}, Camera({0.f, 0.f, 5.f}, {0.f, 0.f, 0.f}), Small());
    REQUIRE(planner.find(1)->tiles[0].size == 128);
}

TEST_CASE("PN8 the atlas grows, then the lights that look smallest get smaller tiles", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    std::vector<ShadowRequest> lights;
    for (std::uint64_t key = 1; key <= 5; ++key) {
        lights.push_back(Point(key, {static_cast<float>(key), 0.f, 0.f}, 50.f));
    }
    const ShadowPlan plan = Frame(planner, lights, {}, camera, Small());
    REQUIRE(plan.atlasResized);
    REQUIRE(planner.atlasSize() == 512);
    // All tie at infinitely big, so the order they came in is the priority.
    const int expected[5] = {128, 128, 64, 64, 32};
    for (std::uint64_t key = 1; key <= 5; ++key) {
        INFO(key);
        REQUIRE(planner.find(key) != nullptr);
        REQUIRE(planner.find(key)->tiles[0].size == expected[key - 1]);
    }
}

TEST_CASE("PN9 a light that is gone gives its tiles back", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    Frame(planner, {Point(1, {0.f, 0.f, 0.f}, 50.f), Point(2, {1.f, 0.f, 0.f}, 50.f)}, {}, camera, Small());
    REQUIRE(planner.atlasFreeTexels() < 512 * 512);
    Frame(planner, {}, {}, camera, Small());
    REQUIRE(planner.atlasFreeTexels() == 512 * 512);
    REQUIRE(planner.find(1) == nullptr);
}
