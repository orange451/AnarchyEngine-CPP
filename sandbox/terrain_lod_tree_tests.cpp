// Task 5 of the terrain LOD plan: LodTree (one Terrain's octree bookkeeping)
// and its TerrainWorld integration -- every level built, edits rebuilt with
// a 100 ms debounce, levels 0-1 resident only near the camera. Bookkeeping
// tests drive LodTree directly (with fake or synchronously built meshes and
// their own clock); whole-loop tests drive TerrainWorld with wait_idle.

#include "support.hpp"

#include "Camera.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainWorld.hpp"
#include "amesh.hpp"
#include "terrain/LodBuilder.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/LodTree.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace engine_core;
using namespace engine_core::terrain;

namespace {

// A flat-bottomed island over chunks [0, n) x {0} x [0, n): a gentle rolling
// top, its walls and floor kept 4 cells inside the outer chunks so every
// surface quad belongs to a chunk of the island itself (no -1 or n chunk).
std::optional<std::string> fill_island(VoxelVolume& volume, int n) {
    const int x1 = kChunkSize * n - 1, z1 = x1, y1 = kChunkSize - 1;
    const int lo = 4, hi = kChunkSize * n - 5;
    const int width = x1 + 1, height = y1 + 1, depth = z1 + 1;
    std::vector<float> distances(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                 static_cast<std::size_t>(depth));
    std::vector<std::uint8_t> materials(distances.size(), 1);
    for (int z = 0; z <= z1; ++z) {
        for (int x = 0; x <= x1; ++x) {
            const float surface = 16.f + 2.f * std::sin(static_cast<float>(x) / 40.f) * std::cos(static_cast<float>(z) / 50.f);
            for (int y = 0; y <= y1; ++y) {
                const float d = std::max({static_cast<float>(y) - surface, static_cast<float>(lo - y),
                                          static_cast<float>(lo - x), static_cast<float>(x - hi),
                                          static_cast<float>(lo - z), static_cast<float>(z - hi)});
                distances[static_cast<std::size_t>(x + width * (y + height * z))] = d;
            }
        }
    }
    return volume.write(CellCoord{0, 0, 0}, CellCoord{x1, y1, z1}, distances, materials);
}

// One triangle inside min..max.
std::shared_ptr<const anarchy::amesh::Data> fake_mesh(Vec3 min, Vec3 max) {
    anarchy::amesh::Data mesh;
    mesh.vertices.resize(3);
    const Vec3 c{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    const float positions[3][3] = {{c.x, c.y, c.z}, {c.x + 1.f, c.y, c.z}, {c.x, c.y, c.z + 1.f}};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t a = 0; a < 3; ++a) mesh.vertices[i].p[a] = positions[i][a];
        mesh.vertices[i].n[1] = 1.f;
    }
    mesh.indices = {0, 2, 1};
    anarchy::amesh::compute_aabb(mesh);
    return std::make_shared<const anarchy::amesh::Data>(std::move(mesh));
}

std::shared_ptr<const anarchy::amesh::Data> fake_chunk_mesh(ChunkCoord coord) {
    Vec3 min, max;
    node_bounds(NodeKey{0, coord.x, coord.y, coord.z}, 1.f, min, max);
    return fake_mesh(min, max);
}

LodResult fake_build(const LodInput& input) {
    LodResult result;
    result.key = input.key;
    if (input.children.empty()) {
        return result;
    }
    Vec3 min, max;
    node_bounds(input.key, input.voxel_size, min, max);
    result.mesh = fake_mesh(min, max);
    result.error = target_error(input.key.level, input.voxel_size);
    result.surface_index_count = 3;
    return result;
}

// Drives a LodTree the way TerrainWorld does, synchronously: chunk jobs and
// node builds finish the moment they are asked for.
struct TreeRig {
    using MeshChunk = std::function<std::shared_ptr<const anarchy::amesh::Data>(ChunkCoord)>;
    using BuildNode = std::function<LodResult(const LodInput&)>;

    LodTree tree;
    MeshChunk mesh_chunk;
    BuildNode build;
    double now = 0.0;
    const ChunkCoord* camera = nullptr;
    ChunkCoord camera_at{};
    std::unordered_map<NodeKey, int, NodeKeyHash> builds;   // accepted builds per node
    int chunk_jobs = 0;                                     // residency re-meshes asked for
    int drops = 0;

