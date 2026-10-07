// Task 6 of the terrain LOD plan: which published LOD nodes the renderer
// draws each frame (runner/TerrainSelection) -- by pixel error, culled to the
// view, a node drawn in place of children not all published, and cross-fades
// between levels. Pure: synthetic node sets, no meshes, no GL.

#include "runner/TerrainSelection.hpp"

#include "TerrainWorld.hpp"
#include "terrain/LodNode.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

using Catch::Approx;
using engine_core::TerrainNodeView;
using engine_core::TerrainView;
using engine_core::Vec3;
using engine_core::terrain::NodeKey;
using namespace runner;

namespace {

constexpr int kPaneHeight = 1080;
constexpr int kPaneWidth = 1920;
constexpr float kFov = 60.f;

// A flat slab of n x n chunks (n a power of two) on y = 0, its surface within
// y 0 to 8, with every level up to the one node over all of it published, as
// TerrainWorld would once everything is built and resident. Voxel size 1: a
// chunk is 32 studs. Level L's error is its target, 0.25 * 2^L studs (0 at
// level 0). keep (when set) leaves out the nodes it returns false for, as
// not built yet: their parents still count them in child_mask.
TerrainView Slab(int n, const std::function<bool(const NodeKey&)>& keep = {}) {
    auto nodes = std::make_shared<std::vector<TerrainNodeView>>();
    int top = 0;
    std::uint64_t revision = 1;
    for (int level = 0; (n >> level) >= 1; ++level) {
        top = level;
        const int count = n >> level;
        for (int x = 0; x < count; ++x) {
            for (int z = 0; z < count; ++z) {
                TerrainNodeView node;
                node.key = NodeKey{level, x, 0, z};
                if (keep && !keep(node.key)) {
                    continue;
                }
                node.revision = revision++;
                engine_core::terrain::node_bounds(node.key, 1.f, node.bounds_min, node.bounds_max);
                node.bounds_min.y = 0.f;
                node.bounds_max.y = 8.f;
                node.error = level == 0 ? 0.f : 0.25f * static_cast<float>(1 << level);
                if (level > 0) {
                    const auto children = engine_core::terrain::children_of(node.key);
                    for (std::size_t i = 0; i < children.size(); ++i) {
                        if (children[i].y == 0) {
                            node.child_mask = static_cast<std::uint8_t>(node.child_mask | (1u << i));
                        }
                    }
                }
                nodes->push_back(node);
            }
        }
    }
    TerrainView view;
    view.terrain = 9;
    view.nodes = nodes;
    view.nodes_revision = 1;
    view.top_level = top;
    return view;
}

TerrainCamera Looking(Vec3 eye, Vec3 target, bool cull = true) {
    TerrainCamera camera;
    // Straight down needs an up that is not along the look.
    const bool down = std::abs(target.x - eye.x) < 1e-3f && std::abs(target.z - eye.z) < 1e-3f;
    camera.world = engine_core::matrix4_look_at(eye, target, down ? Vec3{0.f, 0.f, -1.f} : Vec3{0.f, 1.f, 0.f});
    camera.fov_y_degrees = kFov;
    camera.pane_height = kPaneHeight;
    camera.pane_width = cull ? kPaneWidth : 0;
    // These cameras stand kilometres off: no far plane short of them.
    camera.far_z = 1e7f;
    return camera;
}

std::vector<NodeChoice> Select(const TerrainView& view, const TerrainCamera& camera, double now,
                               TerrainFadeState& state) {
    std::vector<NodeChoice> out;
    SelectTerrainNodes(view, camera, now, state, out);
    return out;
}

std::vector<NodeChoice> SelectFresh(const TerrainView& view, const TerrainCamera& camera) {
    TerrainFadeState state;
    return Select(view, camera, 0.0, state);
}

const NodeKey& KeyOf(const TerrainView& view, const NodeChoice& choice) { return (*view.nodes)[choice.index].key; }

}  // namespace

TEST_CASE("SEL1 NodePixelError projects a node's error to pixels", "[terrain][lod][render]") {
    // 1 stud, 100 away, 90 degrees on a 200-pixel pane: 200 / (2 * tan 45) = 100 px per stud at 1 stud away.
    CHECK(NodePixelError(1.f, 100.f, 90.f, 200) == Approx(1.f));
    // 0.5 studs, 10 away, 60 degrees, 1080 pixels: 0.5 * 1080 / (2 * 0.57735) / 10 = 46.77.
    CHECK(NodePixelError(0.5f, 10.f, 60.f, 1080) == Approx(46.765f).epsilon(1e-3));
    // Twice as far, half the pixels.
    CHECK(NodePixelError(0.5f, 20.f, 60.f, 1080) == Approx(46.765f / 2.f).epsilon(1e-3));
    // Inside the node: infinite, unless there is no error at all.
    CHECK(std::isinf(NodePixelError(1.f, 0.f, 60.f, 1080)));
    CHECK(NodePixelError(0.f, 0.f, 60.f, 1080) == 0.f);
    CHECK(NodePixelError(0.f, 50.f, 60.f, 1080) == 0.f);
}

