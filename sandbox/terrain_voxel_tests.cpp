// The voxel core under Terrain: chunks, shapes, the volume, and the .avox file.
// No instances; nothing here touches the DataModel.

#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

using namespace engine_core;
using namespace engine_core::terrain;

namespace {
bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
}  // namespace

TEST_CASE("VC1 distances quantize to the band and back", "[terrain]") {
    REQUIRE(quantize(0.f, 1.f) == 0);
    REQUIRE(quantize(4.f, 1.f) == 127);
    REQUIRE(quantize(100.f, 1.f) == 127);
    REQUIRE(quantize(-4.f, 1.f) == -127);
    REQUIRE(quantize(-100.f, 1.f) == -127);
    REQUIRE(near(dequantize(quantize(1.5f, 1.f), 1.f), 1.5f, 4.f / 127.f));
    // Studs, not cells: a bigger VoxelSize widens the band.
    REQUIRE(quantize(4.f, 2.f) == 64);
}

TEST_CASE("VC2 cells map to chunks and indices, negatives included", "[terrain]") {
    REQUIRE(chunk_of(0, 0, 0) == ChunkCoord{0, 0, 0});
    REQUIRE(chunk_of(31, 32, -1) == ChunkCoord{0, 1, -1});
    REQUIRE(chunk_of(-32, -33, 64) == ChunkCoord{-1, -2, 2});
    REQUIRE(cell_index(0, 0, 0) == 0);
    REQUIRE(cell_index(1, 0, 0) == 1);
    REQUIRE(cell_index(0, 1, 0) == 32);
    REQUIRE(cell_index(0, 0, 1) == 1024);
}

TEST_CASE("VC3 a dense chunk collapses when its cells agree, and tracks Ids", "[terrain]") {
    std::shared_ptr<ChunkData> chunk = ChunkData::air()->clone_dense();
    chunk->set(cell_index(3, 4, 5), Cell{-20, 7});
    chunk->finish();
    REQUIRE_FALSE(chunk->is_uniform());
    REQUIRE((chunk->ids_used()[0] >> 7 & 1u) == 1u);
    REQUIRE(chunk->cell(cell_index(3, 4, 5)) == Cell{-20, 7});
    REQUIRE(chunk->cell(0) == Cell{kAirDistance, 0});

    chunk->set(cell_index(3, 4, 5), Cell{kAirDistance, 7});
    chunk->finish();
    REQUIRE(chunk->is_air());
    REQUIRE(chunk->ids_used()[0] == 0u);
}

TEST_CASE("VC4 cloning leaves the original as it was", "[terrain]") {
    const ChunkPtr solid = ChunkData::uniform(Cell{kSolidDistance, 2});
    std::shared_ptr<ChunkData> copy = solid->clone_dense();
    copy->set(0, Cell{kAirDistance, 0});
    copy->finish();
    REQUIRE(solid->cell(0) == Cell{kSolidDistance, 2});
    REQUIRE(copy->cell(0) == Cell{kAirDistance, 0});
    REQUIRE(normalized(Cell{kAirDistance, 9}) == Cell{kAirDistance, 0});
}

TEST_CASE("VC5 shape distances are exact for ball, box, and cylinder", "[terrain]") {
    Shape ball;
    ball.center = Vec3{1.f, 2.f, 3.f};
    ball.radius = 2.f;
    prepare_shape(ball);
    REQUIRE(near(shape_distance(ball, Vec3{1.f, 2.f, 3.f}), -2.f, 1e-5f));
    REQUIRE(near(shape_distance(ball, Vec3{4.f, 2.f, 3.f}), 1.f, 1e-5f));

    Shape block;
    block.kind = Shape::Kind::Block;
    block.frame = matrix4_translation(0.f, 10.f, 0.f);
    block.size = Vec3{2.f, 4.f, 6.f};
    prepare_shape(block);
    REQUIRE(near(shape_distance(block, Vec3{0.f, 10.f, 0.f}), -1.f, 1e-5f));
    REQUIRE(near(shape_distance(block, Vec3{0.f, 13.f, 0.f}), 1.f, 1e-5f));
    REQUIRE(near(shape_distance(block, Vec3{2.f, 13.f, 0.f}), std::sqrt(2.f), 1e-5f));

    Shape cylinder;
    cylinder.kind = Shape::Kind::Cylinder;
    cylinder.size = Vec3{2.f, 4.f, 2.f};   // radius 1, height 4, along Y
    prepare_shape(cylinder);
    REQUIRE(near(shape_distance(cylinder, Vec3{3.f, 0.f, 0.f}), 2.f, 1e-5f));
    REQUIRE(near(shape_distance(cylinder, Vec3{0.f, 5.f, 0.f}), 3.f, 1e-5f));
}

