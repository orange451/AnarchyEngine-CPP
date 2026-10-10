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

// A small atlas, so the tests fill it: one page, 256 growing to 512, tiles
// 32 to 128, and no redraw cap unless a test sets one.
ShadowSettings Small() {
    ShadowSettings settings;
    settings.atlasMinSize = 256;
    settings.atlasMaxSize = 512;
    settings.atlasMaxPages = 1;
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

ShadowRequest Spot(std::uint64_t key, Vec3 at, Vec3 direction, float radius) {
    ShadowRequest request;
    request.key = key;
    request.owner = key;
    request.kind = ShadowKind::Spot;
    request.position = at;
    request.direction = direction;
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
    ShadowSettings settings = Small();
    settings.atlasMaxPages = 1;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    std::vector<ShadowRequest> lights;
    for (std::uint64_t key = 1; key <= 5; ++key) {
        lights.push_back(Point(key, {static_cast<float>(key), 0.f, 0.f}, 50.f));
    }
    const ShadowPlan plan = Frame(planner, lights, {}, camera, settings);
    REQUIRE(plan.atlasResized);
    REQUIRE(planner.atlasSize() == 512);
    REQUIRE(planner.atlasPages() == 1);
    // All tie at infinitely big, so the order they came in is the priority.
    // Each wants six 128s: 5 x 98304 = 491520 texels, over the 262144 of one
    // 512 page. Halving a 128 PointLight to 64 frees 73728, one light at a
    // time from the lowest: 5 (417792), 4 (344064), 3 (270336), 2 (196608),
    // which fits. Light 1 keeps its full size.
    const int expected[5] = {128, 64, 64, 64, 64};
    for (std::uint64_t key = 1; key <= 5; ++key) {
        INFO(key);
        REQUIRE(planner.find(key) != nullptr);
        REQUIRE(planner.find(key)->tiles[0].size == expected[key - 1]);
    }
    // Nothing changed: a kept, downsized tile does not churn.
    REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
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

TEST_CASE("a SpotLight is drawn once and cached, with its tile and viewProjection set", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Spot(1, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f}, 5.f)};
    const std::vector<ShadowCaster> casters = {Box(100, {0.f, 1.f, -1.f})};
    const ShadowPlan first = Frame(planner, lights, casters, camera, Small());
    REQUIRE(Draws(first, 1) == 1);
    REQUIRE(first.draws[0].face == 0);
    REQUIRE(planner.find(1) != nullptr);
    REQUIRE(planner.find(1)->tiles[0].size > 0);
    REQUIRE_FALSE(engine_core::same_matrix4(planner.find(1)->viewProjection, engine_core::matrix4_identity()));
    const ShadowPlan second = Frame(planner, lights, casters, camera, Small());
    REQUIRE(second.draws.empty());
}

TEST_CASE("a caster owned by its light is excluded from that light's draws, a non-owned one is not", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 1.f, 0.f}, 5.f)};
    // Caster 100 belongs to light 1, right on it; caster 101 does not, and is also in reach.
    const std::vector<ShadowCaster> casters = {Box(100, {0.f, 1.2f, 0.f}, 1), Box(101, {1.f, 0.f, 0.f})};
    const ShadowPlan plan = Frame(planner, lights, casters, camera, Small());
    REQUIRE_FALSE(plan.draws.empty());
    bool sawOther = false;
    for (const TileDraw& draw : plan.draws) {
        for (int index : draw.casters) {
            REQUIRE(index != 0);  // never caster 100: it is light 1's own
            sawOther = sawOther || index == 1;
        }
    }
    REQUIRE(sawOther);  // caster 101 does show up somewhere
}

