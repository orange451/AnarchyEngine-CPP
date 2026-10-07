// The voxel core under Terrain: chunks, shapes, the volume, and the .avox file.
// No instances; nothing here touches the DataModel.

#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelChunk.hpp"

#include <catch2/catch_test_macros.hpp>

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