TEST_CASE("VC6 a wedge is solid under its slope and empty above it", "[terrain]") {
    Shape wedge;
    wedge.kind = Shape::Kind::Wedge;
    wedge.size = Vec3{4.f, 4.f, 4.f};
    prepare_shape(wedge);
    // Its tall side is at +Z; it slopes down toward -Z.
    REQUIRE(shape_distance(wedge, Vec3{0.f, -1.5f, 1.5f}) < 0.f);
    REQUIRE(shape_distance(wedge, Vec3{0.f, 1.5f, -1.5f}) > 0.f);
    REQUIRE(shape_distance(wedge, Vec3{0.f, 5.f, 0.f}) > 0.f);
}

TEST_CASE("VC7 shape bounds cover the shape and its margin", "[terrain]") {
    Shape ball;
    ball.center = Vec3{10.f, 0.f, 0.f};
    ball.radius = 3.f;
    prepare_shape(ball);
    Vec3 min{}, max{};
    shape_bounds(ball, 4.f, min, max);
    REQUIRE(near(min.x, 3.f, 1e-5f));
    REQUIRE(near(max.x, 17.f, 1e-5f));

    Shape block;
    block.kind = Shape::Kind::Block;
    // Turned 90 degrees about Y: its 2-wide X becomes Z.
    block.frame = matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, 1.5707964);
    block.size = Vec3{2.f, 2.f, 8.f};
    prepare_shape(block);
    shape_bounds(block, 0.f, min, max);
    REQUIRE(near(max.x, 4.f, 1e-4f));
    REQUIRE(near(max.z, 1.f, 1e-4f));
}

namespace {
Shape ball_at(float x, float y, float z, float r) {
    Shape s;
    s.center = Vec3{x, y, z};
    s.radius = r;
    return s;
}
}  // namespace

TEST_CASE("V1 a filled ball has the right distances and material", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 3));
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).distance == kSolidDistance);
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 3);
    REQUIRE(near(dequantize(volume.cell(CellCoord{6, 0, 0}).distance, 1.f), 1.f, 0.05f));
    REQUIRE(volume.cell(CellCoord{5, 0, 0}).material == 3);   // band cell at the surface (s=0): painted
    REQUIRE(volume.cell(CellCoord{20, 0, 0}).distance == kAirDistance);
    REQUIRE((volume.ids_used()[0] >> 3 & 1u) == 1u);
}

TEST_CASE("V2 subtracting what was filled leaves no chunks", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 1));
    REQUIRE_FALSE(volume.subtract(ball_at(0.f, 0.f, 0.f, 14.f)));
    REQUIRE(volume.chunks().empty());
    REQUIRE(volume.ids_used() == std::array<std::uint64_t, 4>{});
}

TEST_CASE("V3 an edit clones only the chunks it changes", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 3.f), 1));
    REQUIRE_FALSE(volume.fill(ball_at(100.f, 0.f, 0.f, 3.f), 1));
    const ChunkMap before = volume.chunks();
    REQUIRE_FALSE(volume.fill(ball_at(100.f, 0.f, 0.f, 4.f), 2));
    for (const auto& [coord, chunk] : before) {
        if (coord.x <= 0) {
            REQUIRE(volume.chunks().at(coord) == chunk);   // same pointer: untouched
        }
    }
    // The old snapshot still says Id 1 at the far ball's middle.
    REQUIRE(before.at(chunk_of(100, 0, 0))->cell(cell_index(100 - 96, 0, 0)).material == 1);
    REQUIRE(volume.cell(CellCoord{100, 0, 0}).material == 2);
}