TEST_CASE("PN10 a newly seen face that was never drawn is drawn now, or the light reads as having no map",
          "[shadow]") {
    const CameraView ahead = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    const CameraView turned = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    ShadowSettings cramped = Small();
    cramped.maxTexelsPerFrame = 1;

    // A must-draw light (a newly visible face it has never drawn) goes
    // ahead of one that is merely due: A already matches everything the
    // turned camera can see of it (warmed up turned, the same view the
    // critical frame below uses), B does not.
    {
        ShadowPlanner planner;
        ShadowRequest a = Point(1, {5.f, 0.f, 0.f}, 50.f);       // huge reach
        const ShadowRequest b = Point(2, {0.f, 0.f, 3.f}, 5.f);  // PN6's geometry: only -Z visible, ahead
        Frame(planner, {a}, {}, turned, Small());    // A alone: warms up what turned can see of it
        Frame(planner, {a, b}, {}, ahead, Small());  // both kept alive; B gets its one visible face
        REQUIRE(planner.find(1) != nullptr);
        REQUIRE(planner.find(2) != nullptr);

        a.cached = false;  // forced due every frame, but it already matches everything turned can see
        const ShadowPlan plan = Frame(planner, {a, b}, {}, turned, cramped);
        REQUIRE(Draws(plan, 2) > 0);   // B: must-draw (newly visible faces), goes first
        REQUIRE(Draws(plan, 1) == 0);  // A: merely due, loses the one slot
    }

    // A must-draw light can still lose to a higher-priority must-draw light:
    // it then reads as having no map, until a later frame lets it through.
    {
        ShadowPlanner planner;
        const ShadowRequest b = Point(2, {0.f, 0.f, 3.f}, 5.f);
        Frame(planner, {b}, {}, ahead, Small());
        REQUIRE(planner.find(2) != nullptr);

        // Turned, B's remaining faces are must-draw. C, brand new, is ordered first and takes the one slot.
        const ShadowRequest c = Point(3, {5.f, 0.f, 0.f}, 50.f);
        const ShadowPlan blocked = Frame(planner, {c, b}, {}, turned, cramped);
        REQUIRE(Draws(blocked, 3) > 0);
        REQUIRE(Draws(blocked, 2) == 0);
        REQUIRE(planner.find(2) == nullptr);  // null, not B's stale, now-mismatched map

        // Next frame, C already matches, so B gets the slot.
        const ShadowPlan recovered = Frame(planner, {c, b}, {}, turned, cramped);
        REQUIRE(Draws(recovered, 2) > 0);
        REQUIRE(planner.find(2) != nullptr);
    }
}

TEST_CASE("PN11 a light that keeps losing the cap is redrawn within a few frames", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const ShadowRequest warmA = Point(1, {0.f, 0.f, 0.f}, 50.f);
    const ShadowRequest warmB = Point(2, {1.f, 0.f, 0.f}, 50.f);
    Frame(planner, {warmA, warmB}, {}, camera, Small());  // both ready and fully matched
    REQUIRE(planner.find(1) != nullptr);
    REQUIRE(planner.find(2) != nullptr);

    ShadowSettings settings = Small();
    settings.maxTexelsPerFrame = 1;  // one light's worth per frame, at most
    ShadowRequest a = warmA;
    a.cached = false;  // always due: a tied-priority light that never finishes

    bool drawn = false;
    for (int frame = 0; frame < kMaxShadowWait + 1 && !drawn; ++frame) {
        ShadowRequest b = warmB;
        b.position.y = 0.01f * static_cast<float>(frame + 1);  // moved: due, but still matches everywhere
        const ShadowPlan plan = Frame(planner, {a, b}, {}, camera, settings);
        drawn = Draws(plan, 2) > 0;
    }
    REQUIRE(drawn);
}

TEST_CASE("a must-draw light scheduled but not committed still reads as having no map", "[shadow]") {
    ShadowPlanner planner;
    const CameraView ahead = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    const CameraView turned = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 0.f, 3.f}, 5.f)};  // PN6's geometry
    Frame(planner, lights, {}, ahead, Small());
    REQUIRE(planner.find(1) != nullptr);

    // Turned, the light's unmatched faces are visible, so it is must-draw
    // and (an infinite cap) gets scheduled. Without a commit, the draw
    // never actually happened: find() must not hand out a map with a
    // visible face it never drew.
    const ShadowPlan plan = planner.plan(lights, {}, turned, Small());
    REQUIRE_FALSE(Draws(plan, 1) == 0);  // it was in fact scheduled, not skipped by any cap
    REQUIRE(planner.find(1) == nullptr);

    // A normal, committed frame makes it readable again.
    REQUIRE(Draws(Frame(planner, lights, {}, turned, Small()), 1) > 0);
    REQUIRE(planner.find(1) != nullptr);
}