    explicit TreeRig(float voxel_size = 1.f, MeshChunk mesher = fake_chunk_mesh, BuildNode builder = fake_build)
        : tree(voxel_size), mesh_chunk(std::move(mesher)), build(std::move(builder)) {}

    void look_from(ChunkCoord at) {
        camera_at = at;
        camera = &camera_at;
    }

    // An edit (or first sight) of these chunks: queued, then landed.
    void edit(const std::vector<ChunkCoord>& chunks) {
        for (const ChunkCoord& c : chunks) tree.chunk_queued(c, true);
        for (const ChunkCoord& c : chunks) tree.chunk_meshed(c, mesh_chunk(c), true);
    }

    // One TerrainWorld::update's worth; true if it asked for any work.
    bool step() {
        std::vector<ChunkCoord> dropped, needed;
        tree.update_residency(camera, dropped, needed);
        drops += static_cast<int>(dropped.size());
        chunk_jobs += static_cast<int>(needed.size());
        for (const ChunkCoord& c : needed) tree.chunk_queued(c, false);
        for (const ChunkCoord& c : needed) tree.chunk_meshed(c, mesh_chunk(c), false);
        std::vector<NodeBuildRequest> requests;
        tree.next_builds(now, camera, requests);
        for (NodeBuildRequest& request : requests) {
            NodeResult result;
            result.key = request.input.key;
            result.revision = request.revision;
            result.result = build(request.input);
            tree.node_built(result);
            ++builds[request.input.key];
        }
        return !dropped.empty() || !needed.empty() || !requests.empty();
    }

    // Steps until nothing more is asked for, advancing the clock past the
    // debounce window each step.
    void settle() {
        // Two idle steps in a row: the first may only be a node waiting out
        // its debounce window, which the clock has passed by the second.
        int idle = 0;
        for (int i = 0; i < 64; ++i) {
            const bool worked = step();
            now += 2.0 * kRebuildIntervalMs;
            idle = worked ? 0 : idle + 1;
            if (idle == 2) return;
        }
        FAIL("the tree never settled");
    }
};

std::vector<ChunkCoord> island_chunks(int n) {
    std::vector<ChunkCoord> out;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) out.push_back(ChunkCoord{x, 0, z});
    return out;
}

std::vector<NodeKey> ancestors_of(ChunkCoord coord, int top) {
    std::vector<NodeKey> out;
    NodeKey key{0, coord.x, coord.y, coord.z};
    for (int level = 1; level <= top; ++level) {
        key = parent_of(key);
        out.push_back(key);
    }
    return out;
}

int count_level(const std::vector<TerrainNodeView>& nodes, int level) {
    return static_cast<int>(
        std::count_if(nodes.begin(), nodes.end(), [&](const TerrainNodeView& v) { return v.key.level == level; }));
}

const TerrainNodeView* find_view(const std::vector<TerrainNodeView>& nodes, const NodeKey& key) {
    for (const TerrainNodeView& v : nodes) {
        if (v.key == key) return &v;
    }
    return nullptr;
}

Terrain& island_terrain(DataModel& game, int n) {
    auto& t = game.create<Terrain>();
    game.set_parent(t.id(), workspace_of(game));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return fill_island(v, n); }));
    return t;
}

// Updates TerrainWorld until its workers are idle and nothing new was
// queued, with a clock that passes the debounce window every update.
void settle_lod(TerrainWorld& world, DataModel& game, double& now) {
    for (int i = 0; i < 16; ++i) {
        world.update(game, now);
        world.wait_idle();
        now += 2.0 * kRebuildIntervalMs;
    }
    world.update(game, now);
}

Camera& camera_at(DataModel& game, Vec3 at) {
    Camera& camera = game.create<Camera>();
    camera.set_transform(matrix4_translation(at.x, at.y, at.z));
    game.set_parent(camera.id(), workspace_of(game));
    auto* workspace = dynamic_cast<Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->set_current_camera(camera.id()));
    return camera;
}

}  // namespace

