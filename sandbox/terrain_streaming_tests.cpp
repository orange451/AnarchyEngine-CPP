// Terrain streaming: the decoded-chunk cache, first-build admission, and the
// .alod far-mesh cache. No instances unless a test says otherwise.

#include "support.hpp"
#include "Camera.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainWorld.hpp"
#include "terrain/AlodStore.hpp"
#include "terrain/AvoxFile.hpp"
#include "terrain/ChunkCache.hpp"
#include "terrain/LodTree.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelSampler.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace engine_core;
using namespace engine_core::terrain;

namespace {
constexpr std::size_t kChunkBytes = static_cast<std::size_t>(kChunkCells) * sizeof(Cell);

Shape ball_at(float x, float y, float z, float radius) {
    Shape shape;
    shape.kind = Shape::Kind::Ball;
    shape.center = Vec3{x, y, z};
    shape.radius = radius;
    return shape;
}

// A dense, shared chunk whose cell 0 reads (d, m), cell 1 (-5, m), the rest air.
ChunkPtr dense_chunk(std::int8_t d, std::uint8_t m) {
    std::shared_ptr<ChunkData> chunk = ChunkData::air()->clone_dense();
    chunk->set(0, Cell{d, m});
    chunk->set(1, Cell{-5, m});
    chunk->finish();
    return chunk;
}

// Empties the global cache and sets its budget; puts both back after.
struct BudgetGuard {
    std::size_t saved = ChunkCache::global().budget();
    explicit BudgetGuard(std::size_t bytes) {
        ChunkCache::global().clear();
        ChunkCache::global().set_budget(bytes);
    }
    ~BudgetGuard() {
        ChunkCache::global().clear();
        ChunkCache::global().set_budget(saved);
    }
};
}  // namespace

TEST_CASE("CC1 a released chunk reads the same cells after eviction", "[terrain]") {
    BudgetGuard guard(kChunkBytes);   // room for one
    ChunkPtr a = dense_chunk(-20, 7);
    ChunkPtr b = dense_chunk(-30, 9);
    a->release_cells();
    b->release_cells();   // evicts a's cells
    REQUIRE_FALSE(a->cells_owned());
    REQUIRE(ChunkCache::global().bytes() <= kChunkBytes);
    REQUIRE(a->cell(0) == Cell{-20, 7});   // decoded again from its frame
    REQUIRE(a->cell(1) == Cell{-5, 7});
    REQUIRE(b->cell(0) == Cell{-30, 9});
}

TEST_CASE("CC2 the cache never holds more than its budget", "[terrain]") {
    BudgetGuard guard(3 * kChunkBytes);
    std::vector<ChunkPtr> chunks;
    for (int i = 0; i < 20; ++i) {
        chunks.push_back(dense_chunk(static_cast<std::int8_t>(-i - 1), 1));
        chunks.back()->release_cells();
        REQUIRE(ChunkCache::global().bytes() <= 3 * kChunkBytes);
    }
    for (int i = 0; i < 20; ++i) {
        REQUIRE(chunks[static_cast<std::size_t>(i)]->cell(0).distance == -i - 1);
        REQUIRE(ChunkCache::global().bytes() <= 3 * kChunkBytes);
    }
}

TEST_CASE("CC3 an edited chunk keeps its cells until released", "[terrain]") {
    BudgetGuard guard(0);   // the cache keeps nothing
    ChunkPtr edited = dense_chunk(-12, 3);
    REQUIRE(edited->cells_owned());
    REQUIRE(edited->cell(0) == Cell{-12, 3});
    edited->release_cells();
    REQUIRE_FALSE(edited->cells_owned());
    REQUIRE(edited->cell(0) == Cell{-12, 3});
}

TEST_CASE("CC4 a pin outlives eviction", "[terrain]") {
    BudgetGuard guard(kChunkBytes);
    ChunkPtr a = dense_chunk(-20, 7);
    a->release_cells();
    CellsPtr pinned = a->cells();
    for (int i = 0; i < 4; ++i) {
        dense_chunk(-1, 1)->release_cells();   // churn: a's entry is evicted
    }
    REQUIRE((*pinned)[0] == Cell{-20, 7});
}