TEST_CASE("CP1 the sun's cascades are reused while the camera and casters hold still", "[shadow]") {
    ShadowPlanner planner;
    SunRequest sun;
    sun.shine = Normalize({0.3f, -1.f, 0.2f});
    sun.shadowDistance = 20.f;
    const std::vector<ShadowCaster> casters = {Box(100, {0.f, 0.f, 0.f})};
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    REQUIRE(planner.planCascades(&sun, casters, camera, Small()).size() == 4);
    REQUIRE(planner.cascades() == nullptr);  // not committed yet
    planner.commitCascades();
    REQUIRE(planner.cascades() != nullptr);
    REQUIRE(planner.cascades()->count == 4);
    REQUIRE(planner.planCascades(&sun, casters, camera, Small()).empty());
    // The camera moves, or a caster does: drawn again.
    REQUIRE_FALSE(planner.planCascades(&sun, casters, Camera({0.f, 2.f, 11.f}, {0.f, 0.f, 0.f}), Small()).empty());
    planner.commitCascades();
    REQUIRE_FALSE(planner.planCascades(&sun, {Box(100, {0.5f, 0.f, 0.f})}, Camera({0.f, 2.f, 11.f}, {0.f, 0.f, 0.f}),
                                       Small())
                      .empty());
}

TEST_CASE("CP2 a cascade reaches toward the sun for casters, up to its limit", "[shadow]") {
    ShadowPlanner planner;
    SunRequest sun;
    sun.shine = {0.f, -1.f, 0.f};
    sun.shadowDistance = 20.f;  // a 4 x 20 = 80 stud reach
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    float splits[kMaxCascades + 1];
    CascadeSplits(camera.nearZ, sun.shadowDistance, 4, 0.75f, splits);
    const Sphere first = FrustumSliceSphere(camera.world, 60.f, 1.f, splits[0], splits[1]);
    const std::vector<ShadowCaster> casters = {
        Box(100, Add(first.center, {0.f, first.radius + 60.f, 0.f})),   // above, within reach
        Box(101, Add(first.center, {0.f, first.radius + 200.f, 0.f})),  // above, past it
    };
    const std::vector<CascadeDraw> draws = planner.planCascades(&sun, casters, camera, Small());
    REQUIRE(draws.size() == 4);
    REQUIRE(std::count(draws[0].casters.begin(), draws[0].casters.end(), 0) == 1);
    REQUIRE(std::count(draws[0].casters.begin(), draws[0].casters.end(), 1) == 0);
}

TEST_CASE("CP3 with no sun, or a ShadowDistance inside the near plane, there are no cascades", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    REQUIRE(planner.planCascades(nullptr, {}, camera, Small()).empty());
    SunRequest sun;
    sun.shadowDistance = 0.05f;
    REQUIRE(planner.planCascades(&sun, {}, camera, Small()).empty());
    planner.commitCascades();
    REQUIRE(planner.cascades() == nullptr);
}

TEST_CASE("CP4 the sun's own meshes cast nothing for it", "[shadow]") {
    ShadowPlanner planner;
    SunRequest sun;
    sun.owner = 7;
    sun.shadowDistance = 20.f;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<CascadeDraw> draws =
        planner.planCascades(&sun, {Box(100, {0.f, 0.f, 0.f}, 7), Box(101, {0.f, 0.f, 0.f}, 8)}, camera, Small());
    REQUIRE_FALSE(draws.empty());
    bool otherDrawn = false;
    for (const CascadeDraw& draw : draws) {
        REQUIRE(std::count(draw.casters.begin(), draw.casters.end(), 0) == 0);
        otherDrawn = otherDrawn || std::count(draw.casters.begin(), draw.casters.end(), 1) == 1;
    }
    REQUIRE(otherDrawn);
}

TEST_CASE("PN12 a light that is not cached reads as having no map on a frame the cap skips it", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    // A light with no instance: its key is only a place in this frame's list.
    const std::uint64_t key = (std::uint64_t{1} << 63) | 0u;
    ShadowRequest first = Point(key, {0.f, 0.f, 0.f}, 50.f);
    first.cached = false;
    first.owner = 0;
    Frame(planner, {first}, {}, camera, Small());
    REQUIRE(planner.find(key) != nullptr);

    // Next frame another light holds that place, somewhere else, and a new
    // light takes the frame's one redraw: last frame's map is not this light's.
    ShadowSettings cramped = Small();
    cramped.maxTexelsPerFrame = 1;
    ShadowRequest second = first;
    second.position = {3.f, 0.f, 0.f};
    const ShadowPlan plan = Frame(planner, {Point(2, {1.f, 0.f, 0.f}, 50.f), second}, {}, camera, cramped);
    REQUIRE(Draws(plan, 2) > 0);
    REQUIRE(Draws(plan, key) == 0);
    REQUIRE(planner.find(key) == nullptr);
}