TEST_CASE("LT1 a settled 8x1x8-chunk island has nodes at every level up to the top and one top node",
          "[terrain][lod]") {
    SimRole role;
    Game game;
    Terrain& t = island_terrain(game, 8);
    TerrainWorld world;
    double now = 0.0;
    settle_lod(world, game, now);

    REQUIRE(world.views().size() == 1u);
    const TerrainView& view = world.views()[0];
    REQUIRE(view.nodes != nullptr);
    const std::vector<TerrainNodeView>& nodes = *view.nodes;
    REQUIRE(view.top_level == 3);
    INFO("nodes per level: " << count_level(nodes, 0) << ", " << count_level(nodes, 1) << ", "
                             << count_level(nodes, 2) << ", " << count_level(nodes, 3));
    REQUIRE(count_level(nodes, 0) == 64);
    REQUIRE(count_level(nodes, 1) == 16);
    REQUIRE(count_level(nodes, 2) == 4);
    REQUIRE(count_level(nodes, 3) == 1);
    REQUIRE(count_level(nodes, 4) == 0);
    REQUIRE(find_view(nodes, NodeKey{3, 0, 0, 0}) != nullptr);
    for (const TerrainNodeView& node : nodes) {
        // R12: level 0 publishes its chunk mesh, levels >= 1 only their compact mesh.
        if (node.key.level == 0) {
            REQUIRE(node.mesh != nullptr);
            REQUIRE(node.compact == nullptr);
            REQUIRE_FALSE(node.mesh->indices.empty());
        } else {
            REQUIRE(node.mesh == nullptr);
            REQUIRE(node.compact != nullptr);
            REQUIRE_FALSE(unpack(*node.compact).indices.empty());
        }
        Vec3 min, max;
        node_bounds(node.key, t.volume().voxel_size(), min, max);
        REQUIRE((node.bounds_min.x <= min.x && node.bounds_min.y <= min.y && node.bounds_min.z <= min.z));
        REQUIRE((node.bounds_max.x >= max.x && node.bounds_max.y >= max.y && node.bounds_max.z >= max.z));
        if (node.key.level == 0) {
            REQUIRE(node.error == 0.f);
            REQUIRE(node.child_mask == 0);
        } else {
            // Every level-L node of the island has its 4 (y = 0) children.
            int bits = 0;
            for (int i = 0; i < 8; ++i) bits += (node.child_mask >> i) & 1;
            REQUIRE(bits == 4);
            REQUIRE(node.error >= 0.f);
        }
    }
    // The renderer's chunk list still shows every chunk (no camera: all resident).
    REQUIRE(view.chunks->size() == 64u);
}

TEST_CASE("LT2 levels >= 2 stay resident far from the camera; levels 0-1 drop and come back", "[terrain][lod]") {
    SimRole role;
    Game game;
    Terrain& t = island_terrain(game, 4);   // top level 2
    const float span = kChunkSize * t.volume().voxel_size();
    Camera& camera = camera_at(game, Vec3{2.5f * span, 16.f, 2.5f * span});
    // A stand-in collider for every chunk with triangles, as PhysicsWorld's would be.
    TerrainWorld world([](const ChunkMesh& mesh) -> std::shared_ptr<void> {
        return mesh.triangles.empty() ? nullptr : std::make_shared<int>(1);
    });
    double now = 0.0;
    const auto collider_revisions = [&] {
        std::unordered_map<ChunkCoord, std::uint64_t, ChunkCoordHash> out;
        for (const TerrainWorld::ChunkCollider& c : *world.colliders(t.id())) out[c.coord] = c.revision;
        return out;
    };
    settle_lod(world, game, now);

    const auto at_home = world.views()[0].nodes;
    REQUIRE(world.views()[0].top_level == 2);
    REQUIRE(count_level(*at_home, 0) == 16);
    REQUIRE(count_level(*at_home, 1) == 4);
    REQUIRE(count_level(*at_home, 2) == 1);
    const TerrainNodeView* top = find_view(*at_home, NodeKey{2, 0, 0, 0});
    REQUIRE(top != nullptr);
    const std::uint64_t top_revision = top->revision;
    REQUIRE(world.colliders(t.id()) != nullptr);
    const std::size_t colliders = world.colliders(t.id())->size();
    REQUIRE(colliders == 16u);
    const auto home_colliders = collider_revisions();

    // 50 chunks away.
    camera.set_transform(matrix4_translation(52.5f * span, 16.f, 2.5f * span));
    settle_lod(world, game, now);
    const auto away = world.views()[0].nodes;
    REQUIRE(count_level(*away, 0) == 0);
    REQUIRE(count_level(*away, 1) == 0);
    REQUIRE(count_level(*away, 2) == 1);
    REQUIRE(find_view(*away, NodeKey{2, 0, 0, 0})->revision == top_revision);   // kept, not rebuilt
    REQUIRE(world.views()[0].chunks->empty());
    REQUIRE(world.colliders(t.id())->size() == colliders);   // R5: residency never drops colliders

    // Back home: levels 0-1 return; the top is still the same build.
    camera.set_transform(matrix4_translation(2.5f * span, 16.f, 2.5f * span));
    settle_lod(world, game, now);
    const auto back = world.views()[0].nodes;
    REQUIRE(count_level(*back, 0) == 16);
    REQUIRE(count_level(*back, 1) == 4);
    REQUIRE(count_level(*back, 2) == 1);
    REQUIRE(find_view(*back, NodeKey{2, 0, 0, 0})->revision == top_revision);
    REQUIRE(world.views()[0].chunks->size() == 16u);
    // The residency re-meshes left every collider as it was: no physics churn.
    REQUIRE(collider_revisions() == home_colliders);
}