TEST_CASE("CC5 many threads read evicting chunks correctly", "[terrain]") {
    BudgetGuard guard(4 * kChunkBytes);
    std::vector<ChunkPtr> chunks;
    for (int i = 0; i < 64; ++i) {
        chunks.push_back(dense_chunk(static_cast<std::int8_t>(-1 - i), static_cast<std::uint8_t>(i)));
        chunks.back()->release_cells();
    }
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int pass = 0; pass < 50; ++pass) {
                for (int i = 0; i < 64; ++i) {
                    const int index = (i + t * 7) % 64;
                    const Cell expected{static_cast<std::int8_t>(-1 - index), static_cast<std::uint8_t>(index)};
                    if (!(chunks[static_cast<std::size_t>(index)]->cell(0) == expected)) {
                        ++wrong;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    REQUIRE(wrong == 0);
    REQUIRE(ChunkCache::global().bytes() <= 4 * kChunkBytes);
}

TEST_CASE("CC6 a decoded .avox keeps only frames resident", "[terrain]") {
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 100.f), 1));
    const std::vector<std::byte> bytes = encode_avox(volume);
    BudgetGuard guard(2 * kChunkBytes);
    VoxelVolume loaded(1.f);
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), loaded));
    REQUIRE(ChunkCache::global().bytes() <= 2 * kChunkBytes);
    REQUIRE(loaded.chunks().size() == volume.chunks().size());
    for (const auto& [coord, chunk] : loaded.chunks()) {
        REQUIRE_FALSE(chunk->cells_owned());
        const ChunkPtr& original = volume.chunks().at(coord);
        for (int i = 0; i < kChunkCells; i += 997) {
            REQUIRE(chunk->cell(i) == original->cell(i));
        }
    }
}

TEST_CASE("CC7 editing a released chunk changes only the copy", "[terrain]") {
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 40.f), 1));
    const std::vector<std::byte> bytes = encode_avox(volume);
    BudgetGuard guard(kChunkBytes);
    VoxelVolume loaded(1.f);
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), loaded));
    const ChunkMap before = loaded.chunks();
    REQUIRE_FALSE(loaded.paint(ball_at(0.f, 0.f, 0.f, 10.f), 2));
    REQUIRE(loaded.cell(CellCoord{0, 0, 0}).material == 2);
    REQUIRE(loaded.cell(CellCoord{0, 0, 30}).material == 1);
    // The chunks the paint replaced still read as they did.
    for (const auto& [coord, chunk] : before) {
        for (int i = 0; i < kChunkCells; i += 1013) {
            REQUIRE(chunk->cell(i) == volume.chunks().at(coord)->cell(i));
        }
    }
}

TEST_CASE("CC8 meshing a released volume matches meshing it decoded", "[terrain]") {
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 50.f), 1));
    const ChunkMesh reference = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 1}));
    REQUIRE_FALSE(reference.positions.empty());
    BudgetGuard guard(kChunkBytes);   // fewer than the 27 neighbours
    release_all_cells(volume.chunks());
    const ChunkMesh streamed = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 1}));
    REQUIRE(streamed.positions.size() == reference.positions.size());
    REQUIRE(streamed.triangles == reference.triangles);
    REQUIRE(streamed.triangle_ids == reference.triangle_ids);
}

TEST_CASE("CC9 a sampler reads released chunks", "[terrain]") {
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 50.f), 1));
    const float inside = VoxelSampler(volume.chunks(), 1.f).distance(Vec3{0.5f, 0.5f, 48.5f});
    BudgetGuard guard(kChunkBytes);
    release_all_cells(volume.chunks());
    REQUIRE(VoxelSampler(volume.chunks(), 1.f).distance(Vec3{0.5f, 0.5f, 48.5f}) == inside);
    for (const auto& [coord, chunk] : volume.chunks()) {
        REQUIRE_FALSE(chunk->cells_owned());
    }
}

// ---- First build admission (Task 3) ----