TEST_CASE("PN13 a PointLight that turns keeps its map: its cube does not turn with it", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowCaster> casters = {Box(100, {1.f, 0.f, 0.f})};
    ShadowRequest light = Point(1, {0.f, 1.f, 0.f}, 5.f);
    REQUIRE(Draws(Frame(planner, {light}, casters, camera, Small()), 1) > 0);
    light.direction = Normalize({1.f, -1.f, 0.f});
    light.outerFovDegrees = 30.f;
    REQUIRE(Frame(planner, {light}, casters, camera, Small()).draws.empty());
    REQUIRE(planner.find(1) != nullptr);
}

TEST_CASE("CP5 cascades dropped before their redraw commits stay unreadable until it does", "[shadow]") {
    ShadowPlanner planner;
    SunRequest sun;
    sun.shadowDistance = 20.f;
    const std::vector<ShadowCaster> casters = {Box(100, {0.f, 0.f, 0.f})};
    REQUIRE_FALSE(planner.planCascades(&sun, casters, Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f}), Small()).empty());
    planner.commitCascades();
    REQUIRE(planner.cascades() != nullptr);
    // The camera moved: planned, then dropped before the draws, which never commit.
    REQUIRE_FALSE(planner.planCascades(&sun, casters, Camera({0.f, 2.f, 11.f}, {0.f, 0.f, 0.f}), Small()).empty());
    planner.forgetCascades();
    REQUIRE(planner.cascades() == nullptr);
    // Dropped, the same view is drawn again, and readable once that commits.
    REQUIRE_FALSE(planner.planCascades(&sun, casters, Camera({0.f, 2.f, 11.f}, {0.f, 0.f, 0.f}), Small()).empty());
    planner.commitCascades();
    REQUIRE(planner.cascades() != nullptr);
}

namespace {

// Every tile key's light has, or none.
std::vector<AtlasTile> TilesOf(const ShadowPlanner& planner, std::uint64_t key) {
    const LocalShadow* shadow = planner.find(key);
    if (shadow == nullptr) {
        return {};
    }
    const int count = shadow->kind == ShadowKind::Spot ? 1 : 6;
    return std::vector<AtlasTile>(shadow->tiles, shadow->tiles + count);
}

}  // namespace

TEST_CASE("PN14 more lights than two full pages hold: every one still gets a tile, the biggest-looking the biggest",
          "[shadow]") {
    ShadowPlanner planner;
    ShadowSettings settings = Small();
    settings.atlasMinSize = 256;
    settings.atlasMaxSize = 256;
    settings.atlasMaxPages = 2;
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    // Two SpotLights, then six PointLights, each farther than the last, so
    // each looks smaller; every one is near enough to want a 128 tile.
    std::vector<ShadowRequest> lights;
    for (std::uint64_t key = 1; key <= 8; ++key) {
        const Vec3 at{0.f, 0.f, -(7.f + static_cast<float>(key))};
        lights.push_back(key <= 2 ? Spot(key, at, {0.f, 0.f, 1.f}, 5.f) : Point(key, at, 5.f));
    }
    REQUIRE(TileSizeFor(ProjectedReach(lights.back().position, 5.f, {0.f, 0.f, 0.f}, 60.f), 0, 512, settings) == 128);
    for (int frame = 0; frame < 3; ++frame) {
        Frame(planner, lights, {}, camera, settings);
    }
    REQUIRE(planner.atlasSize() == 256);
    REQUIRE(planner.atlasPages() == 2);

    // At 128 they want 2 x 16384 + 6 x 98304 = 622592 texels, over the 131072
    // of two 256 pages. One pass from the lowest halves each to 64 (155648,
    // still over); the next halves 8 to 32 (137216), then 7 (118784), which fits.
    const int expected[8] = {64, 64, 64, 64, 64, 64, 32, 32};
    bool onPage[2] = {false, false};
    int previous = 128;
    for (std::uint64_t key = 1; key <= 8; ++key) {
        INFO(key);
        const std::vector<AtlasTile> tiles = TilesOf(planner, key);
        REQUIRE_FALSE(tiles.empty());
        for (const AtlasTile& tile : tiles) {
            REQUIRE(tile.size == expected[key - 1]);
            REQUIRE((tile.page == 0 || tile.page == 1));
            onPage[tile.page] = true;
        }
        // Never bigger than a light that looks bigger.
        REQUIRE(tiles[0].size <= previous);
        previous = tiles[0].size;
    }
    REQUIRE(onPage[0]);
    REQUIRE(onPage[1]);
    REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
}