TEST_CASE("LT3 an edit marks every ancestor stale and rebuilds each exactly once", "[terrain][lod]") {
    TreeRig rig;
    rig.edit(island_chunks(8));
    rig.settle();
    const int top = rig.tree.top_level();
    REQUIRE(top == 3);
    for (const auto& [key, node] : rig.tree.nodes()) {
        INFO("level " << key.level << " (" << key.x << ", " << key.y << ", " << key.z << ")");
        REQUIRE_FALSE((key.level > 0 && node.stale()));
    }

    const ChunkCoord edited{3, 0, 5};
    const std::vector<NodeKey> ancestors = ancestors_of(edited, top);
    std::unordered_map<NodeKey, std::uint64_t, NodeKeyHash> before;
    for (const auto& [key, node] : rig.tree.nodes()) before[key] = node.mesh_revision;
    rig.builds.clear();

    rig.tree.chunk_queued(edited, true);
    for (const NodeKey& key : ancestors) {
        REQUIRE(rig.tree.find(key) != nullptr);
        REQUIRE(rig.tree.find(key)->stale());
    }
    rig.tree.chunk_meshed(edited, fake_chunk_mesh(edited), true);
    rig.settle();

    for (const auto& [key, node] : rig.tree.nodes()) {
        INFO("level " << key.level << " (" << key.x << ", " << key.y << ", " << key.z << ")");
        const bool ancestor = std::find(ancestors.begin(), ancestors.end(), key) != ancestors.end();
        if (key.level == 0) continue;
        REQUIRE_FALSE(node.stale());
        REQUIRE(rig.builds[key] == (ancestor ? 1 : 0));
        REQUIRE((node.mesh_revision != before[key]) == ancestor);
    }
}

TEST_CASE("LT4 50 edits to one chunk within 100 ms rebuild each ancestor at most twice", "[terrain][lod]") {
    TreeRig rig;
    rig.edit(island_chunks(8));
    rig.settle();
    rig.builds.clear();
    const ChunkCoord edited{6, 0, 1};
    const std::vector<NodeKey> ancestors = ancestors_of(edited, rig.tree.top_level());

    const double start = rig.now;
    for (int i = 0; i < 50; ++i) {
        rig.now = start + i * 2.0;   // 50 edits over 98 ms
        rig.edit({edited});
        while (rig.step()) {
        }   // every finished build lands at once, as fast as a worker could
    }
    rig.settle();

    for (const NodeKey& key : ancestors) {
        INFO("level " << key.level << " builds " << rig.builds[key]);
        REQUIRE(rig.builds[key] >= 1);
        REQUIRE(rig.builds[key] <= 2);
        REQUIRE_FALSE(rig.tree.find(key)->stale());
    }
}

TEST_CASE("LT5 a camera oscillating across kNearChunks re-meshes nothing after the first settle", "[terrain][lod]") {
    TreeRig rig;
    rig.look_from(ChunkCoord{4, 0, 4});
    rig.edit(island_chunks(32));
    rig.settle();
    REQUIRE(rig.drops > 0);   // residency is in play: chunks far from the camera went

    // Edge of the near ring: chunk x = 4 + kNearChunks is near at A, not at B.
    const ChunkCoord a{4, 0, 4}, b{3, 0, 4};
    rig.look_from(b);
    rig.settle();
    rig.look_from(a);
    rig.settle();
    rig.builds.clear();
    rig.chunk_jobs = 0;
    rig.drops = 0;
    for (int i = 0; i < 10; ++i) {
        rig.look_from(i % 2 == 0 ? b : a);
        rig.settle();
    }
    REQUIRE(rig.chunk_jobs == 0);
    REQUIRE(rig.drops == 0);
    REQUIRE(rig.builds.empty());

    // A control: a real move does re-mesh.
    rig.look_from(ChunkCoord{20, 0, 20});
    rig.settle();
    REQUIRE(rig.chunk_jobs > 0);
}

