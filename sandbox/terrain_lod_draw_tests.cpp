// Task 6 of the terrain LOD plan: which published LOD nodes the renderer
// draws each frame (runner/TerrainSelection) -- by pixel error, culled to the
// view, a node drawn in place of children not all published, and cross-fades
// between levels. Pure: synthetic node sets, no meshes, no GL.

#include "runner/RenderMath.hpp"
#include "runner/ShadowMath.hpp"
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
#include <string>
#include <tuple>
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
// chunk is 32 units. Level L's error is its target, 0.25 * 2^L units (0 at
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

// The 16 dither thresholds terrain.frag tests a fade against (kBayer).
float Threshold(int i) {
    static const float kBayer[16] = {0.f, 8.f, 2.f, 10.f, 12.f, 4.f, 14.f, 6.f,
                                     3.f, 11.f, 1.f, 9.f, 15.f, 7.f, 13.f, 5.f};
    return (kBayer[i] + 0.5f) / 16.f;
}

// Whether choice draws a pixel of threshold th, as terrain.frag decides.
bool Draws(const NodeChoice& choice, float th) {
    return choice.incoming ? !(th > choice.fade) : !(th <= 1.f - choice.fade);
}

// Over each chunk of an n x n Slab whose x and z are within [x0, x1) and
// [z0, z1) (chunks), and each of the 16 dither thresholds: how many choices
// draw it. Empty when every one is drawn exactly once; else the first that
// is not, for the failure message.
std::string Coverage(const TerrainView& view, const std::vector<NodeChoice>& choices, int n, int x0 = 0,
                     int x1 = -1, int z0 = 0, int z1 = -1) {
    x1 = x1 < 0 ? n : x1;
    z1 = z1 < 0 ? n : z1;
    std::vector<int> count(static_cast<std::size_t>(n * n * 16), 0);
    for (const NodeChoice& choice : choices) {
        const NodeKey& key = KeyOf(view, choice);
        const int side = 1 << key.level;
        for (int t = 0; t < 16; ++t) {
            if (!Draws(choice, Threshold(t))) {
                continue;
            }
            for (int x = std::max(key.x * side, x0); x < std::min((key.x + 1) * side, x1); ++x) {
                for (int z = std::max(key.z * side, z0); z < std::min((key.z + 1) * side, z1); ++z) {
                    ++count[static_cast<std::size_t>((x * n + z) * 16 + t)];
                }
            }
        }
    }
    for (int x = x0; x < x1; ++x) {
        for (int z = z0; z < z1; ++z) {
            for (int t = 0; t < 16; ++t) {
                const int c = count[static_cast<std::size_t>((x * n + z) * 16 + t)];
                if (c != 1) {
                    std::string text = "chunk " + std::to_string(x) + "," + std::to_string(z) + " threshold " +
                                       std::to_string(Threshold(t)) + " drawn " + std::to_string(c) + " times by:";
                    for (const NodeChoice& choice : choices) {
                        const NodeKey& key = KeyOf(view, choice);
                        const int side = 1 << key.level;
                        if (x >= key.x * side && x < (key.x + 1) * side && z >= key.z * side &&
                            z < (key.z + 1) * side) {
                            text += " L" + std::to_string(key.level) + "(" + std::to_string(key.x) + "," +
                                    std::to_string(key.z) + ")" + (choice.incoming ? " in " : " out ") +
                                    std::to_string(choice.fade);
                        }
                    }
                    return text;
                }
            }
        }
    }
    return {};
}

}  // namespace

TEST_CASE("SEL1 NodePixelError projects a node's error to pixels", "[terrain][lod][render]") {
    // 1 unit, 100 away, 90 degrees on a 200-pixel pane: 200 / (2 * tan 45) = 100 px per unit at 1 unit away.
    CHECK(NodePixelError(1.f, 100.f, 90.f, 200) == Approx(1.f));
    // 0.5 units, 10 away, 60 degrees, 1080 pixels: 0.5 * 1080 / (2 * 0.57735) / 10 = 46.77.
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

    // 50,000 units above the middle: the top node's 16 units are under a pixel.
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
    // Level 0 reaches about 470 units (where level 1's half unit is a pixel), not the whole slab.
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
    // 10,000 above: the top node (16 units) is 1.5 px, its children (8 units) 0.75 px.
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
    // The three level-5 nodes built and the four level-4 children of the one not: each chunk once.
    CHECK(roots.size() == 7);
    CHECK(Coverage(noTop, roots, 64) == "");
    CHECK(std::none_of(roots.begin(), roots.end(), [&](const NodeChoice& choice) {
        return KeyOf(noTop, choice).level == 6;
    }));
    CHECK(Coverage(missing, out, 64) == "");
}