TEST_CASE("PN15 past what even the smallest tiles hold, only the lights that look smallest go without", "[shadow]") {
    ShadowPlanner planner;
    ShadowSettings settings = Small();
    settings.atlasMinSize = 256;
    settings.atlasMaxSize = 256;
    settings.atlasMaxPages = 1;
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    // Twelve PointLights at six 32s each is 73728 texels; one 256 page holds 65536, so ten.
    std::vector<ShadowRequest> lights;
    for (std::uint64_t key = 1; key <= 12; ++key) {
        lights.push_back(Point(key, {0.f, 0.f, -(7.f + static_cast<float>(key))}, 5.f));
    }
    for (int frame = 0; frame < 2; ++frame) {
        Frame(planner, lights, {}, camera, settings);
    }
    for (std::uint64_t key = 1; key <= 12; ++key) {
        INFO(key);
        const std::vector<AtlasTile> tiles = TilesOf(planner, key);
        if (key <= 10) {
            REQUIRE_FALSE(tiles.empty());
            for (const AtlasTile& tile : tiles) {
                REQUIRE(tile.size >= settings.minTile);
            }
        } else {
            REQUIRE(tiles.empty());
        }
    }
    REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
}

TEST_CASE("PN16 the atlas grows in one step to the smallest page, then fewest pages, that hold what is wanted",
          "[shadow]") {
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    // Lights the camera is inside, each wanting 128s.
    const auto points = [](int count) {
        std::vector<ShadowRequest> lights;
        for (int key = 1; key <= count; ++key) {
            lights.push_back(Point(static_cast<std::uint64_t>(key), {static_cast<float>(key), 0.f, 0.f}, 50.f));
        }
        return lights;
    };

    // Five want 491520 texels, more than one 512 page: from 256, straight
    // to two 512 pages in the first frame.
    {
        ShadowPlanner planner;
        ShadowSettings settings = Small();  // 256 growing to 512
        settings.atlasMaxPages = 2;
        const std::vector<ShadowRequest> lights = points(5);
        REQUIRE(Frame(planner, lights, {}, camera, settings).atlasResized);
        REQUIRE(planner.atlasSize() == 512);
        REQUIRE(planner.atlasPages() == 2);
        REQUIRE(planner.atlasFreeTexels() == 2 * 512 * 512 - 5 * 6 * 128 * 128);
        for (std::uint64_t key = 1; key <= 5; ++key) {
            INFO(key);
            REQUIRE(TilesOf(planner, key).at(0).size == 128);
        }
        // Everyone fits: no more growth, and nothing redrawn.
        const ShadowPlan still = Frame(planner, lights, {}, camera, settings);
        REQUIRE_FALSE(still.atlasResized);
        REQUIRE(still.draws.empty());
    }
    // Two want 196608: one 512 page holds them, so 256 doubles rather than
    // taking three 256 pages.
    {
        ShadowPlanner planner;
        ShadowSettings settings = Small();
        settings.atlasMaxPages = 4;
        Frame(planner, points(2), {}, camera, settings);
        REQUIRE(planner.atlasSize() == 512);
        REQUIRE(planner.atlasPages() == 1);
    }
    // Pages already at their biggest: nine SpotLights' 147456 texels take
    // three 256 pages, added in one frame.
    {
        ShadowPlanner planner;
        ShadowSettings settings = Small();
        settings.atlasMaxSize = 256;
        settings.atlasMaxPages = 4;
        std::vector<ShadowRequest> spots;
        for (std::uint64_t key = 1; key <= 9; ++key) {
            spots.push_back(Spot(key, {static_cast<float>(key), 0.f, 0.f}, {0.f, 0.f, -1.f}, 50.f));
        }
        REQUIRE(Frame(planner, spots, {}, camera, settings).atlasResized);
        REQUIRE(planner.atlasSize() == 256);
        REQUIRE(planner.atlasPages() == 3);
        REQUIRE_FALSE(Frame(planner, spots, {}, camera, settings).atlasResized);
    }
    // A frame wanting no more than one 256 page does not grow it.
    {
        ShadowPlanner planner;
        ShadowSettings settings = Small();
        settings.atlasMaxPages = 2;
        const ShadowRequest one = Spot(1, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}, 50.f);  // one 128
        REQUIRE(Frame(planner, {one}, {}, camera, settings).atlasResized);
        REQUIRE(planner.atlasSize() == 256);
        REQUIRE(planner.atlasPages() == 1);
        REQUIRE_FALSE(Frame(planner, {one}, {}, camera, settings).atlasResized);
    }
}