TEST_CASE("LT6 levels >= 2 of a built island stay within the compact RAM budget", "[terrain][lod]") {
    // The whole loop, as LT1 builds it; then the tree TerrainWorld keeps.
    SimRole role;
    Game game;
    Terrain& t = island_terrain(game, 8);
    TerrainWorld world;
    double now = 0.0;
    settle_lod(world, game, now);
    const LodTree* tree = world.lod_tree(t.id());
    REQUIRE(tree != nullptr);
    REQUIRE(world.views()[0].top_level == 3);
    std::size_t level0_triangles = 0;
    for (const TerrainChunkView& chunk : *world.views()[0].chunks) level0_triangles += chunk.mesh->indices.size() / 3;

    std::size_t bytes = 0, vertices = 0, triangles = 0, budget = 0, nodes = 0;
    for (const auto& [key, node] : tree->nodes()) {
        if (key.level < 2) continue;
        REQUIRE(node.resident);
        REQUIRE(node.compact != nullptr);
        const CompactMesh& mesh = *node.compact;
        const std::size_t v = mesh.positions.size() / 3;
        const std::size_t tris = (mesh.indices.size() + mesh.indices32.size()) / 3;
        // bytes() is what the vectors really hold, not a formula of its own.
        const std::size_t held = mesh.positions.size() * sizeof(std::uint16_t) + mesh.normals.size() +
                                 mesh.ids.size() + mesh.weights.size() +
                                 mesh.indices.size() * sizeof(std::uint16_t) +
                                 mesh.indices32.size() * sizeof(std::uint32_t);
        REQUIRE(mesh.bytes() == held);
        // The per-vertex/per-triangle layout the budget assumes (R1).
        REQUIRE(mesh.normals.size() == 2 * v);
        REQUIRE(mesh.ids.size() == 4 * v);
        REQUIRE(mesh.weights.size() == 4 * v);
        REQUIRE((mesh.indices.empty() || mesh.indices32.empty()));
        bytes += held;
        vertices += v;
        triangles += tris;
        ++nodes;
    }
    budget = 16 * vertices;   // R1, from the counts alone
    for (const auto& [key, node] : tree->nodes()) {
        if (key.level < 2) continue;
        const std::size_t tris = (node.compact->indices.size() + node.compact->indices32.size()) / 3;
        budget += (node.compact->indices32.empty() ? 6 : 12) * tris;
    }
    INFO("levels >= 2: " << nodes << " nodes, " << vertices << " vertices, " << triangles << " triangles, " << bytes
                         << " bytes (budget " << budget << "); level 0: " << level0_triangles << " triangles");
    WARN("LT6 levels >= 2: " << nodes << " nodes, " << vertices << " vertices, " << triangles << " triangles, "
                             << bytes << " bytes (budget " << budget << "); level 0: " << level0_triangles
                             << " triangles");
    REQUIRE(nodes == 5u);
    REQUIRE(bytes > 0u);
    REQUIRE(bytes <= budget);

    // R12: RAM each resident node >= 1 costs. Published views share the
    // tree's compact mesh and hold nothing unpacked; before R12 each also
    // kept its unpacked amesh::Data cached for as long as it was published.
    const std::vector<TerrainNodeView>& published = *world.views()[0].nodes;
    std::size_t resident = 0, compact_bytes = 0, unpacked_bytes = 0;
    for (const auto& [key, node] : tree->nodes()) {
        if (key.level < 1 || !node.resident) continue;
        const TerrainNodeView* view = find_view(published, key);
        REQUIRE(view != nullptr);
        REQUIRE(view->mesh == nullptr);
        REQUIRE(view->compact.get() == node.compact.get());   // shared, not copied
        const anarchy::amesh::Data unpacked = unpack(*node.compact);
        compact_bytes += node.compact->bytes();
        unpacked_bytes += unpacked.vertices.size() * sizeof(anarchy::amesh::Vertex) +
                          unpacked.indices.size() * sizeof(std::uint32_t);
        ++resident;
    }
    REQUIRE(resident == 21u);
    WARN("LT6 RAM, " << resident << " resident nodes >= 1: before R12 " << (compact_bytes + unpacked_bytes) / resident
                     << " B/node (compact + unpacked cache), after " << compact_bytes / resident
                     << " B/node (compact only); totals " << compact_bytes + unpacked_bytes << " -> " << compact_bytes);
    // The spec's estimate: levels 2 and up are a small fraction of full detail.
    REQUIRE(triangles * 4 <= level0_triangles);
}

