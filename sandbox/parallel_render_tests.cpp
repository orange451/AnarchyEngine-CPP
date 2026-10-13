// The render loops that run in blocks on worker threads: whatever the block
// size, each must give what one loop over everything gives, in the same order.

#include "runner/DrawBatches.hpp"
#include "runner/ParallelBlocks.hpp"
#include "runner/RenderMath.hpp"
#include "runner/ShadowPlanner.hpp"
#include "runner/Visibility.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using engine_core::Vec3;
using namespace runner;

namespace {

// A small, fixed random sequence, so every run sees the same scene.
struct Random {
    std::uint32_t seed = 2024u;
    float next() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    }
    float between(float low, float high) { return low + (high - low) * next(); }
};

CameraView Camera(Vec3 eye, Vec3 target) {
    CameraView camera;
    camera.world = engine_core::matrix4_look_at(eye, target, {0.f, 1.f, 0.f});
    camera.viewProjection = engine_core::matrix4_multiply(Perspective(60.f, 1.5f, 0.1f, 1000.f),
                                                          LookAtView(eye, target, {0.f, 1.f, 0.f}));
    camera.fovYDegrees = 60.f;
    camera.aspect = 1.5f;
    camera.nearZ = 0.1f;
    camera.paneHeight = 720;
    return camera;
}

const float kBoxMin[3] = {-1.f, 0.f, -1.f};
const float kBoxMax[3] = {1.f, 6.f, 1.f};

// A field of draws around the origin: turned, scaled, some mirrored, some
// see-through, some not drawable, some shadow-only, some terrain, a few slots.
struct Field {
    std::vector<engine_core::Matrix4> models;
    std::vector<std::array<float, 3>> tints;
    std::vector<DrawItem> items;

    explicit Field(int count) {
        Random random;
        models.reserve(static_cast<std::size_t>(count));
        tints.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            engine_core::Matrix4 model = engine_core::matrix4_multiply(
                engine_core::matrix4_translation(random.between(-300.f, 300.f), random.between(-5.f, 5.f),
                                                 random.between(-300.f, 300.f)),
                engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, random.between(0.f, 6.28f)));
            const float scale = random.between(0.2f, 3.f) * (i % 17 == 0 ? -1.f : 1.f);
            for (int column = 0; column < 3; ++column) {
                for (int axis = 0; axis < 3; ++axis) {
                    model.m[column * 4 + axis] *= scale;
                }
            }
            models.push_back(model);
            // Most share white; some have their own color.
            tints.push_back(i % 5 == 0 ? std::array<float, 3>{random.next(), random.next(), random.next()}
                                       : std::array<float, 3>{1.f, 1.f, 1.f});
            DrawItem item;
            item.model = &models.back();
            item.tint = i % 7 == 0 ? nullptr : tints.back().data();
            item.boundsMin = kBoxMin;
            item.boundsMax = kBoxMax;
            item.drawable = i % 13 != 0;
            item.shadowOnly = i % 29 == 0;
            item.transparency = i % 11 == 0 ? 0.5f : 0.f;
            item.terrain = i % 97 == 0;
            item.slot = item.terrain ? 0u : static_cast<std::uint32_t>(i % 6);
            item.lod = static_cast<std::uint8_t>(i % 3 == 0 ? 1 : 0);
            item.boneBase = i % 31 == 0 ? i : -1;
            items.push_back(item);
        }
    }
};

bool SameSphere(const Sphere& a, const Sphere& b) {
    return std::memcmp(&a.center, &b.center, sizeof(a.center)) == 0 && a.radius == b.radius;
}

bool SameVisible(const std::vector<VisibleDraw>& a, const std::vector<VisibleDraw>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].index != b[i].index || a[i].lod != b[i].lod ||
            std::memcmp(&a[i].screenRadius, &b[i].screenRadius, sizeof(float)) != 0) {
            return false;
        }
    }
    return true;
}