namespace {

Terrain& streaming_terrain(DataModel& game) {
    auto& t = game.create<Terrain>();
    game.set_parent(t.id(), workspace_of(game));
    return t;
}

// The coords a first sight meshes: every stored chunk and its 26 neighbours.
std::unordered_set<ChunkCoord, ChunkCoordHash> first_footprint(const VoxelVolume& volume) {
    std::unordered_set<ChunkCoord, ChunkCoordHash> footprint;
    for (const auto& [coord, chunk] : volume.chunks()) {
        (void)chunk;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    footprint.insert(ChunkCoord{coord.x + dx, coord.y + dy, coord.z + dz});
                }
            }
        }
    }
    return footprint;
}

// Updates until the first build is admitted and every job has landed.
void drain(TerrainWorld& world, DataModel& game, InstanceId id) {
    for (int i = 0; i < 100000; ++i) {
        world.update(game);
        world.wait_idle();
        if (world.first_build_remaining(id) == 0 && world.jobs_in_flight(id) == 0) {
            break;
        }
    }
    world.update(game);
}

}  // namespace

TEST_CASE("TW-S1 first build never has more jobs in flight than the cap", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = streaming_terrain(game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(0.f, 0.f, 0.f, 60.f), 0); }));
    const std::size_t footprint = first_footprint(t.volume()).size();
    TerrainWorld world({}, 2);
    const std::size_t cap = 2 * TerrainWorld::kFirstBuildJobsPerThread;
    REQUIRE(footprint > cap);
    world.update(game);
    REQUIRE(world.jobs_in_flight(t.id()) <= cap);
    REQUIRE(world.first_build_remaining(t.id()) + world.jobs_in_flight(t.id()) == footprint);
    for (int i = 0; i < 20; ++i) {
        world.update(game);
        REQUIRE(world.jobs_in_flight(t.id()) <= cap);
    }
}

TEST_CASE("TW-S2 first build meshes every surface chunk", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = streaming_terrain(game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(0.f, 0.f, 0.f, 60.f), 0); }));
    std::size_t surface = 0;
    for (const ChunkCoord& coord : first_footprint(t.volume())) {
        if (surface_nets(mesh_input(t.volume(), coord)).render != nullptr) {
            ++surface;
        }
    }
    TerrainWorld world({}, 2);
    drain(world, game, t.id());
    REQUIRE(world.first_build_remaining(t.id()) == 0);
    REQUIRE(world.views().size() == 1u);
    REQUIRE(world.views()[0].chunks->size() == surface);
}

TEST_CASE("TW-S3 an edit during first build meshes that chunk once, with the edit", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = streaming_terrain(game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(0.f, 0.f, 0.f, 60.f), 0); }));
    const std::size_t footprint = first_footprint(t.volume()).size();
    TerrainWorld world({}, 1);
    world.update(game);   // first sight: two chunks admitted, the rest wait
    // Carve a dent at the ball's far rim: its chunks are queued by the edit.
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(ball_at(0.f, 0.f, -60.f, 6.f)); }));
    drain(world, game, t.id());
    // Each chunk is meshed once, apart from the few in flight when the edit came.
    REQUIRE(world.meshed_count() <= footprint + TerrainWorld::kFirstBuildJobsPerThread);
    // The dent shows: every published mesh matches the voxels as they are now.
    for (const TerrainChunkView& chunk : *world.views()[0].chunks) {
        const ChunkMesh now = surface_nets(mesh_input(t.volume(), chunk.coord));
        REQUIRE(now.render != nullptr);
        REQUIRE(chunk.mesh->vertices.size() == now.render->vertices.size());
    }
}

// ---- The .alod store (Task 4) ----

namespace {

CompactMesh test_mesh(int seed, bool wide_indices) {
    CompactMesh mesh;
    mesh.origin = Vec3{static_cast<float>(seed), 2.f, 3.f};
    mesh.scale = Vec3{10.f, 20.f, 30.f + static_cast<float>(seed)};
    const int vertices = 4 + seed;
    for (int v = 0; v < vertices; ++v) {
        mesh.positions.insert(mesh.positions.end(), {static_cast<std::uint16_t>(v * 7 + seed),
                                                     static_cast<std::uint16_t>(v * 11), static_cast<std::uint16_t>(v)});
        mesh.normals.insert(mesh.normals.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(255 - v)});
        mesh.ids.insert(mesh.ids.end(), {1, static_cast<std::uint8_t>(seed), 0, 0});
        mesh.weights.insert(mesh.weights.end(), {255, 0, 0, static_cast<std::uint8_t>(v)});
    }
    for (int t = 0; t + 2 < vertices; ++t) {
        if (wide_indices) {
            mesh.indices32.insert(mesh.indices32.end(),
                                  {0u, static_cast<std::uint32_t>(t + 1), static_cast<std::uint32_t>(t + 2)});
        } else {
            mesh.indices.insert(mesh.indices.end(),
                                {0, static_cast<std::uint16_t>(t + 1), static_cast<std::uint16_t>(t + 2)});
        }
    }
    mesh.surface_index_count = 3;
    return mesh;
}