TEST_CASE("PN17 a light the free blocks are too broken up for gets a smaller tile, and its own once there is room",
          "[shadow]") {
    ShadowPlanner planner;
    ShadowSettings settings = Small();
    settings.atlasMinSize = 256;
    settings.atlasMaxSize = 256;
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    // Sixteen far SpotLights want a 64 each, filling the one 256 page.
    std::vector<ShadowRequest> far;
    for (std::uint64_t key = 1; key <= 16; ++key) {
        far.push_back(Spot(key, {3.f * (static_cast<float>(key) - 8.f), 0.f, -100.f}, {0.f, 0.f, 1.f}, 5.f));
    }
    Frame(planner, far, {}, camera, settings);
    REQUIRE(planner.atlasFreeTexels() == 0);

    // One light gone from each 128 block: four 64s free, but no 128.
    std::vector<ShadowRequest> kept;
    bool blockFreed[4] = {};
    for (const ShadowRequest& light : far) {
        REQUIRE(TilesOf(planner, light.key).at(0).size == 64);
        const AtlasTile tile = TilesOf(planner, light.key)[0];
        const int block = tile.x / 128 + 2 * (tile.y / 128);
        if (!blockFreed[block]) {
            blockFreed[block] = true;
        } else {
            kept.push_back(light);
        }
    }
    REQUIRE(kept.size() == 12);
    // A nearer one wants a 128: 12 x 4096 + 16384 is the whole page, but
    // there is no 128 block left, so it takes a 64 rather than none.
    std::vector<ShadowRequest> lights = kept;
    lights.push_back(Spot(17, {0.f, 0.f, -50.f}, {0.f, 0.f, 1.f}, 5.f));
    Frame(planner, lights, {}, camera, settings);
    REQUIRE(TilesOf(planner, 17).at(0).size == 64);
    for (const ShadowRequest& light : kept) {
        REQUIRE(TilesOf(planner, light.key).at(0).size == 64);
    }
    REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());

    SECTION("the others go: now a 128 is free, and it moves into one at once") {
        Frame(planner, {lights.back()}, {}, camera, settings);
        REQUIRE(TilesOf(planner, 17).at(0).size == 128);
        REQUIRE(Frame(planner, {lights.back()}, {}, camera, settings).draws.empty());
    }
    SECTION("the others stay: once it has been short long enough, the atlas is packed again around it") {
        // Two frames short so far; it waits out the rest.
        for (int frame = 3; frame < kShortFramesBeforeRepack; ++frame) {
            REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
            REQUIRE(TilesOf(planner, 17).at(0).size == 64);
        }
        Frame(planner, lights, {}, camera, settings);
        REQUIRE(TilesOf(planner, 17).at(0).size == 128);
        for (const ShadowRequest& light : kept) {
            REQUIRE(TilesOf(planner, light.key).at(0).size == 64);
        }
        REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
    }
}