ShadowCaster Caster(Random& random, std::uint64_t mesh) {
    ShadowCaster caster;
    caster.mesh = mesh;
    caster.revision = mesh % 3;
    caster.owner = mesh % 50 == 0 ? 7 : 0;
    const Vec3 at{random.between(-150.f, 150.f), random.between(0.f, 10.f), random.between(-150.f, 150.f)};
    caster.model = engine_core::matrix4_translation(at.x, at.y, at.z);
    caster.bounds = {at, random.between(0.5f, 4.f)};
    return caster;
}

}  // namespace

TEST_CASE("PAR1 ParallelSelect keeps what one loop keeps, in order, whatever the block size", "[parallel]") {
    const std::size_t count = 10'007;
    const auto keep = [](std::size_t i) { return (i * 2654435761u) % 7 < 3; };
    std::vector<int> expected;
    for (std::size_t i = 0; i < count; ++i) {
        if (keep(i)) {
            expected.push_back(static_cast<int>(i));
        }
    }
    for (const std::size_t grain : {std::size_t{1}, std::size_t{3}, std::size_t{64}, std::size_t{4096}, count * 2}) {
        std::vector<std::vector<int>> blocks;
        std::vector<int> kept = {99, 98};
        ParallelSelect(count, grain, keep, blocks, kept);
        INFO("grain " << grain);
        REQUIRE(kept == expected);
        REQUIRE(blocks.size() == BlockCount(count, grain));
    }
    // Nothing to look at keeps nothing.
    std::vector<std::vector<int>> blocks;
    std::vector<int> kept = {1};
    ParallelSelect(0, 8, keep, blocks, kept);
    REQUIRE(kept.empty());
}