bool same_mesh(const CompactMesh& a, const CompactMesh& b) {
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.z == b.origin.z &&
           a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z &&
           a.positions == b.positions && a.normals == b.normals && a.ids == b.ids && a.weights == b.weights &&
           a.indices == b.indices && a.indices32 == b.indices32 && a.surface_index_count == b.surface_index_count;
}

// A fresh path in the temp folder, removed when the test ends.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

}  // namespace

TEST_CASE("AL1 committed nodes round-trip through a reopen", "[terrain]") {
    TempFile file("al1.alod");
    {
        std::optional<AlodStore> store = AlodStore::create(file.path, 0x1234u, 2.f);
        REQUIRE(store);
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.5f, Vec3{0, 0, 0}, Vec3{256, 64, 256}));
        REQUIRE(store->put(NodeKey{3, -1, 0, 2}, test_mesh(2, true), 4.f, Vec3{-512, 0, 1024}, Vec3{0, 64, 1536}));
        REQUIRE(store->put(NodeKey{2, 5, 1, -3}, test_mesh(3, false), 2.f, Vec3{1, 2, 3}, Vec3{4, 5, 6}));
        store->set_surface_chunks({ChunkCoord{0, 0, 0}, ChunkCoord{-4, 1, 7}});
        REQUIRE(store->commit());
    }
    std::optional<AlodStore> store = AlodStore::open(file.path, 0x1234u, 2.f);
    REQUIRE(store);
    REQUIRE(store->entries().size() == 3u);
    const AlodEntry& entry = store->entries().at(NodeKey{3, -1, 0, 2});
    REQUIRE(entry.error == 4.f);
    REQUIRE(entry.bounds_min.x == -512.f);
    REQUIRE(entry.bounds_max.z == 1536.f);
    REQUIRE(same_mesh(*store->load(NodeKey{2, 0, 0, 0}), test_mesh(1, false)));
    REQUIRE(same_mesh(*store->load(NodeKey{3, -1, 0, 2}), test_mesh(2, true)));
    REQUIRE(same_mesh(*store->load(NodeKey{2, 5, 1, -3}), test_mesh(3, false)));
    REQUIRE(store->load(NodeKey{2, 9, 9, 9}) == nullptr);
    REQUIRE(store->surface_chunks().size() == 2u);
    REQUIRE(store->surface_chunks()[1] == ChunkCoord{-4, 1, 7});
}

TEST_CASE("AL2 a node put twice reads back its newest mesh", "[terrain]") {
    TempFile file("al2.alod");
    {
        std::optional<AlodStore> store = AlodStore::create(file.path, 7u, 1.f);
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.f, Vec3{}, Vec3{}));
        REQUIRE(store->commit());
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(5, false), 3.f, Vec3{}, Vec3{}));
        REQUIRE(same_mesh(*store->load(NodeKey{2, 0, 0, 0}), test_mesh(5, false)));   // before commit too
        REQUIRE(store->commit());
    }
    std::optional<AlodStore> store = AlodStore::open(file.path, 7u, 1.f);
    REQUIRE(store);
    REQUIRE(store->entries().size() == 1u);
    REQUIRE(store->entries().at(NodeKey{2, 0, 0, 0}).error == 3.f);
    REQUIRE(same_mesh(*store->load(NodeKey{2, 0, 0, 0}), test_mesh(5, false)));
}