TEST_CASE("LT7 a node build that fails is offered again and the node and its ancestors settle", "[terrain][lod]") {
    SimRole role;
    Game game;
    Terrain& t = island_terrain(game, 4);   // top level 2
    const NodeKey broken{1, 1, 0, 1};
    std::atomic<int> calls{0};
    TerrainWorld world({}, 0, [&](const LodInput& input) -> LodResult {
        if (input.key == broken && calls.fetch_add(1) < 2) {
            throw std::runtime_error("LT7: a deliberately broken node build");
        }
        return build_node(input);
    });
    double now = 0.0;
    settle_lod(world, game, now);

    REQUIRE(world.mesh_failures() == 2u);
    REQUIRE(calls.load() == 3);   // two failures, then one build that took
    const LodTree* tree = world.lod_tree(t.id());
    REQUIRE(tree != nullptr);
    for (const NodeKey& key : {broken, NodeKey{2, 0, 0, 0}}) {
        INFO("level " << key.level);
        const LodTree::Node* node = tree->find(key);
        REQUIRE(node != nullptr);
        REQUIRE(node->built);
        REQUIRE_FALSE(node->stale());
        REQUIRE(find_view(*world.views()[0].nodes, key) != nullptr);
    }
}

TEST_CASE("LT8 a chunk job that fails keeps its old mesh and does not block its ancestors", "[terrain][lod]") {
    SimRole role;
    Game game;
    Terrain& t = island_terrain(game, 4);
    const float span = kChunkSize * t.volume().voxel_size();
    std::atomic<bool> armed{false};
    // Throws, once armed, for chunk (1, 0, 1)'s mesh only (found by its AABB's center).
    TerrainWorld world([&](const ChunkMesh& mesh) -> std::shared_ptr<void> {
        if (mesh.render == nullptr || mesh.render->indices.empty()) return nullptr;
        const float cx = (mesh.render->bbox_min[0] + mesh.render->bbox_max[0]) * 0.5f;
        const float cz = (mesh.render->bbox_min[2] + mesh.render->bbox_max[2]) * 0.5f;
        if (armed.load() && std::floor(cx / span) == 1.f && std::floor(cz / span) == 1.f) {
            throw std::runtime_error("LT8: a deliberately broken chunk build");
        }
        return std::make_shared<int>(1);
    });
    double now = 0.0;
    settle_lod(world, game, now);
    const ChunkCoord edited{1, 0, 1};
    const auto chunk_revision = [&]() -> std::uint64_t {
        for (const TerrainChunkView& chunk : *world.views()[0].chunks) {
            if (chunk.coord == edited) return chunk.revision;
        }
        return 0;
    };
    const std::uint64_t before = chunk_revision();
    REQUIRE(before != 0u);
    REQUIRE(world.mesh_failures() == 0u);

    // A bump on the surface inside the chunk; its job fails.
    armed = true;
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) {
        const std::vector<float> distances(64, -1.f);
        const std::vector<std::uint8_t> materials(64, 1);
        return v.write(CellCoord{44, 17, 44}, CellCoord{47, 20, 47}, distances, materials);
    }));
    settle_lod(world, game, now);

    REQUIRE(world.mesh_failures() >= 1u);
    REQUIRE(chunk_revision() == before);   // the old mesh stays
    REQUIRE(world.views()[0].chunks->size() == 16u);
    const LodTree* tree = world.lod_tree(t.id());
    REQUIRE(tree != nullptr);
    REQUIRE_FALSE(tree->find(NodeKey{0, edited.x, edited.y, edited.z})->in_flight);
    for (const NodeKey& key : ancestors_of(edited, world.views()[0].top_level)) {
        INFO("level " << key.level);
        const LodTree::Node* node = tree->find(key);
        REQUIRE(node != nullptr);
        REQUIRE(node->built);
        REQUIRE_FALSE(node->stale());
    }
}