TEST_CASE("V4 paint changes only solid cells' Ids; replace swaps one Id", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 1));
    REQUIRE_FALSE(volume.paint(ball_at(0.f, 0.f, 0.f, 2.f), 4));
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 4);
    REQUIRE(volume.cell(CellCoord{4, 0, 0}).material == 1);
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).distance == kSolidDistance);   // shape unchanged
    REQUIRE_FALSE(volume.replace(CellCoord{-10, -10, -10}, CellCoord{10, 10, 10}, 1, 6));
    REQUIRE(volume.cell(CellCoord{4, 0, 0}).material == 6);
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 4);
}

TEST_CASE("V5 read then write round-trips exactly", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 9));
    std::vector<float> distances;
    std::vector<std::uint8_t> materials;
    REQUIRE_FALSE(volume.read(CellCoord{-8, -8, -8}, CellCoord{8, 8, 8}, distances, materials));
    REQUIRE(distances.size() == 17u * 17u * 17u);
    VoxelVolume copy;
    REQUIRE_FALSE(copy.write(CellCoord{-8, -8, -8}, CellCoord{8, 8, 8}, distances, materials));
    for (int x = -8; x <= 8; ++x) {
        REQUIRE(copy.cell(CellCoord{x, 1, 2}) == volume.cell(CellCoord{x, 1, 2}));
    }
}

TEST_CASE("V6 an edit dirties its chunks and their neighbors", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(16.f, 16.f, 16.f, 2.f), 1));
    std::vector<ChunkCoord> dirty;
    volume.take_dirty(dirty);
    REQUIRE(dirty.size() == 27u);
    volume.take_dirty(dirty);
    REQUIRE(dirty.empty());
}

TEST_CASE("V7 too large an edit is refused; a zero ball changes nothing", "[terrain]") {
    VoxelVolume volume;
    Shape huge;
    huge.kind = Shape::Kind::Block;
    huge.size = Vec3{300.f, 300.f, 300.f};
    REQUIRE(*volume.fill(huge, 1) == "Terrain edit too large: split it into smaller calls");
    REQUIRE(volume.chunks().empty());
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 0.f), 1));
    REQUIRE(volume.chunks().empty());
}

TEST_CASE("V8 set_chunks dirties only what changed", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 3.f), 1));
    const ChunkMap saved = volume.chunks();
    std::vector<ChunkCoord> dirty;
    volume.take_dirty(dirty);
    volume.set_chunks(saved);
    volume.take_dirty(dirty);
    REQUIRE(dirty.empty());
    REQUIRE_FALSE(volume.fill(ball_at(200.f, 0.f, 0.f, 3.f), 1));
    volume.take_dirty(dirty);
    volume.set_chunks(saved);
    volume.take_dirty(dirty);
    REQUIRE_FALSE(dirty.empty());
    REQUIRE(volume.chunks().size() == saved.size());
}

TEST_CASE("V9 FillBall of radius 8 is fast", "[.][terrain-bench]") {
    VoxelVolume volume;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        REQUIRE_FALSE(volume.fill(ball_at(static_cast<float>(i * 40), 0.f, 0.f, 8.f), 1));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    INFO(ms / 100.0);
    REQUIRE(ms / 100.0 < 0.5);
}

TEST_CASE("V10 an inverted box is refused by read, write, and replace", "[terrain]") {
    VoxelVolume volume;
    std::vector<float> distances;
    std::vector<std::uint8_t> materials;
    // One axis inverted.
    REQUIRE(*volume.read(CellCoord{5, 0, 0}, CellCoord{-5, 0, 0}, distances, materials) ==
            "max must not be less than min on any axis");
    REQUIRE(*volume.write(CellCoord{0, 5, 0}, CellCoord{0, -5, 0}, distances, materials) ==
            "max must not be less than min on any axis");
    REQUIRE(*volume.replace(CellCoord{0, 0, 5}, CellCoord{0, 0, -5}, 1, 2) ==
            "max must not be less than min on any axis");
    // Two axes inverted: the naive product would come out positive again.
    REQUIRE(*volume.read(CellCoord{5, 5, 0}, CellCoord{-5, -5, 0}, distances, materials) ==
            "max must not be less than min on any axis");
    REQUIRE(volume.chunks().empty());
}