namespace {

// Flips bytes of key's record in the file so its first index points past
// the last vertex. The record's size and key stay right, so only load()'s
// own checks can catch it.
void corrupt_first_index(const std::filesystem::path& path, const AlodEntry& entry, bool wide_indices) {
    // Record layout, after the key (four i32): f32 error, vec3 min, vec3 max,
    // u32 surface_index_count, vec3 origin, vec3 scale, u32 vertices, u32
    // indices16, u32 indices32, then positions (u16 x 3 per vertex), normals
    // (u8 x 2), ids (u8 x 4), weights (u8 x 4), indices16, indices32.
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    const std::uint64_t key_bytes = 4 * sizeof(std::int32_t);
    const std::uint64_t header = key_bytes + 4 + 12 + 12 + 4 + 12 + 12;
    file.seekg(static_cast<std::streamoff>(entry.offset + header));
    std::uint32_t vertices = 0;
    file.read(reinterpret_cast<char*>(&vertices), 4);
    const std::uint64_t per_vertex = 6 + 2 + 4 + 4;
    const std::uint64_t first_index = entry.offset + header + 12 + vertices * per_vertex;
    file.seekp(static_cast<std::streamoff>(first_index));
    if (wide_indices) {
        const std::uint32_t bad = 0xFFFFFFF0u;
        file.write(reinterpret_cast<const char*>(&bad), 4);
    } else {
        const std::uint16_t bad = 0xFFF0u;
        file.write(reinterpret_cast<const char*>(&bad), 2);
    }
    REQUIRE(file.good());
}

}  // namespace

TEST_CASE("AL8 a record whose index is past its vertices is a miss, not a mesh", "[terrain]") {
    TempFile file("al8.alod");
    std::optional<AlodStore> store = AlodStore::create(file.path, 0x55u, 1.f);
    REQUIRE(store);
    REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.f, Vec3{}, Vec3{1, 1, 1}));
    REQUIRE(store->put(NodeKey{2, 1, 0, 0}, test_mesh(2, true), 1.f, Vec3{}, Vec3{1, 1, 1}));
    REQUIRE(store->commit());
    REQUIRE(store->load(NodeKey{2, 0, 0, 0}) != nullptr);
    REQUIRE(store->load(NodeKey{2, 1, 0, 0}) != nullptr);

    corrupt_first_index(file.path, store->entries().at(NodeKey{2, 0, 0, 0}), false);
    corrupt_first_index(file.path, store->entries().at(NodeKey{2, 1, 0, 0}), true);
    REQUIRE(store->load(NodeKey{2, 0, 0, 0}) == nullptr);
    REQUIRE(store->load(NodeKey{2, 1, 0, 0}) == nullptr);
}

TEST_CASE("AL3 a damaged file is not a store", "[terrain]") {
    TempFile file("al3.alod");
    {
        std::optional<AlodStore> store = AlodStore::create(file.path, 7u, 1.f);
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.f, Vec3{}, Vec3{}));
        REQUIRE(store->commit());
    }
    const std::uintmax_t size = std::filesystem::file_size(file.path);
    SECTION("truncated") {
        std::filesystem::resize_file(file.path, size - 1);
        REQUIRE_FALSE(AlodStore::open(file.path, 7u, 1.f));
    }
    SECTION("a footer byte flipped") {
        std::fstream io(file.path, std::ios::in | std::ios::out | std::ios::binary);
        io.seekg(static_cast<std::streamoff>(size - 2));
        char c = 0;
        io.read(&c, 1);
        c = static_cast<char>(c ^ 0x5a);
        io.seekp(static_cast<std::streamoff>(size - 2));
        io.write(&c, 1);
        io.close();
        REQUIRE_FALSE(AlodStore::open(file.path, 7u, 1.f));
    }
    SECTION("not a store at all") {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out << "hello";
        out.close();
        REQUIRE_FALSE(AlodStore::open(file.path, 7u, 1.f));
    }
    SECTION("missing") {
        std::filesystem::remove(file.path);
        REQUIRE_FALSE(AlodStore::open(file.path, 7u, 1.f));
    }
}

TEST_CASE("AL4 a store for other voxels is not opened", "[terrain]") {
    TempFile file("al4.alod");
    {
        std::optional<AlodStore> store = AlodStore::create(file.path, 7u, 1.f);
        REQUIRE(store->commit());
    }
    REQUIRE(AlodStore::open(file.path, 7u, 1.f));
    REQUIRE_FALSE(AlodStore::open(file.path, 8u, 1.f));
    REQUIRE_FALSE(AlodStore::open(file.path, 7u, 2.f));
}