TEST_CASE("SEL2 a far camera draws the top node; at the surface, level 0 under it and coarser away, each spot once",
          "[terrain][lod][render]") {
    const TerrainView view = Slab(64);   // levels 0 to 6
    REQUIRE(view.top_level == 6);

    // 50,000 studs above the middle: the top node's 16 studs are under a pixel.
    const std::vector<NodeChoice> far = SelectFresh(view, Looking({1024.f, 50000.f, 1024.f}, {1024.f, 0.f, 1024.f}));
    REQUIRE(far.size() == 1);
    CHECK(KeyOf(view, far[0]).level == 6);
    CHECK(far[0].fade == 1.f);
    CHECK(far[0].incoming);

    // At the surface, looking across the slab to its far corner: the chunk the camera stands in is drawn
    // at level 0, and nodes far from it coarser.
    const Vec3 eye{40.f, 4.f, 40.f};
    const std::vector<NodeChoice> near = SelectFresh(view, Looking(eye, {2048.f, 4.f, 2048.f}));
    const bool underCamera = std::any_of(near.begin(), near.end(), [&](const NodeChoice& choice) {
        return KeyOf(view, choice) == NodeKey{0, 1, 0, 1};
    });
    CHECK(underCamera);
    int coarsest = 0;
    for (const NodeChoice& choice : near) {
        coarsest = std::max(coarsest, KeyOf(view, choice).level);
    }
    CHECK(coarsest >= 3);

    // Without culling, the chosen nodes cover each chunk of the slab exactly once.
    const std::vector<NodeChoice> all = SelectFresh(view, Looking(eye, {500.f, 4.f, 0.f}, false));
    std::vector<int> covered(64 * 64, 0);
    for (const NodeChoice& choice : all) {
        CHECK(choice.fade == 1.f);
        const NodeKey& key = KeyOf(view, choice);
        const int side = 1 << key.level;
        for (int x = key.x * side; x < (key.x + 1) * side; ++x) {
            for (int z = key.z * side; z < (key.z + 1) * side; ++z) {
                ++covered[static_cast<std::size_t>(x * 64 + z)];
            }
        }
    }
    CHECK(std::all_of(covered.begin(), covered.end(), [](int count) { return count == 1; }));
    // And each is the coarsest that is under a pixel: its error is (or it is
    // level 0), and its parent's is not.
    const auto distance = [&](const NodeKey& key) {
        Vec3 min, max;
        engine_core::terrain::node_bounds(key, 1.f, min, max);
        min.y = 0.f;
        max.y = 8.f;
        const float dx = std::max({min.x - eye.x, 0.f, eye.x - max.x});
        const float dy = std::max({min.y - eye.y, 0.f, eye.y - max.y});
        const float dz = std::max({min.z - eye.z, 0.f, eye.z - max.z});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const auto error = [](int level) { return level == 0 ? 0.f : 0.25f * static_cast<float>(1 << level); };
    int finest = 0;
    for (const NodeChoice& choice : all) {
        const NodeKey& key = KeyOf(view, choice);
        CHECK(NodePixelError(error(key.level), distance(key), kFov, kPaneHeight) < 1.f);
        if (key.level < view.top_level) {
            const NodeKey parent = engine_core::terrain::parent_of(key);
            CHECK(NodePixelError(error(parent.level), distance(parent), kFov, kPaneHeight) >= 1.f);
        }
        finest += key.level == 0 ? 1 : 0;
    }
    // Level 0 reaches about 470 studs (where level 1's half stud is a pixel), not the whole slab.
    CHECK(finest > 100);
    CHECK(finest < 64 * 64 / 4);
}

TEST_CASE("SEL3 nodes behind the camera are culled", "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    const Vec3 eye{500.f, 4.f, 500.f};
    // Looking along -Z: everything at z beyond the camera is behind it.
    const std::vector<NodeChoice> culled = SelectFresh(view, Looking(eye, {500.f, 4.f, 0.f}));
    const std::vector<NodeChoice> all = SelectFresh(view, Looking(eye, {500.f, 4.f, 0.f}, false));
    const auto behind = [&](const NodeChoice& choice) { return (*view.nodes)[choice.index].bounds_min.z > eye.z; };
    CHECK(std::count_if(all.begin(), all.end(), behind) > 0);
    CHECK(std::count_if(culled.begin(), culled.end(), behind) == 0);
    CHECK(culled.size() < all.size());
    // Turned around, the nodes behind are the ones drawn.
    const std::vector<NodeChoice> turned = SelectFresh(view, Looking(eye, {500.f, 4.f, 2000.f}));
    CHECK(std::count_if(turned.begin(), turned.end(), behind) > 0);
}

TEST_CASE("SEL4 a switch between levels cross-fades over a quarter second", "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    TerrainFadeState state;
    const TerrainCamera far = Looking({1024.f, 50000.f, 1024.f}, {1024.f, 0.f, 1024.f});
    // 10,000 above: the top node (16 studs) is 1.5 px, its children (8 studs) 0.75 px.
    const TerrainCamera closer = Looking({1024.f, 10000.f, 1024.f}, {1024.f, 0.f, 1024.f});

    std::vector<NodeChoice> out = Select(view, far, 0.0, state);
    REQUIRE(out.size() == 1);
    CHECK(out[0].fade == 1.f);
    out = Select(view, far, 0.5, state);
    REQUIRE(out.size() == 1);
    CHECK(out[0].fade == 1.f);

    const auto check = [&](double now, float incoming, bool parentDrawn) {
        out = Select(view, closer, now, state);
        int children = 0;
        int parents = 0;
        for (const NodeChoice& choice : out) {
            const NodeKey& key = KeyOf(view, choice);
            if (key.level == 5) {
                ++children;
                CHECK(choice.incoming);
                CHECK(choice.fade == Approx(incoming).margin(1e-4));
            } else {
                REQUIRE(key.level == 6);
                ++parents;
                CHECK(!choice.incoming);
                CHECK(choice.fade + incoming == Approx(1.f).margin(1e-4));
            }
        }
        CHECK(children == 4);
        CHECK(parents == (parentDrawn ? 1 : 0));
    };
    check(1.0, 0.f, true);       // the switch: children in at 0, the top out at 1
    check(1.125, 0.5f, true);    // halfway: each 0.5, summing to 1
    check(1.25, 1.f, false);     // done: the children whole, the top gone
    check(2.0, 1.f, false);

    // Back out: the top fades in and the children out, the same way.
    out = Select(view, far, 3.0, state);
    REQUIRE(out.size() == 5);
    for (const NodeChoice& choice : out) {
        const bool top = KeyOf(view, choice).level == 6;
        CHECK(choice.incoming == top);
        CHECK(choice.fade == (top ? 0.f : 1.f));
    }
    out = Select(view, far, 3.25, state);
    REQUIRE(out.size() == 1);
    CHECK(out[0].fade == 1.f);

    // A node seen for the first time with nothing related drawn before draws whole at once.
    TerrainFadeState fresh;
    out = Select(view, closer, 0.0, fresh);
    CHECK(out.size() == 4);
    CHECK(std::all_of(out.begin(), out.end(), [](const NodeChoice& choice) { return choice.fade == 1.f; }));
}

TEST_CASE("SEL5 a node with a child missing from the set is drawn instead of its children", "[terrain][lod][render]") {
    // As SEL4's closer camera, which draws the top's four children, but with
    // one child not published (not built yet): the top draws instead.
    const TerrainView missing = Slab(64, [](const NodeKey& key) { return !(key == NodeKey{5, 0, 0, 0}); });
    const TerrainCamera closer = Looking({1024.f, 10000.f, 1024.f}, {1024.f, 0.f, 1024.f});
    const std::vector<NodeChoice> out = SelectFresh(missing, closer);
    REQUIRE(out.size() == 1);
    CHECK(KeyOf(missing, out[0]).level == 6);

    // With the top not published either, its published descendants are roots:
    // the region draws from what is built.
    const TerrainView noTop =
        Slab(64, [](const NodeKey& key) { return key.level != 6 && !(key == NodeKey{5, 0, 0, 0}); });
    const std::vector<NodeChoice> roots = SelectFresh(noTop, closer);
    CHECK(roots.size() >= 3);
    CHECK(std::none_of(roots.begin(), roots.end(), [&](const NodeChoice& choice) {
        return KeyOf(noTop, choice).level == 6;
    }));
}

TEST_CASE("SEL6 selection over a 4 km island's nodes takes under half a millisecond", "[.][terrain-bench]") {
    // 128 x 128 chunks of 32 studs (4,096 studs a side), every level of it
    // published: 21,845 nodes, more than the ~16 k a 4 km island keeps resident.
    const TerrainView view = Slab(128);
    std::printf("SEL6: %zu nodes\n", view.nodes->size());
    TerrainFadeState state;
    std::vector<NodeChoice> out;
    const auto first = std::chrono::steady_clock::now();
    SelectTerrainNodes(view, Looking({100.f, 20.f, 100.f}, {200.f, 10.f, 200.f}), 0.0, state, out);
    const double firstMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - first).count();
    // A camera flying across the island at 60 frames a second.
    constexpr int kFrames = 600;
    std::size_t drawn = 0;
    double worstMs = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 1; frame <= kFrames; ++frame) {
        const float t = static_cast<float>(frame) / kFrames;
        const Vec3 eye{100.f + 3900.f * t, 20.f + 30.f * t, 100.f + 3900.f * t};
        const auto before = std::chrono::steady_clock::now();
        SelectTerrainNodes(view, Looking(eye, {eye.x + 100.f, 10.f, eye.z + 60.f}), frame / 60.0, state, out);
        worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                              before)
                                        .count());
        drawn += out.size();
    }
    const double averageMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
    std::printf("SEL6: first frame (roots) %.3f ms; per frame %.4f ms average, %.4f ms worst; %.0f nodes drawn a frame\n",
                firstMs, averageMs, worstMs, static_cast<double>(drawn) / kFrames);
    CHECK(averageMs < 0.5);
}