TEST_CASE("LT9 a node whose build has no surface is removed, with ancestors it leaves childless", "[terrain][lod]") {
    // Level-1 builds under (2, 0, 0, 0), and (1, 3, 0, 3), come back empty.
    const auto empty = [](const NodeKey& key) {
        return key.level == 1 && ((key.x < 2 && key.z < 2) || (key.x == 3 && key.z == 3));
    };
    TreeRig rig(1.f, fake_chunk_mesh, [&](const LodInput& input) {
        if (empty(input.key)) {
            LodResult result;
            result.key = input.key;
            return result;
        }
        return fake_build(input);
    });
    rig.edit(island_chunks(8));
    rig.settle();
    REQUIRE(rig.tree.top_level() == 3);

    for (const auto& [key, node] : rig.tree.nodes()) {
        INFO("level " << key.level << " (" << key.x << ", " << key.y << ", " << key.z << ")");
        REQUIRE_FALSE(empty(key));
        if (key.level > 0) {
            // Only nodes with surface exist.
            REQUIRE(node.built);
            REQUIRE(node.has_surface);
            REQUIRE_FALSE(node.stale());
        }
    }
    REQUIRE(rig.tree.find(NodeKey{2, 0, 0, 0}) == nullptr);   // all four children went: so did it
    const std::vector<TerrainNodeView> views = rig.tree.nodes_for_view();
    const auto bits = [](std::uint8_t mask) {
        int n = 0;
        for (int i = 0; i < 8; ++i) n += (mask >> i) & 1;
        return n;
    };
    const TerrainNodeView* kept = find_view(views, NodeKey{2, 1, 0, 1});
    REQUIRE(kept != nullptr);
    REQUIRE(bits(kept->child_mask) == 3);
    const TerrainNodeView* top = find_view(views, NodeKey{3, 0, 0, 0});
    REQUIRE(top != nullptr);
    REQUIRE(bits(top->child_mask) == 3);
}

namespace {

// Settles a tree over chunks and checks its roots: every node at
// top_level, each chunk under exactly one of them, nothing above.
void check_roots(const std::vector<ChunkCoord>& chunks, int expected_top, std::size_t expected_roots) {
    TreeRig rig;
    rig.edit(chunks);
    rig.settle();
    const int top = rig.tree.top_level();
    REQUIRE(top == expected_top);
    std::vector<NodeKey> roots;
    for (const auto& [key, node] : rig.tree.nodes()) {
        REQUIRE(key.level <= top);
        if (key.level == top) {
            roots.push_back(key);
            if (top > 0) {
                REQUIRE(node.built);
                REQUIRE_FALSE(node.stale());
            }
        }
    }
    REQUIRE(roots.size() == expected_roots);
    for (const ChunkCoord& chunk : chunks) {
        INFO("chunk (" << chunk.x << ", " << chunk.y << ", " << chunk.z << ")");
        const NodeKey root = node_of(chunk, top);
        const auto covering = std::count_if(roots.begin(), roots.end(), [&](const NodeKey& r) { return r == root; });
        REQUIRE(covering == 1);
        // And the tree links the chunk up to that root.
        NodeKey key{0, chunk.x, chunk.y, chunk.z};
        for (int level = 1; level <= top; ++level) {
            key = parent_of(key);
            REQUIRE(rig.tree.find(key) != nullptr);
        }
        REQUIRE((key == root));
    }
    const std::vector<TerrainNodeView> views = rig.tree.nodes_for_view();
    REQUIRE(static_cast<std::size_t>(count_level(views, top)) == expected_roots);
}

std::vector<ChunkCoord> box_chunks(ChunkCoord lo, ChunkCoord hi) {
    std::vector<ChunkCoord> out;
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x) out.push_back(ChunkCoord{x, y, z});
    return out;
}

}  // namespace

TEST_CASE("LT10 a Terrain straddling the origin has one root per side, each chunk under exactly one",
          "[terrain][lod]") {
    // x straddles at level 2 ((-1, 0)), y at level 0, z collapses to one node at level 1.
    check_roots(box_chunks(ChunkCoord{-3, -1, 0}, ChunkCoord{2, 0, 1}), 2, 4u);
    // Every axis straddles: 8 roots at level 1.
    check_roots(box_chunks(ChunkCoord{-2, -2, -2}, ChunkCoord{1, 1, 1}), 1, 8u);
    // No straddle: one root (the control).
    check_roots(box_chunks(ChunkCoord{0, 0, 0}, ChunkCoord{3, 0, 3}), 2, 1u);
}