namespace {

// One 256 page: two SpotLights hold 128s in two of its blocks, and the
// other two blocks each hold two 64s with two free. Then the nearest light
// of all arrives wanting a 128. What everyone wants is exactly the page, so
// nobody is fitted smaller, but no 128 block is free.
std::vector<ShadowRequest> Fragmented(ShadowPlanner& planner, const CameraView& camera,
                                      const ShadowSettings& settings) {
    std::vector<ShadowRequest> first = {Spot(1, {-5.f, 0.f, -50.f}, {0.f, 0.f, 1.f}, 5.f),
                                        Spot(2, {5.f, 0.f, -50.f}, {0.f, 0.f, 1.f}, 5.f)};
    for (std::uint64_t key = 3; key <= 10; ++key) {
        first.push_back(Spot(key, {3.f * (static_cast<float>(key) - 6.f), 0.f, -100.f}, {0.f, 0.f, 1.f}, 5.f));
    }
    Frame(planner, first, {}, camera, settings);
    REQUIRE(planner.atlasFreeTexels() == 0);
    REQUIRE(TilesOf(planner, 1).at(0).size == 128);
    REQUIRE(TilesOf(planner, 2).at(0).size == 128);

    std::vector<ShadowRequest> lights = {first[0], first[1]};
    int inBlock[4] = {};
    for (std::size_t i = 2; i < first.size(); ++i) {
        const AtlasTile tile = TilesOf(planner, first[i].key).at(0);
        REQUIRE(tile.size == 64);
        const int block = tile.x / 128 + 2 * (tile.y / 128);
        if (++inBlock[block] > 2) {
            lights.push_back(first[i]);
        }
    }
    REQUIRE(lights.size() == 6);
    lights.push_back(Spot(11, {0.f, 0.f, -30.f}, {0.f, 0.f, 1.f}, 5.f));
    return lights;
}

}  // namespace

TEST_CASE("PN18 a light held short by kept tiles gets its size back by a packing again, a few frames later",
          "[shadow]") {
    ShadowPlanner planner;
    ShadowSettings settings = Small();
    settings.atlasMaxSize = 256;
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    const std::vector<ShadowRequest> lights = Fragmented(planner, camera, settings);

    // At first it takes a 64 rather than none, and the farther ones keep their 128s.
    Frame(planner, lights, {}, camera, settings);
    REQUIRE(TilesOf(planner, 11).at(0).size == 64);
    REQUIRE(TilesOf(planner, 1).at(0).size == 128);
    for (int frame = 2; frame < kShortFramesBeforeRepack; ++frame) {
        REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
        REQUIRE(TilesOf(planner, 11).at(0).size == 64);
    }
    // Short that many frames running: everyone is packed again, biggest first.
    REQUIRE_FALSE(Frame(planner, lights, {}, camera, settings).draws.empty());
    REQUIRE(TilesOf(planner, 11).at(0).size == 128);
    REQUIRE(TilesOf(planner, 1).at(0).size == 128);
    REQUIRE(TilesOf(planner, 2).at(0).size == 128);
    for (std::size_t i = 2; i + 1 < lights.size(); ++i) {
        REQUIRE(TilesOf(planner, lights[i].key).at(0).size == 64);
    }
    REQUIRE(Frame(planner, lights, {}, camera, settings).draws.empty());
}

TEST_CASE("PN19 a short light fitted down to the size it already holds keeps its tile", "[shadow]") {
    ShadowPlanner planner;
    ShadowSettings settings = Small();
    settings.atlasMaxSize = 256;
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    const std::vector<ShadowRequest> lights = Fragmented(planner, camera, settings);
    Frame(planner, lights, {}, camera, settings);
    const AtlasTile held = TilesOf(planner, 11).at(0);
    REQUIRE(held.size == 64);

    // Tiles top out at 64 now: what it holds is all it wants.
    settings.maxTile = 64;
    const ShadowPlan plan = Frame(planner, lights, {}, camera, settings);
    REQUIRE(Draws(plan, 11) == 0);
    REQUIRE(Draws(plan, 1) > 0);  // its 128 went
    const AtlasTile now = TilesOf(planner, 11).at(0);
    REQUIRE((now.x == held.x && now.y == held.y && now.page == held.page && now.size == 64));
}