TEST_CASE("AL5 nodes put after the last commit are not seen by a reopen", "[terrain]") {
    TempFile file("al5.alod");
    {
        std::optional<AlodStore> store = AlodStore::create(file.path, 7u, 1.f);
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.f, Vec3{}, Vec3{}));
        REQUIRE(store->commit());
        REQUIRE(store->put(NodeKey{2, 1, 0, 0}, test_mesh(2, false), 1.f, Vec3{}, Vec3{}));
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(3, false), 1.f, Vec3{}, Vec3{}));
        // No commit: as if Studio stopped here.
    }
    std::optional<AlodStore> store = AlodStore::open(file.path, 7u, 1.f);
    REQUIRE(store);
    REQUIRE(store->entries().size() == 1u);
    REQUIRE(same_mesh(*store->load(NodeKey{2, 0, 0, 0}), test_mesh(1, false)));
}

TEST_CASE("AL6 content keys differ when the bytes do", "[terrain]") {
    const std::vector<std::byte> a{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> b{std::byte{1}, std::byte{2}, std::byte{4}};
    REQUIRE(content_key_of(a.data(), a.size()) == content_key_of(a.data(), a.size()));
    REQUIRE(content_key_of(a.data(), a.size()) != content_key_of(b.data(), b.size()));
    REQUIRE(content_key_of(a.data(), a.size()) != 0u);
}

// ---- The .alod store wired into Terrain and TerrainWorld (Task 6) ----

namespace {

constexpr int kSlabChunks = 24;   // a 24 x 1 x 24 chunk slab: wider than kNearChunks reaches

std::optional<std::string> fill_slab(VoxelVolume& volume) {
    Shape slab;
    slab.kind = Shape::Kind::Block;
    const float side = static_cast<float>(kSlabChunks * kChunkSize);
    slab.frame = matrix4_translation(side * 0.5f, 16.f, side * 0.5f);
    slab.size = Vec3{side - 8.f, 16.f, side - 8.f};
    return volume.fill(slab, 1);
}

Camera& look_from(DataModel& game, Vec3 at) {
    Camera& camera = game.create<Camera>();
    camera.set_transform(matrix4_translation(at.x, at.y, at.z));
    game.set_parent(camera.id(), workspace_of(game));
    auto* workspace = dynamic_cast<Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->set_current_camera(camera.id()));
    return camera;
}

Terrain& slab_named(DataModel& game) {
    auto* terrain = dynamic_cast<Terrain*>(game.instance(game.find_first_child(workspace_of(game), "Slab")));
    REQUIRE(terrain != nullptr);
    return *terrain;
}

// Updates until the first build is in, every job has landed, and the LOD
// tree has built what it can, with a clock past the debounce each update.
void settle_all(TerrainWorld& world, DataModel& game, InstanceId id, double& now) {
    for (int i = 0; i < 24; ++i) {
        world.update(game, now);
        world.wait_idle();
        now += 2.0 * kRebuildIntervalMs;
        if (world.first_build_remaining(id) != 0 || world.jobs_in_flight(id) != 0) {
            i = 0;
        }
    }
}

// A project with the slab saved, and its first (cold) build run so the
// .alod is written.
void make_slab_project(const TempDir& dir) {
    Game game;
    Project project = Project::create(dir.path, game);
    auto& t = game.create<Terrain>();
    game.set_parent(t.id(), workspace_of(game));
    game.set_name(t.id(), "Slab");
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return fill_slab(v); }));
    project.save();
    look_from(game, Vec3{16.f, 40.f, 16.f});
    TerrainWorld world({}, 2);
    double now = 0.0;
    settle_all(world, game, t.id(), now);
    REQUIRE(std::filesystem::is_regular_file(t.lod_cache_path()));
}

}  // namespace

