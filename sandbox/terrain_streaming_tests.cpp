// Terrain streaming: the decoded-chunk cache, first-build admission, and the
// .alod far-mesh cache. No instances unless a test says otherwise.

#include "terrain/AvoxFile.hpp"
#include "terrain/ChunkCache.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelSampler.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
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