TEST_CASE("PAR2 FindVisible gives the same spheres and lists whatever the block size", "[parallel][visibility]") {
    const Field field(20'000);
    const CameraView camera = Camera({0.f, 20.f, 0.f}, {100.f, 0.f, -60.f});
    VisibilityResult one;
    FindVisible(field.items.data(), static_cast<int>(field.items.size()), camera, true, one,
                std::numeric_limits<std::size_t>::max());
    REQUIRE(!one.opaque.empty());
    REQUIRE(!one.transparent.empty());
    REQUIRE(one.culled > 0);
    for (const std::size_t grain : {std::size_t{1}, std::size_t{7}, std::size_t{1000}, kVisibilityGrain}) {
        VisibilityResult many;
        // A result used before keeps nothing of it.
        FindVisible(field.items.data(), 50, camera, false, many, 3);
        FindVisible(field.items.data(), static_cast<int>(field.items.size()), camera, true, many, grain);
        INFO("grain " << grain);
        REQUIRE(many.culled == one.culled);
        REQUIRE(SameVisible(many.opaque, one.opaque));
        REQUIRE(SameVisible(many.transparent, one.transparent));
        REQUIRE(many.spheres.size() == one.spheres.size());
        for (std::size_t i = 0; i < one.spheres.size(); ++i) {
            REQUIRE(SameSphere(many.spheres[i], one.spheres[i]));
        }
    }
    // Each visible draw carries its item's LOD.
    for (const VisibleDraw& draw : one.opaque) {
        REQUIRE(draw.lod == field.items[static_cast<std::size_t>(draw.index)].lod);
    }
}

TEST_CASE("PAR3 BuildBatches gives the same runs and instances whatever the block size", "[parallel][batches]") {
    const Field field(20'000);
    const CameraView camera = Camera({0.f, 20.f, 0.f}, {100.f, 0.f, -60.f});
    VisibilityResult visible;
    FindVisible(field.items.data(), static_cast<int>(field.items.size()), camera, true, visible);
    const engine_core::Matrix4 view = LookAtView({0.f, 20.f, 0.f}, {100.f, 0.f, -60.f}, {0.f, 1.f, 0.f});
    DrawBatches one;
    BuildBatches(field.items.data(), visible, view, one, std::numeric_limits<std::size_t>::max());
    REQUIRE(one.opaqueRuns > 1);
    REQUIRE(one.instances.size() == visible.opaque.size() + visible.transparent.size());
    for (const std::size_t grain : {std::size_t{1}, std::size_t{5}, std::size_t{999}, kBatchGrain}) {
        DrawBatches many;
        BuildBatches(field.items.data(), visible, view, many, grain);
        INFO("grain " << grain);
        REQUIRE(many.opaqueRuns == one.opaqueRuns);
        REQUIRE(many.runs.size() == one.runs.size());
        for (std::size_t r = 0; r < one.runs.size(); ++r) {
            const DrawRun& a = many.runs[r];
            const DrawRun& b = one.runs[r];
            REQUIRE((a.first == b.first && a.count == b.count && a.draw == b.draw && a.lod == b.lod &&
                     a.mirrored == b.mirrored));
        }
        REQUIRE(many.instances.size() == one.instances.size());
        for (std::size_t i = 0; i < one.instances.size(); ++i) {
            REQUIRE(std::memcmp(&many.instances[i], &one.instances[i], sizeof(InstanceData)) == 0);
        }
    }
}

TEST_CASE("PAR4 cascades are planned the same whatever the block size, down to whether to redraw",
          "[parallel][shadow]") {
    Random random;
    std::vector<ShadowCaster> casters;
    for (std::uint64_t mesh = 1; mesh <= 12'000; ++mesh) {
        casters.push_back(Caster(random, mesh));
    }
    const CameraView camera = Camera({0.f, 15.f, 40.f}, {0.f, 0.f, 0.f});
    SunRequest sun;
    sun.shine = Normalize({0.4f, -1.f, 0.3f});
    sun.shadowDistance = 120.f;
    sun.owner = 7;
    const ShadowSettings settings;
    ShadowPlanner one;
    const std::vector<CascadeDraw> expected =
        one.planCascades(&sun, casters, camera, settings, std::numeric_limits<std::size_t>::max());
    REQUIRE(expected.size() == static_cast<std::size_t>(settings.cascadeCount));
    std::size_t inside = 0;
    for (const CascadeDraw& draw : expected) {
        inside += draw.casters.size();
        for (const int caster : draw.casters) {
            REQUIRE(casters[static_cast<std::size_t>(caster)].owner != sun.owner);
        }
    }
    REQUIRE(inside > 0);
    one.commitCascades();
    for (const std::size_t grain : {std::size_t{1}, std::size_t{9}, std::size_t{2000}, ShadowPlanner::kCascadeGrain}) {
        ShadowPlanner many;
        const std::vector<CascadeDraw> draws = many.planCascades(&sun, casters, camera, settings, grain);
        INFO("grain " << grain);
        REQUIRE(draws.size() == expected.size());
        for (std::size_t c = 0; c < draws.size(); ++c) {
            REQUIRE(draws[c].layer == expected[c].layer);
            REQUIRE(std::memcmp(&draws[c].viewProjection, &expected[c].viewProjection,
                                sizeof(engine_core::Matrix4)) == 0);
            REQUIRE(draws[c].casters == expected[c].casters);
        }
        // Planned again at another block size, nothing changed: nothing to redraw.
        many.commitCascades();
        REQUIRE(many.planCascades(&sun, casters, camera, settings, grain == 1 ? 4096 : 1).empty());
    }
    // One caster moved: every block size redraws.
    std::vector<ShadowCaster> moved = casters;
    for (std::size_t c = 0; c < moved.size(); ++c) {
        if (moved[c].owner != sun.owner && moved[c].bounds.center.x > -10.f && moved[c].bounds.center.x < 10.f &&
            moved[c].bounds.center.z > -10.f && moved[c].bounds.center.z < 10.f) {
            moved[c].model.m[12] += 0.25f;
            break;
        }
    }
    REQUIRE_FALSE(one.planCascades(&sun, moved, camera, settings, 3).empty());
}

TEST_CASE("PAR5 ParallelSort sorts as std::sort does, whatever the block size", "[parallel]") {
    Random random;
    std::vector<std::pair<int, int>> items;
    for (int i = 0; i < 25'000; ++i) {
        // Many ties in the first field; the second makes the order total.
        items.emplace_back(static_cast<int>(random.next() * 50.f), i);
    }
    std::vector<std::pair<int, int>> expected = items;
    std::sort(expected.begin(), expected.end());
    for (const std::size_t grain :
         {std::size_t{1}, std::size_t{3}, std::size_t{1000}, std::size_t{8192}, std::size_t{1u << 20}}) {
        std::vector<std::pair<int, int>> sorted = items;
        ParallelSort(sorted, grain, [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a < b; });
        INFO("grain " << grain);
        REQUIRE(sorted == expected);
    }
}

TEST_CASE("PAR6 every block runs once, and a loop inside a block runs where it is", "[parallel]") {
    std::vector<int> runs(500, 0);
    std::vector<int> inner(500 * 20, 0);
    RunBlocks(runs.size(), [&](std::size_t block) {
        ++runs[block];
        // Nested: must not wait on the pool this block is running on.
        RunBlocks(20, [&](std::size_t sub) { ++inner[block * 20 + sub]; });
    });
    REQUIRE(std::all_of(runs.begin(), runs.end(), [](int count) { return count == 1; }));
    REQUIRE(std::all_of(inner.begin(), inner.end(), [](int count) { return count == 1; }));
    // Many loops in a row, as a frame runs them.
    std::vector<std::size_t> sums(64, 0);
    for (int loop = 0; loop < 200; ++loop) {
        RunBlocks(sums.size(), [&](std::size_t block) { sums[block] += block; });
    }
    for (std::size_t block = 0; block < sums.size(); ++block) {
        REQUIRE(sums[block] == block * 200);
    }
}

TEST_CASE("PAR7 ranges normalize; bounds kept but for changed ranges cull as if all were new", "[parallel][visibility]") {
    std::vector<DrawRange> ranges = {{10, 5}, {0, 2}, {14, 3}, {2, 1}, {40, 0}, {30, 4}, {31, 1}};
    NormalizeRanges(ranges);
    REQUIRE(ranges.size() == 3);
    REQUIRE((ranges[0].first == 0 && ranges[0].count == 3));
    REQUIRE((ranges[1].first == 10 && ranges[1].count == 7));
    REQUIRE((ranges[2].first == 30 && ranges[2].count == 4));

    Field field(20'000);
    const CameraView camera = Camera({0.f, 20.f, 0.f}, {100.f, 0.f, -60.f});
    VisibilityResult kept;
    UpdateDrawBounds(field.items.data(), static_cast<int>(field.items.size()), true, {}, kept, 1000);
    // Some draws move, some stop or start being drawable; only their ranges are bounded again.
    std::vector<DrawRange> changed = {{5, 3}, {9'000, 2'500}, {19'990, 10}};
    for (const DrawRange& range : changed) {
        for (std::size_t i = range.first; i < range.first + range.count; ++i) {
            field.models[i].m[12] += 40.f;
            field.items[i].drawable = !field.items[i].drawable;
            field.items[i].lod = static_cast<std::uint8_t>(2);
        }
    }
    UpdateDrawBounds(field.items.data(), static_cast<int>(field.items.size()), false, changed, kept, 700);
    CullDraws(static_cast<int>(field.items.size()), camera, true, kept, 333);
    VisibilityResult fresh;
    FindVisible(field.items.data(), static_cast<int>(field.items.size()), camera, true, fresh);
    REQUIRE(kept.culled == fresh.culled);
    REQUIRE(SameVisible(kept.opaque, fresh.opaque));
    REQUIRE(SameVisible(kept.transparent, fresh.transparent));
    for (std::size_t i = 0; i < fresh.spheres.size(); ++i) {
        REQUIRE(SameSphere(kept.spheres[i], fresh.spheres[i]));
        REQUIRE(kept.traits[i] == fresh.traits[i]);
    }
    // Fewer items: the lists and arrays follow.
    UpdateDrawBounds(field.items.data(), 100, false, {}, kept);
    CullDraws(100, camera, true, kept);
    FindVisible(field.items.data(), 100, camera, true, fresh);
    REQUIRE(kept.spheres.size() == 100);
    REQUIRE(SameVisible(kept.opaque, fresh.opaque));
}

TEST_CASE("PAR8 a caster view plans as the casters it holds, and those with no fingerprint cast nothing",
          "[parallel][shadow]") {
    Random random;
    std::vector<ShadowCaster> casters;
    for (std::uint64_t mesh = 1; mesh <= 3'000; ++mesh) {
        casters.push_back(Caster(random, mesh));
    }
    // The view holds every caster with one that casts nothing after every fourth.
    std::vector<Sphere> bounds;
    std::vector<std::uint64_t> owners;
    std::vector<std::uint64_t> fingerprints;
    std::vector<int> viewIndex;
    for (std::size_t c = 0; c < casters.size(); ++c) {
        viewIndex.push_back(static_cast<int>(bounds.size()));
        bounds.push_back(casters[c].bounds);
        owners.push_back(casters[c].owner);
        fingerprints.push_back(ShadowCasterFingerprint(casters[c]));
        if (c % 4 == 3) {
            bounds.push_back(casters[c].bounds);
            owners.push_back(0);
            fingerprints.push_back(0);
        }
    }
    const CasterView view{bounds.size(), bounds.data(), owners.data(), fingerprints.data()};
    const CameraView camera = Camera({0.f, 15.f, 40.f}, {0.f, 0.f, 0.f});
    SunRequest sun;
    sun.shine = Normalize({0.4f, -1.f, 0.3f});
    sun.shadowDistance = 120.f;
    sun.owner = 7;
    const ShadowSettings settings;
    ShadowPlanner byVector;
    ShadowPlanner byView;
    const std::vector<CascadeDraw> expected = byVector.planCascades(&sun, casters, camera, settings);
    const std::vector<CascadeDraw> drawn = byView.planCascadesView(&sun, view, camera, settings, 500);
    REQUIRE(drawn.size() == expected.size());
    for (std::size_t c = 0; c < drawn.size(); ++c) {
        std::vector<int> mapped;
        for (const int caster : expected[c].casters) {
            mapped.push_back(viewIndex[static_cast<std::size_t>(caster)]);
        }
        REQUIRE(drawn[c].casters == mapped);
        REQUIRE(std::memcmp(&drawn[c].viewProjection, &expected[c].viewProjection, sizeof(engine_core::Matrix4)) == 0);
    }
    // Lights too: the same tiles, from the same casters.
    std::vector<ShadowRequest> lights;
    ShadowRequest point;
    point.key = 5;
    point.owner = 5;
    point.kind = ShadowKind::Point;
    point.position = {0.f, 3.f, 0.f};
    point.radius = 25.f;
    lights.push_back(point);
    const ShadowPlan planVector = byVector.plan(lights, casters, camera, settings);
    const ShadowPlan planView = byView.planView(lights, view, camera, settings);
    REQUIRE(planView.draws.size() == planVector.draws.size());
    for (std::size_t d = 0; d < planView.draws.size(); ++d) {
        std::vector<int> mapped;
        for (const int caster : planVector.draws[d].casters) {
            mapped.push_back(viewIndex[static_cast<std::size_t>(caster)]);
        }
        REQUIRE(planView.draws[d].casters == mapped);
    }
}