TEST_CASE("TW-A1 a reopened Terrain shows its far nodes from the .alod and meshes only what is near", "[terrain]") {
    SimRole role;
    TempDir dir;
    make_slab_project(dir);
    Game game;
    Project project = Project::load(dir.path, game);
    Terrain& t = slab_named(game);
    REQUIRE(AlodStore::open(t.lod_cache_path(), t.content_key(), t.volume().voxel_size()));
    const std::size_t footprint = first_footprint(t.volume()).size();
    look_from(game, Vec3{16.f, 40.f, 16.f});
    TerrainWorld world({}, 2);
    double now = 0.0;
    settle_all(world, game, t.id(), now);
    // Only chunks near the camera (in a corner of the slab), not the slab.
    REQUIRE(world.meshed_count() < static_cast<std::uint64_t>(kSlabChunks * kSlabChunks / 2));
    (void)footprint;
    REQUIRE(world.views().size() == 1u);
    REQUIRE(world.views()[0].nodes != nullptr);
    const LodTree* tree = world.lod_tree(t.id());
    REQUIRE(tree != nullptr);
    bool top_shown = false;
    for (const TerrainNodeView& node : *world.views()[0].nodes) {
        top_shown = top_shown || node.key.level == world.views()[0].top_level;
    }
    REQUIRE(top_shown);
}

TEST_CASE("TW-A2 an edit saved after a warm open is in the .alod a later open reads", "[terrain]") {
    SimRole role;
    TempDir dir;
    make_slab_project(dir);
    const ChunkCoord far_chunk{kSlabChunks - 2, 0, kSlabChunks - 2};
    const NodeKey far_node = node_of(far_chunk, 2);
    std::size_t before_vertices = 0;
    std::filesystem::path alod;
    {
        Game game;
        Project project = Project::load(dir.path, game);
        Terrain& t = slab_named(game);
        alod = t.lod_cache_path();
        {
            std::optional<AlodStore> store = AlodStore::open(alod, t.content_key(), t.volume().voxel_size());
            REQUIRE(store);
            before_vertices = store->load(far_node)->positions.size();
        }
        look_from(game, Vec3{16.f, 40.f, 16.f});
        TerrainWorld world({}, 2);
        double now = 0.0;
        settle_all(world, game, t.id(), now);
        // Carve a deep pit into the far corner, then save.
        const float x = (static_cast<float>(far_chunk.x) + 0.5f) * kChunkSize;
        const float z = (static_cast<float>(far_chunk.z) + 0.5f) * kChunkSize;
        REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(ball_at(x, 24.f, z, 14.f)); }));
        settle_all(world, game, t.id(), now);
        project.save();
        settle_all(world, game, t.id(), now);
        std::optional<AlodStore> store = AlodStore::open(alod, t.content_key(), t.volume().voxel_size());
        REQUIRE(store);   // rewritten for the saved voxels
        REQUIRE(store->load(far_node)->positions.size() != before_vertices);
    }
    Game game;
    Project project = Project::load(dir.path, game);
    Terrain& t = slab_named(game);
    std::optional<AlodStore> store = AlodStore::open(alod, t.content_key(), t.volume().voxel_size());
    REQUIRE(store);
    REQUIRE(store->load(far_node)->positions.size() != before_vertices);
}

TEST_CASE("TW-A3 a damaged .alod is rebuilt from the voxels", "[terrain]") {
    SimRole role;
    TempDir dir;
    make_slab_project(dir);
    Game game;
    Project project = Project::load(dir.path, game);
    Terrain& t = slab_named(game);
    const std::filesystem::path alod = t.lod_cache_path();
    std::filesystem::resize_file(alod, std::filesystem::file_size(alod) - 1);
    const std::size_t footprint = first_footprint(t.volume()).size();
    look_from(game, Vec3{16.f, 40.f, 16.f});
    TerrainWorld world({}, 2);
    double now = 0.0;
    settle_all(world, game, t.id(), now);
    REQUIRE(world.meshed_count() >= footprint);
    REQUIRE(AlodStore::open(alod, t.content_key(), t.volume().voxel_size()));
}

// ---- Review fixes ----

TEST_CASE("AL7 a store never committed is not a store", "[terrain]") {
    TempFile file("al7.alod");
    {
        std::optional<AlodStore> store = AlodStore::create(file.path, 7u, 1.f);
        REQUIRE(store);
        REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.f, Vec3{}, Vec3{}));
        // Not committed: Studio stopped before the first build settled.
    }
    REQUIRE_FALSE(AlodStore::open(file.path, 7u, 1.f));
}