TEST_CASE("SEL15 an edit swaps nodes in place: new revisions and new nodes draw whole, nothing coarsens",
          "[terrain][lod][render]") {
    // Ruling R24. Close over the corner of an 8 x 8 slab: level 0 there.
    const TerrainView slab = Slab(8);
    const TerrainCamera close = Looking({16.f, 20.f, 16.f}, {16.f, 0.f, 16.f}, false);
    TerrainFadeState state;
    Select(slab, close, 0.0, state);
    const std::vector<NodeChoice> settled = Select(slab, close, 1.0, state);
    REQUIRE(Coverage(slab, settled, 8) == "");
    const auto name = [](const NodeKey& key) {
        return "L" + std::to_string(key.level) + "(" + std::to_string(key.x) + "," + std::to_string(key.y) + "," +
               std::to_string(key.z) + ")";
    };
    // The chosen keys, sorted; each drawn whole (no fade).
    const auto keys = [&](const TerrainView& view, const std::vector<NodeChoice>& choices) {
        std::vector<std::string> out;
        for (const NodeChoice& choice : choices) {
            INFO(name(KeyOf(view, choice)));
            CHECK(choice.incoming);
            CHECK(choice.fade == 1.f);
            out.push_back(name(KeyOf(view, choice)));
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    const std::vector<std::string> before = keys(slab, settled);

    // 1. The edit re-meshed every node: the same keys with new revisions, a new set.
    TerrainView remeshed = slab;
    auto nodes = std::make_shared<std::vector<TerrainNodeView>>(*slab.nodes);
    for (TerrainNodeView& node : *nodes) node.revision += 1000;
    remeshed.nodes = nodes;
    remeshed.nodes_revision = 2;
    for (const double now : {1.1, 1.2, 1.4}) {
        INFO("re-meshed, at " << now);
        CHECK(keys(remeshed, Select(remeshed, close, now, state)) == before);
    }

    // 2. The edit made surface in a region with none: chunk (0, 2, 0), whose
    // level-1 parent is not built yet, so the level-2 node over it leaves that
    // parent out of its child_mask. It is a root and draws whole at once; the
    // rest is as it was (nothing coarser), no hole.
    TerrainView grown = remeshed;
    auto withNew = std::make_shared<std::vector<TerrainNodeView>>(*nodes);
    TerrainNodeView fresh;
    fresh.key = NodeKey{0, 0, 2, 0};
    fresh.revision = 5000;
    engine_core::terrain::node_bounds(fresh.key, 1.f, fresh.bounds_min, fresh.bounds_max);
    withNew->insert(withNew->begin(), fresh);
    grown.nodes = withNew;
    grown.nodes_revision = 3;
    std::vector<std::string> withFresh = before;
    withFresh.push_back(name(fresh.key));
    std::sort(withFresh.begin(), withFresh.end());
    for (const double now : {1.5, 1.6, 1.8}) {
        INFO("new chunk, at " << now);
        CHECK(keys(grown, Select(grown, close, now, state)) == withFresh);
    }

    // 3. Its parent is built: the level-2 node now counts it, selection
    // reaches the new chunk through it, and still nothing fades.
    TerrainView built = grown;
    auto withParent = std::make_shared<std::vector<TerrainNodeView>>(*withNew);
    TerrainNodeView parent;
    parent.key = NodeKey{1, 0, 1, 0};
    parent.revision = 5001;
    parent.error = 0.5f;
    parent.child_mask = 1u << 0;   // child (0, 2, 0)
    engine_core::terrain::node_bounds(parent.key, 1.f, parent.bounds_min, parent.bounds_max);
    for (TerrainNodeView& node : *withParent) {
        if (node.key == NodeKey{2, 0, 0, 0}) node.child_mask = static_cast<std::uint8_t>(node.child_mask | (1u << 2));
    }
    withParent->push_back(parent);
    std::sort(withParent->begin(), withParent->end(), [](const TerrainNodeView& a, const TerrainNodeView& b) {
        return std::tie(a.key.level, a.key.x, a.key.y, a.key.z) < std::tie(b.key.level, b.key.x, b.key.y, b.key.z);
    });
    built.nodes = withParent;
    built.nodes_revision = 4;
    for (const double now : {1.9, 2.0, 2.2}) {
        INFO("parent built, at " << now);
        CHECK(keys(built, Select(built, close, now, state)) == withFresh);
    }
}

TEST_CASE("SEL6 selection over a 4 km island's nodes takes under half a millisecond", "[.][terrain-bench]") {
    // 128 x 128 chunks of 32 units (4,096 units a side), every level of it
    // published: 21,845 nodes, more than the ~16 k a 4 km island keeps resident.
    TerrainView view = Slab(128);
    std::printf("SEL6: %zu nodes\n", view.nodes->size());
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    TerrainFadeState state;
    std::vector<NodeChoice> out;
    std::vector<std::size_t> casters, keep, upload;
    const auto first = Clock::now();
    SelectTerrainNodes(view, Looking({100.f, 20.f, 100.f}, {200.f, 10.f, 200.f}), 0.0, state, out);
    const double firstMs = ms(first, Clock::now());
    // A camera flying across the island at 60 frames a second: the frame's
    // selection, then its shadow casters, then its prefetch (nothing held).
    constexpr int kFrames = 600;
    std::size_t drawn = 0;
    std::size_t cast = 0;
    std::size_t candidates = 0;
    double worstMs = 0.0;
    double selectMs = 0.0;
    double castersMs = 0.0;
    double prefetchMs = 0.0;
    for (int frame = 1; frame <= kFrames; ++frame) {
        const float t = static_cast<float>(frame) / kFrames;
        const Vec3 eye{100.f + 3900.f * t, 20.f + 30.f * t, 100.f + 3900.f * t};
        const TerrainCamera camera = Looking(eye, {eye.x + 100.f, 10.f, eye.z + 60.f});
        const auto before = Clock::now();
        SelectTerrainNodes(view, camera, frame / 60.0, state, out);
        const auto selected = Clock::now();
        SelectTerrainCasters(view, camera, state, casters);
        const auto casted = Clock::now();
        SelectTerrainPrefetch(view, camera, out, state, [](std::size_t) { return false; }, keep, upload);
        const auto prefetched = Clock::now();
        worstMs = std::max(worstMs, ms(before, selected));
        selectMs += ms(before, selected);
        castersMs += ms(selected, casted);
        prefetchMs += ms(casted, prefetched);
        drawn += out.size();
        cast += casters.size();
        candidates += upload.size() + keep.size();
    }
    const double averageMs = selectMs / kFrames;
    std::printf("SEL6: first frame (index and roots) %.3f ms; selection per frame %.4f ms average, %.4f ms worst; "
                "%.0f nodes drawn a frame\n",
                firstMs, averageMs, worstMs, static_cast<double>(drawn) / kFrames);
    std::printf("SEL6: shadow casters out of view %.4f ms a frame, %.0f of them; prefetch %.4f ms a frame\n",
                castersMs / kFrames, static_cast<double>(cast) / kFrames, prefetchMs / kFrames);
    // A new node set every frame (a build or an edit publishing). Re-meshed
    // nodes only (the same keys): the index is kept. Keys added or gone
    // (alternating with a set missing one leaf): the index and roots each time.
    const TerrainCamera still = Looking({2000.f, 20.f, 2000.f}, {2100.f, 10.f, 2060.f});
    TerrainView fewer = Slab(128, [](const NodeKey& key) { return !(key == NodeKey{0, 3, 0, 3}); });
    constexpr int kRebuilds = 100;
    const auto average = [&](const std::function<void(int)>& before, double from) {
        double total = 0.0;
        for (int i = 0; i < kRebuilds; ++i) {
            before(i);
            const TerrainView& shown = (i % 2 == 1 && fewer.nodes_revision >= 1000) ? fewer : view;
            const auto start = Clock::now();
            SelectTerrainNodes(shown, still, from + i / 60.0, state, out);
            total += ms(start, Clock::now());
        }
        return total / kRebuilds;
    };
    const double steadyMs = average([](int) {}, 20.0);
    const double remeshedMs = average([&](int i) { view.nodes_revision = 100 + static_cast<std::uint64_t>(i); }, 30.0);
    const double changedMs = average(
        [&](int i) {
            view.nodes_revision = 1000 + 2 * static_cast<std::uint64_t>(i);
            fewer.nodes_revision = 1001 + 2 * static_cast<std::uint64_t>(i);
        },
        40.0);
    std::printf("SEL6: a frame with the same node set %.4f ms; with a new set of the same keys %.4f ms (+%.4f); "
                "with keys changed %.4f ms (+%.4f, the index and roots rebuilt)\n",
                steadyMs, remeshedMs, remeshedMs - steadyMs, changedMs, changedMs - steadyMs);
    CHECK(averageMs < 0.5);
}

namespace {

// SEL4's cameras over Slab(64), looking straight down at its middle: the top
// node alone (far), its 4 children (closer), its 16 grandchildren (closest).
const TerrainCamera kFar = Looking({1024.f, 50000.f, 1024.f}, {1024.f, 0.f, 1024.f});
const TerrainCamera kCloser = Looking({1024.f, 10000.f, 1024.f}, {1024.f, 0.f, 1024.f});
const TerrainCamera kClosest = Looking({1024.f, 5000.f, 1024.f}, {1024.f, 0.f, 1024.f});

int CountLevel(const TerrainView& view, const std::vector<NodeChoice>& choices, int level, bool incoming) {
    return static_cast<int>(std::count_if(choices.begin(), choices.end(), [&](const NodeChoice& choice) {
        return KeyOf(view, choice).level == level && choice.incoming == incoming;
    }));
}

}  // namespace

TEST_CASE("SEL7 a region mid-fade finishes its fade before it switches again, every pixel drawn once",
          "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    TerrainFadeState state;
    Select(view, kFar, 0.0, state);
    std::vector<NodeChoice> out = Select(view, kCloser, 1.0, state);   // top -> 4 children begins
    CHECK(Coverage(view, out, 64) == "");

    SECTION("zooming in again at +0.1 s") {
        // The grandchildren are wanted now, and the children, still fading in,
        // are held. But the top fading out is 3 px from here, over
        // kTerrainFadeOutPixelError: it goes at once (R23), and the children draw whole.
        out = Select(view, kClosest, 1.1, state);
        CHECK(CountLevel(view, out, 5, true) == 4);
        CHECK(CountLevel(view, out, 6, false) == 0);
        CHECK(CountLevel(view, out, 4, true) == 0);
        CHECK(std::all_of(out.begin(), out.end(), [](const NodeChoice& choice) { return choice.fade == 1.f; }));
        CHECK(Coverage(view, out, 64) == "");
        // Whole, they hold nothing: the switch to the grandchildren starts, the children (1.5 px) fading out.
        out = Select(view, kClosest, 1.2, state);
        CHECK(CountLevel(view, out, 4, true) == 16);
        CHECK(CountLevel(view, out, 5, false) == 4);
        CHECK(CountLevel(view, out, 6, false) == 0);
        CHECK(Coverage(view, out, 64) == "");
        out = Select(view, kClosest, 1.35, state);
        CHECK(Coverage(view, out, 64) == "");
        out = Select(view, kClosest, 1.45, state);
        CHECK(out.size() == 16);
        CHECK(Coverage(view, out, 64) == "");
    }
    SECTION("zooming back out at +0.1 s") {
        out = Select(view, kFar, 1.1, state);
        CHECK(CountLevel(view, out, 5, true) == 4);
        CHECK(CountLevel(view, out, 6, false) == 1);
        CHECK(Coverage(view, out, 64) == "");
        out = Select(view, kFar, 1.25, state);
        CHECK(CountLevel(view, out, 6, true) == 1);
        CHECK(CountLevel(view, out, 5, false) == 4);
        CHECK(Coverage(view, out, 64) == "");
        out = Select(view, kFar, 1.4, state);
        CHECK(Coverage(view, out, 64) == "");
        out = Select(view, kFar, 1.5, state);
        CHECK(out.size() == 1);
    }
}

TEST_CASE("SEL8 zooming in and out, and wandering, draws every pixel once each frame", "[terrain][lod][render]") {
    const TerrainView view = Slab(32);   // levels 0 to 5
    const auto run = [&](const std::function<Vec3(int)>& eyeAt, int frames, double dt) {
        TerrainFadeState state;
        int finest = 99;
        int coarsest = -1;
        for (int frame = 0; frame < frames; ++frame) {
            const Vec3 eye = eyeAt(frame);
            const std::vector<NodeChoice> out =
                Select(view, Looking(eye, {eye.x + 1.f, 0.f, eye.z + 1.f}, false), frame * dt, state);
            const std::string coverage = Coverage(view, out, 32);
            INFO("frame " << frame << " eye " << eye.x << "," << eye.y << "," << eye.z);
            REQUIRE(coverage == "");
            for (const NodeChoice& choice : out) {
                finest = std::min(finest, KeyOf(view, choice).level);
                coarsest = std::max(coarsest, KeyOf(view, choice).level);
            }
        }
        return std::make_pair(finest, coarsest);
    };
    // Down from 40,000 units to 20 over 3 s at 60 frames a second, then back up.
    const auto height = [](int frame) {
        const float t = static_cast<float>(frame) / 180.f;
        return 40000.f * std::pow(20.f / 40000.f, t);
    };
    const auto in = run([&](int frame) { return Vec3{500.f, height(frame), 500.f}; }, 181, 1.0 / 60.0);
    CHECK(in.first == 0);
    CHECK(in.second == 5);
    const auto out = run([&](int frame) { return Vec3{500.f, height(180 - frame), 500.f}; }, 181, 1.0 / 60.0);
    CHECK(out.first == 0);
    CHECK(out.second == 5);
    // Jumping about at random, faster than a fade, at 30 frames a second.
    unsigned seed = 12345u;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    std::vector<Vec3> eyes;
    for (int i = 0; i < 240; ++i) {
        const float x = next() * 1024.f;
        const float h = next() * next();
        const float z = next() * 1024.f;
        eyes.push_back(Vec3{x, 5.f + h * 20000.f, z});
    }
    run([&](int frame) { return eyes[static_cast<std::size_t>(frame)]; }, 240, 1.0 / 30.0);
}

TEST_CASE("SEL9 a child coming into view while its parent fades out takes the other half of that fade",
          "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    // A narrow view (10 degrees, 50 x 1080 pixels) along +X from 70,000 units
    // off the slab's low-X side: it sees a strip of the slab ~570 units wide.
    // The top node (16 units of error) is 1.4 px there, its children 0.7 px.
    const auto camera = [](Vec3 eye, float aimZ) {
        TerrainCamera c = Looking(eye, {0.f, 4.f, aimZ});
        c.fov_y_degrees = 10.f;
        c.pane_width = 50;
        return c;
    };
    TerrainFadeState state;
    std::vector<NodeChoice> out = Select(view, camera({-150000.f, 4.f, 512.f}, 512.f), 0.0, state);
    REQUIRE(out.size() == 1);
    CHECK(KeyOf(view, out[0]).level == 6);
    // Closer, aimed at z 512: the top's children at z 0-1024 switch in; those at z 1024-2048 are out of view.
    out = Select(view, camera({-70000.f, 4.f, 512.f}, 512.f), 1.0, state);
    CHECK(CountLevel(view, out, 5, true) == 2);
    CHECK(CountLevel(view, out, 6, false) == 1);
    CHECK(Coverage(view, out, 64, 0, 64, 0, 32) == "");
    // Turned to z 1536 at +0.1 s: the other two come into view, joining the top's fade at 0.4.
    out = Select(view, camera({-70000.f, 4.f, 512.f}, 1536.f), 1.1, state);
    int joined = 0;
    float top = -1.f;
    for (const NodeChoice& choice : out) {
        const NodeKey& key = KeyOf(view, choice);
        if (key.level == 5 && key.z == 1) {
            ++joined;
            CHECK(choice.incoming);
            CHECK(choice.fade == Approx(0.4f).margin(1e-4));
        } else if (key.level == 6) {
            CHECK(!choice.incoming);
            top = choice.fade;
        }
    }
    CHECK(joined == 2);
    CHECK(top == Approx(0.6f).margin(1e-4));
    CHECK(Coverage(view, out, 64, 0, 64, 32, 64) == "");
    out = Select(view, camera({-70000.f, 4.f, 512.f}, 1536.f), 1.2, state);
    CHECK(Coverage(view, out, 64, 0, 64, 32, 64) == "");
    out = Select(view, camera({-70000.f, 4.f, 512.f}, 1536.f), 1.3, state);
    CHECK(CountLevel(view, out, 5, true) == 2);
    CHECK(CountLevel(view, out, 6, false) == 0);
    CHECK(Coverage(view, out, 64, 0, 64, 32, 64) == "");
}

TEST_CASE("SEL10 a node of a fade gone from the set mid-fade leaves no hole", "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    TerrainFadeState state;
    Select(view, kFar, 0.0, state);
    Select(view, kCloser, 1.0, state);   // top fading out, 4 children in
    SECTION("the node fading out goes") {
        TerrainView noTop = Slab(64, [](const NodeKey& key) { return key.level != 6; });
        noTop.nodes_revision = 2;
        const std::vector<NodeChoice> out = Select(noTop, kCloser, 1.1, state);
        CHECK(out.size() == 4);
        CHECK(Coverage(noTop, out, 64) == "");
    }
    SECTION("a node fading in goes") {
        TerrainView missing = Slab(64, [](const NodeKey& key) { return !(key == NodeKey{5, 0, 0, 0}); });
        missing.nodes_revision = 2;
        std::vector<NodeChoice> out = Select(missing, kCloser, 1.1, state);
        REQUIRE(out.size() == 1);
        CHECK(KeyOf(missing, out[0]).level == 6);
        CHECK(Coverage(missing, out, 64) == "");
        // Built again: the switch starts over, from whole nodes.
        TerrainView again = Slab(64);
        again.nodes_revision = 3;
        out = Select(again, kCloser, 1.2, state);
        CHECK(Coverage(again, out, 64) == "");
        CHECK(CountLevel(again, out, 5, true) == 4);
    }
}

TEST_CASE("SEL11 terrain out of view casts shadows at the selection it would draw at", "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    const Vec3 eye{500.f, 4.f, 500.f};
    TerrainFadeState state;
    std::vector<NodeChoice> out;
    std::vector<std::size_t> casters;
    // Looking along -Z, after a step that leaves fades going.
    SelectTerrainNodes(view, Looking({500.f, 4.f, 700.f}, {500.f, 4.f, 0.f}), 0.0, state, out);
    SelectTerrainNodes(view, Looking(eye, {500.f, 4.f, 0.f}), 0.1, state, out);
    SelectTerrainCasters(view, Looking(eye, {500.f, 4.f, 0.f}), state, casters);
    const auto behind = [&](std::size_t index) { return (*view.nodes)[index].bounds_min.z > eye.z; };
    CHECK(std::count_if(casters.begin(), casters.end(), behind) > 0);
    // Nearest first (AppendTerrainDraws uploads the first few not yet uploaded each frame).
    const auto distance = [&](std::size_t index) {
        const TerrainNodeView& node = (*view.nodes)[index];
        const float dx = std::max({node.bounds_min.x - eye.x, eye.x - node.bounds_max.x, 0.f});
        const float dy = std::max({node.bounds_min.y - eye.y, eye.y - node.bounds_max.y, 0.f});
        const float dz = std::max({node.bounds_min.z - eye.z, eye.z - node.bounds_max.z, 0.f});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    REQUIRE(casters.size() > 1);
    CHECK(distance(casters.front()) < distance(casters.back()));
    for (std::size_t i = 1; i < casters.size(); ++i) {
        CHECK(distance(casters[i - 1]) <= distance(casters[i]) + 1e-3f);
    }
    // None of them drawn; with what is drawn fading in (or steady), every chunk once.
    REQUIRE(std::any_of(out.begin(), out.end(), [](const NodeChoice& choice) { return !choice.incoming; }));
    std::vector<NodeChoice> cast;
    for (const NodeChoice& choice : out) {
        CHECK(std::find(casters.begin(), casters.end(), choice.index) == casters.end());
        if (choice.incoming) {
            cast.push_back(NodeChoice{choice.index, 1.f, true});
        }
    }
    for (const std::size_t index : casters) {
        cast.push_back(NodeChoice{index, 1.f, true});
    }
    CHECK(Coverage(view, cast, 64) == "");
    // With no culling, everything that casts is drawn.
    SelectTerrainNodes(view, Looking(eye, {500.f, 4.f, 0.f}, false), 0.2, state, out);
    SelectTerrainCasters(view, Looking(eye, {500.f, 4.f, 0.f}, false), state, casters);
    CHECK(casters.empty());
}

TEST_CASE("SEL12 the children of nodes close to switching upload ahead, nearest first, 8 a frame",
          "[terrain][lod][render]") {
    const TerrainView view = Slab(64);
    TerrainFadeState state;
    std::vector<NodeChoice> out;
    std::vector<std::size_t> keep, upload;
    const auto none = [](std::size_t) { return false; };
    // The top alone at 0.3 px: not close.
    SelectTerrainNodes(view, kFar, 0.0, state, out);
    SelectTerrainPrefetch(view, kFar, out, state, none, keep, upload);
    CHECK(keep.empty());
    CHECK(upload.empty());
    // Its 4 children at about 0.75 px: their 16 children are candidates; 8 upload, nearest first.
    TerrainFadeState fresh;
    const TerrainCamera aside = Looking({300.f, 10000.f, 300.f}, {1024.f, 0.f, 1024.f});
    SelectTerrainNodes(view, aside, 0.0, fresh, out);
    REQUIRE(out.size() == 4);
    SelectTerrainPrefetch(view, aside, out, fresh, none, keep, upload);
    CHECK(keep.empty());
    REQUIRE(upload.size() == static_cast<std::size_t>(kTerrainPrefetchUploads));
    const Vec3 eye = engine_core::matrix4_position(aside.world);
    const auto distance = [&](std::size_t index) {
        const TerrainNodeView& node = (*view.nodes)[index];
        const float dx = std::max({node.bounds_min.x - eye.x, 0.f, eye.x - node.bounds_max.x});
        const float dy = std::max({node.bounds_min.y - eye.y, 0.f, eye.y - node.bounds_max.y});
        const float dz = std::max({node.bounds_min.z - eye.z, 0.f, eye.z - node.bounds_max.z});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (std::size_t i = 0; i < upload.size(); ++i) {
        CHECK((*view.nodes)[upload[i]].key.level == 4);
        if (i > 0) {
            CHECK(distance(upload[i - 1]) <= distance(upload[i]));
        }
    }
    // The nearest is the grandchild under the camera's corner.
    CHECK((*view.nodes)[upload[0]].key == NodeKey{4, 0, 0, 0});
    // Those uploaded already are kept, and the other 8 upload.
    const std::vector<std::size_t> first = upload;
    const auto held = [&](std::size_t index) { return std::find(first.begin(), first.end(), index) != first.end(); };
    SelectTerrainPrefetch(view, aside, out, fresh, held, keep, upload);
    CHECK(keep.size() == 8);
    CHECK(upload.size() == 8);
    for (const std::size_t index : upload) {
        CHECK(!held(index));
        CHECK(distance(index) >= distance(first.back()));
    }
}

namespace {

// Which chunks of an n x n Slab the camera sees (each chunk's box against
// the camera's frustum, as SelectTerrainNodes culls a node's box).
std::vector<bool> VisibleChunks(int n, const TerrainCamera& camera) {
    const engine_core::Matrix4 viewMatrix =
        engine_core::matrix4_inverse(engine_core::matrix4_orthonormalize(camera.world));
    const engine_core::Matrix4 projection =
        runner::Perspective(camera.fov_y_degrees,
                            static_cast<float>(camera.pane_width) / static_cast<float>(camera.pane_height),
                            runner::kSceneNear, camera.far_z);
    const runner::Frustum frustum = runner::MakeFrustum(engine_core::matrix4_multiply(projection, viewMatrix));
    std::vector<bool> visible(static_cast<std::size_t>(n * n), false);
    for (int x = 0; x < n; ++x) {
        for (int z = 0; z < n; ++z) {
            const Vec3 center{x * 32.f + 16.f, 4.f, z * 32.f + 16.f};
            const Vec3 half{16.f, 4.f, 16.f};
            bool in = true;
            for (int p = 0; p < 6 && in; ++p) {
                const float* plane = frustum.planes[p];
                const float d = plane[0] * center.x + plane[1] * center.y + plane[2] * center.z + plane[3];
                const float r = std::abs(plane[0]) * half.x + std::abs(plane[1]) * half.y + std::abs(plane[2]) * half.z;
                in = d + r >= 0.f;
            }
            visible[static_cast<std::size_t>(x * n + z)] = in;
        }
    }
    return visible;
}

// Like Coverage, over an n x n Slab: every chunk the camera sees drawn
// exactly once at each threshold, and none drawn more than once.
std::string VisibleCoverage(const TerrainView& view, const std::vector<NodeChoice>& choices, int n,
                            const std::vector<bool>& visible) {
    std::vector<int> count(static_cast<std::size_t>(n * n * 16), 0);
    for (const NodeChoice& choice : choices) {
        const NodeKey& key = KeyOf(view, choice);
        const int side = 1 << key.level;
        for (int t = 0; t < 16; ++t) {
            if (!Draws(choice, Threshold(t))) {
                continue;
            }
            for (int x = key.x * side; x < std::min((key.x + 1) * side, n); ++x) {
                for (int z = key.z * side; z < std::min((key.z + 1) * side, n); ++z) {
                    ++count[static_cast<std::size_t>((x * n + z) * 16 + t)];
                }
            }
        }
    }
    for (int x = 0; x < n; ++x) {
        for (int z = 0; z < n; ++z) {
            const bool seen = visible[static_cast<std::size_t>(x * n + z)];
            for (int t = 0; t < 16; ++t) {
                const int c = count[static_cast<std::size_t>((x * n + z) * 16 + t)];
                if (seen ? c != 1 : c > 1) {
                    std::string text = "chunk " + std::to_string(x) + "," + std::to_string(z) +
                                       (seen ? " (seen)" : " (unseen)") + " threshold " +
                                       std::to_string(Threshold(t)) + " drawn " + std::to_string(c) + " times by:";
                    for (const NodeChoice& choice : choices) {
                        const NodeKey& key = KeyOf(view, choice);
                        const int side = 1 << key.level;
                        if (x >= key.x * side && x < (key.x + 1) * side && z >= key.z * side &&
                            z < (key.z + 1) * side) {
                            text += " L" + std::to_string(key.level) + "(" + std::to_string(key.x) + "," +
                                    std::to_string(key.z) + ")" + (choice.incoming ? " in " : " out ") +
                                    std::to_string(choice.fade);
                        }
                    }
                    return text;
                }
            }
        }
    }
    return {};
}

}  // namespace

TEST_CASE("SEL13 zooming and turning at once, culled, draws every pixel in view once each frame",
          "[terrain][lod][render]") {
    const TerrainView view = Slab(32);   // levels 0 to 5
    const auto run = [&](const std::function<TerrainCamera(int)>& cameraAt, int frames, double dt) {
        TerrainFadeState state;
        int fadingFrames = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const TerrainCamera camera = cameraAt(frame);
            const std::vector<NodeChoice> out = Select(view, camera, frame * dt, state);
            const Vec3 eye = engine_core::matrix4_position(camera.world);
            INFO("frame " << frame << " eye " << eye.x << "," << eye.y << "," << eye.z);
            REQUIRE(VisibleCoverage(view, out, 32, VisibleChunks(32, camera)) == "");
            if (std::any_of(out.begin(), out.end(), [](const NodeChoice& choice) { return choice.fade < 1.f; })) {
                ++fadingFrames;
            }
        }
        return fadingFrames;
    };
    // Looking yaw radians around, pitched down by drop (units per unit ahead).
    const auto aimed = [](Vec3 eye, float yaw, float drop) {
        return Looking(eye, {eye.x + std::cos(yaw), eye.y - drop, eye.z + std::sin(yaw)});
    };
    const auto height = [](int frame) {
        const float t = static_cast<float>(frame) / 180.f;
        return 20.f * std::pow(40000.f / 20.f, t);
    };
    // Up from 20 units to 40,000 over 3 s at 60 frames a second, turning a full circle a second.
    CHECK(run([&](int frame) { return aimed({500.f, height(frame), 500.f}, frame * 6.2832f / 60.f, 0.8f); }, 181,
              1.0 / 60.0) > 0);
    // And back down.
    CHECK(run([&](int frame) { return aimed({500.f, height(180 - frame), 500.f}, frame * 6.2832f / 60.f, 0.8f); },
              181, 1.0 / 60.0) > 0);
    // Jumping and turning at random, faster than a fade, at 30 frames a second.
    unsigned seed = 777u;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    std::vector<TerrainCamera> cameras;
    for (int i = 0; i < 400; ++i) {
        const float x = next() * 1024.f;
        const float h = next() * next();
        const float z = next() * 1024.f;
        const float yaw = next() * 6.2832f;
        const float drop = 0.2f + next() * 3.f;
        cameras.push_back(aimed({x, 5.f + h * 20000.f, z}, yaw, drop));
    }
    run([&](int frame) { return cameras[static_cast<std::size_t>(frame)]; }, 400, 1.0 / 30.0);
}

TEST_CASE("SEL14 diving faster than a fade, no node fading out draws over 2 px, every pixel drawn once",
          "[terrain][lod][render]") {
    // Ruling R23. The invariant, each frame: every node drawn fading out has
    // a pixel error of at most kTerrainFadeOutPixelError; every node drawn
    // fading in (or steady) over it is the finest available there (level 0,
    // or a child in its child_mask not published); and every chunk is drawn
    // exactly once at each dither threshold.
    const auto run = [](const TerrainView& view, const char* name) {
        NodeKeySet published;
        for (const TerrainNodeView& node : *view.nodes) {
            published.insert(node.key);
        }
        const auto finestAvailable = [&](const TerrainNodeView& node) {
            if (node.key.level == 0 || node.child_mask == 0) {
                return true;
            }
            const auto children = engine_core::terrain::children_of(node.key);
            for (int i = 0; i < 8; ++i) {
                if ((node.child_mask & (1u << i)) != 0 && published.count(children[static_cast<std::size_t>(i)]) == 0) {
                    return true;
                }
            }
            return false;
        };
        TerrainFadeState state;
        float worstOut = 0.f;
        float worstIn = 0.f;
        int fadingOutDrawn = 0;
        int finestOver = 0;
        // Down from 40,000 units to 5 in one second at 60 frames a second:
        // 16 % closer each frame, a fade (15 frames) spanning a factor of 9.
        constexpr int kFrames = 61;
        for (int frame = 0; frame < kFrames; ++frame) {
            const float t = static_cast<float>(frame) / static_cast<float>(kFrames - 1);
            const Vec3 eye{500.f, 40000.f * std::pow(5.f / 40000.f, t), 500.f};
            const std::vector<NodeChoice> out =
                Select(view, Looking(eye, {eye.x + 1.f, 0.f, eye.z + 1.f}, false), frame / 60.0, state);
            INFO(name << " frame " << frame << " eye height " << eye.y);
            REQUIRE(Coverage(view, out, 64) == "");
            for (const NodeChoice& choice : out) {
                const TerrainNodeView& node = (*view.nodes)[choice.index];
                const float dx = std::max({node.bounds_min.x - eye.x, 0.f, eye.x - node.bounds_max.x});
                const float dy = std::max({node.bounds_min.y - eye.y, 0.f, eye.y - node.bounds_max.y});
                const float dz = std::max({node.bounds_min.z - eye.z, 0.f, eye.z - node.bounds_max.z});
                const float pixels =
                    NodePixelError(node.error, std::sqrt(dx * dx + dy * dy + dz * dz), kFov, kPaneHeight);
                INFO("L" << node.key.level << "(" << node.key.x << "," << node.key.z << ") "
                         << (choice.incoming ? "in " : "out ") << choice.fade << ", " << pixels << " px");
                if (!choice.incoming) {
                    ++fadingOutDrawn;
                    worstOut = std::max(worstOut, pixels);
                    CHECK(pixels <= kTerrainFadeOutPixelError);
                } else if (!finestAvailable(node)) {
                    worstIn = std::max(worstIn, pixels);
                    CHECK(pixels <= kTerrainFadeOutPixelError);
                } else if (pixels > kTerrainFadeOutPixelError) {
                    ++finestOver;
                }
            }
        }
        WARN(name << ": largest pixel error drawn fading out " << worstOut << " (" << fadingOutDrawn
                  << " drawn), fading in or steady but not the finest available " << worstIn << "; "
                  << finestOver << " finest-available draws over " << kTerrainFadeOutPixelError << " px");
        // The dive still cross-fades: R23 cuts only fades far over budget.
        CHECK(fadingOutDrawn > 0);
        return finestOver;
    };
    run(Slab(64), "everything published");
    // Level 0 not built for x below chunk 32, under the camera (residency behind it):
    // level 1 is the finest there, drawn however large its error.
    CHECK(run(Slab(64, [](const NodeKey& key) { return key.level > 0 || key.x >= 32; }), "half without level 0") > 0);
}

TEST_CASE("SEL16 a new root under an ancestor that draws itself appears whole, never fading in",
          "[terrain][lod][render]") {
    // Ruling R27. From far off the top node draws the whole 8 x 8 slab.
    const TerrainView slab = Slab(8);
    const TerrainCamera far = Looking({128.f, 3000.f, 128.f}, {128.f, 0.f, 128.f}, false);
    TerrainFadeState state;
    Select(slab, far, 0.0, state);
    const std::vector<NodeChoice> settled = Select(slab, far, 1.0, state);
    REQUIRE(settled.size() == 1u);
    REQUIRE(KeyOf(slab, settled[0]).level == 3);

    // An edit made surface at chunk (0, 2, 0): its level-1 parent is not
    // built, so the level-2 node over it leaves it out, and it is a root.
    TerrainView grown = slab;
    auto nodes = std::make_shared<std::vector<TerrainNodeView>>(*slab.nodes);
    TerrainNodeView fresh;
    fresh.key = NodeKey{0, 0, 2, 0};
    fresh.revision = 5000;
    engine_core::terrain::node_bounds(fresh.key, 1.f, fresh.bounds_min, fresh.bounds_max);
    nodes->insert(nodes->begin(), fresh);
    grown.nodes = nodes;
    grown.nodes_revision = 2;
    for (const double now : {1.0 + 1.0 / 60.0, 1.05, 1.1, 1.2, 1.4}) {
        INFO("at " << now);
        const std::vector<NodeChoice> choices = Select(grown, far, now, state);
        REQUIRE(choices.size() == 2u);
        for (const NodeChoice& choice : choices) {
            INFO("L" << KeyOf(grown, choice).level);
            CHECK(choice.incoming);
            CHECK(choice.fade == 1.f);
        }
    }
}

TEST_CASE("SEL17 a stale node with every child published is descended, whole, and drawn again whole once rebuilt",
          "[terrain][lod][render]") {
    // Ruling R26. From far off the top node of an 8 x 8 slab draws.
    const TerrainView slab = Slab(8);
    const TerrainCamera far = Looking({128.f, 3000.f, 128.f}, {128.f, 0.f, 128.f}, false);
    TerrainFadeState state;
    Select(slab, far, 0.0, state);
    REQUIRE(Select(slab, far, 1.0, state).size() == 1u);
    // A view of slab with the nodes over chunk (0, 0, 0) at levels from..3 stale.
    const auto staleFrom = [&](int from, std::uint64_t revision) {
        TerrainView view = slab;
        auto nodes = std::make_shared<std::vector<TerrainNodeView>>(*slab.nodes);
        for (TerrainNodeView& node : *nodes) {
            node.stale = node.key.level >= from && node.key.x == 0 && node.key.y == 0 && node.key.z == 0;
        }
        view.nodes = nodes;
        view.nodes_revision = revision;
        return view;
    };
    const auto check = [&](const TerrainView& view, double now, int finest, std::size_t count) {
        INFO("at " << now);
        const std::vector<NodeChoice> choices = Select(view, far, now, state);
        CHECK(Coverage(view, choices, 8) == "");
        CHECK(choices.size() == count);
        for (const NodeChoice& choice : choices) {
            const NodeKey& key = KeyOf(view, choice);
            INFO("L" << key.level << " (" << key.x << ", " << key.y << ", " << key.z << ")");
            CHECK(choice.incoming);
            CHECK(choice.fade == 1.f);
            CHECK_FALSE((*view.nodes)[choice.index].stale);
            if (key.x == 0 && key.z == 0) CHECK(key.level == finest);
        }
    };
    // The edit queued chunk (0, 0, 0): levels 1 to 3 over it go stale. Selection
    // goes down to it through them, below the pixel budget, with no fade.
    const TerrainView edited = staleFrom(1, 2);
    for (const double now : {1.0 + 1.0 / 60.0, 1.1, 1.3}) check(edited, now, 0, 3u + 3u + 4u);   // 3 L2s, 3 L1s, 4 chunks
    // Level 1 rebuilt: drawn there, whole.
    const TerrainView level1 = staleFrom(2, 3);
    for (const double now : {1.4, 1.5, 1.7}) check(level1, now, 1, 3u + 4u);   // 3 L2s, 4 L1s
    // Every level rebuilt: the top draws again, whole.
    TerrainView rebuilt = slab;
    rebuilt.nodes_revision = 4;
    for (const double now : {1.8, 1.9, 2.1}) check(rebuilt, now, 3, 1u);
}