TEST_CASE("PN20 an atlas capped below its settings fits the lights to the cap instead of growing", "[shadow]") {
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    // Two lights the camera is inside, wanting six 128s each: 196608 texels,
    // which would grow 256 to 512.
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 0.f, 0.f}, 50.f), Point(2, {1.f, 0.f, 0.f}, 50.f)};
    ShadowSettings settings = Small();
    settings.atlasMaxPages = 2;

    ShadowPlanner capped;
    capped.capAtlas(256, 1);
    Frame(capped, lights, {}, camera, settings);
    REQUIRE(capped.atlasSize() == 256);
    REQUIRE(capped.atlasPages() == 1);
    // 65536 texels: 2 halved to 64 (122880), then 1 (49152).
    REQUIRE(TilesOf(capped, 1).at(0).size == 64);
    REQUIRE(TilesOf(capped, 2).at(0).size == 64);
    REQUIRE(Frame(capped, lights, {}, camera, settings).draws.empty());

    // Capped after it grew: it goes back down, and everyone still gets a tile.
    ShadowPlanner grown;
    Frame(grown, lights, {}, camera, settings);
    REQUIRE(grown.atlasSize() == 512);
    grown.capAtlas(256, 1);
    REQUIRE(Frame(grown, lights, {}, camera, settings).atlasResized);
    REQUIRE(grown.atlasSize() == 256);
    REQUIRE(TilesOf(grown, 1).at(0).size == 64);
    REQUIRE(TilesOf(grown, 2).at(0).size == 64);
}

TEST_CASE("PN21 a growth step leaves every light without a map until its redraw commits", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const ShadowRequest spot = Spot(1, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}, 50.f);  // one 128
    Frame(planner, {spot}, {}, camera, Small());
    REQUIRE(planner.find(1) != nullptr);
    REQUIRE(planner.atlasSize() == 256);

    // Two PointLights more need a 512 page: every tile moves.
    const std::vector<ShadowRequest> lights = {spot, Point(2, {1.f, 0.f, 0.f}, 50.f), Point(3, {2.f, 0.f, 0.f}, 50.f)};
    const ShadowPlan plan = planner.plan(lights, {}, camera, Small());
    REQUIRE(plan.atlasResized);
    REQUIRE(Draws(plan, 1) > 0);
    REQUIRE(planner.find(1) == nullptr);
    planner.commit();
    REQUIRE(planner.find(1) != nullptr);
}

TEST_CASE("PN22 maps forgotten, as when the atlas texture is made again, read as none until redrawn", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 1.f, 0.f}, 5.f),
                                               Spot(2, {2.f, 1.f, 0.f}, {0.f, 0.f, -1.f}, 5.f)};
    Frame(planner, lights, {}, camera, Small());
    REQUIRE(Frame(planner, lights, {}, camera, Small()).draws.empty());
    const AtlasTile held = TilesOf(planner, 1).at(0);

    planner.forgetMaps();
    REQUIRE(planner.find(1) == nullptr);
    REQUIRE(planner.find(2) == nullptr);
    // The same tiles, every face the camera can see drawn again.
    const ShadowPlan plan = planner.plan(lights, {}, camera, Small());
    REQUIRE(Draws(plan, 1) > 0);
    REQUIRE(Draws(plan, 2) == 1);
    REQUIRE(planner.find(1) == nullptr);
    planner.commit();
    REQUIRE(planner.find(1) != nullptr);
    REQUIRE(planner.find(2) != nullptr);
    const AtlasTile now = TilesOf(planner, 1).at(0);
    REQUIRE((now.x == held.x && now.y == held.y && now.page == held.page && now.size == held.size));
}

TEST_CASE("PN9 a skinned caster that holds still but changes its pose redraws its light and the sun", "[shadow]") {
    ShadowPlanner planner;
    const CameraView camera = Camera({0.f, 2.f, 10.f}, {0.f, 0.f, 0.f});
    const std::vector<ShadowRequest> lights = {Point(1, {0.f, 1.f, 0.f}, 5.f)};
    ShadowCaster bent = Box(100, {1.f, 0.f, 0.f});
    bent.pose = 7;
    Frame(planner, lights, {bent}, camera, Small());
    REQUIRE(Frame(planner, lights, {bent}, camera, Small()).draws.empty());
    bent.pose = 8;
    REQUIRE(Draws(Frame(planner, lights, {bent}, camera, Small()), 1) > 0);

    SunRequest sun;
    sun.shine = Normalize({0.3f, -1.f, 0.2f});
    sun.shadowDistance = 20.f;
    REQUIRE_FALSE(planner.planCascades(&sun, {bent}, camera, Small()).empty());
    planner.commitCascades();
    REQUIRE(planner.planCascades(&sun, {bent}, camera, Small()).empty());
    bent.pose = 9;
    REQUIRE_FALSE(planner.planCascades(&sun, {bent}, camera, Small()).empty());
}