TEST_CASE("TK1 a Terrain's content key holds only while its voxels match the file", "[terrain]") {
    SimRole role;
    TempDir dir;
    {
        Game game;
        Project project = Project::create(dir.path, game);
        auto& t = game.create<Terrain>();
        game.set_parent(t.id(), workspace_of(game));
        game.set_name(t.id(), "Slab");
        REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(0.f, 0.f, 0.f, 10.f), 1); }));
        REQUIRE(t.content_key() == 0u);   // no file holds these voxels yet
        project.save();
        REQUIRE(t.content_key() != 0u);
    }
    Game game;
    Project project = Project::load(dir.path, game);
    Terrain& t = slab_named(game);
    const std::uint64_t loaded = t.content_key();
    REQUIRE(loaded != 0u);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(20.f, 0.f, 0.f, 6.f), 1); }));
    REQUIRE(t.content_key() == 0u);   // edited: the file no longer describes them
    project.save();
    REQUIRE(t.content_key() != 0u);
    REQUIRE(t.content_key() != loaded);
}

TEST_CASE("TW-A4 a rewrite that cannot replace a locked .alod keeps serving the new meshes", "[terrain]") {
    SimRole role;
    TempDir dir;
    make_slab_project(dir);
    Game game;
    Project project = Project::load(dir.path, game);
    Terrain& t = slab_named(game);
    const std::filesystem::path alod = t.lod_cache_path();
    std::filesystem::path temp = alod;
    temp += ".tmp";
    look_from(game, Vec3{16.f, 40.f, 16.f});
    TerrainWorld world({}, 2);
    double now = 0.0;
    settle_all(world, game, t.id(), now);
    const ChunkCoord far_chunk{kSlabChunks - 2, 0, kSlabChunks - 2};
    const float x = (static_cast<float>(far_chunk.x) + 0.5f) * kChunkSize;
    const float z = (static_cast<float>(far_chunk.z) + 0.5f) * kChunkSize;
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(ball_at(x, 24.f, z, 14.f)); }));
    settle_all(world, game, t.id(), now);
    {
        // Something else (an indexer, a virus scanner) holds the old file open.
        std::ifstream holder(alod, std::ios::binary);
        REQUIRE(holder.is_open());
        project.save();
        settle_all(world, game, t.id(), now);
    }
    // The old file could not be replaced: the new store lives on beside it,
    // complete and under the saved voxels' key.
    std::optional<AlodStore> fresh = AlodStore::open(temp, t.content_key(), t.volume().voxel_size());
    REQUIRE(fresh);
    REQUIRE(fresh->entries().size() > 0u);
    // Flying to the far corner reads its new meshes back without a rebuild.
    look_from(game, Vec3{x, 40.f, z});
    settle_all(world, game, t.id(), now);
    const LodTree::Node* node = world.lod_tree(t.id())->find(node_of(far_chunk, 2));
    REQUIRE(node != nullptr);
    REQUIRE(node->resident);
}

TEST_CASE("TW-A5 a warm open's updates before a camera exists mesh nothing", "[terrain]") {
    // Studio's first updates after opening a project come before Workspace
    // has a camera. With no camera everything is wanted, which on a warm
    // open means every chunk and every far node at once.
    SimRole role;
    TempDir dir;
    make_slab_project(dir);
    Game game;
    Project project = Project::load(dir.path, game);
    Terrain& t = slab_named(game);
    const std::size_t footprint = first_footprint(t.volume()).size();
    TerrainWorld world({}, 2);
    double now = 0.0;
    for (int i = 0; i < 10; ++i) {
        world.update(game, now);
        world.wait_idle();
        now += 2.0 * kRebuildIntervalMs;
    }
    look_from(game, Vec3{16.f, 40.f, 16.f});
    settle_all(world, game, t.id(), now);
    // Only chunks near the camera (in a corner of the slab), not the slab.
    REQUIRE(world.meshed_count() < static_cast<std::uint64_t>(kSlabChunks * kSlabChunks / 2));
    (void)footprint;
}
