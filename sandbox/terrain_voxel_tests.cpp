// The voxel core under Terrain: chunks, shapes, the volume, and the .avox file.
// No instances; nothing here touches the DataModel.

#include "terrain/AvoxFile.hpp"
#include "terrain/ChunkFrame.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

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

TEST_CASE("V11 ids_used tracks exactly through Id reassignment and removal", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 2));
    REQUIRE_FALSE(volume.paint(ball_at(0.f, 0.f, 0.f, 2.f), 3));
    REQUIRE((volume.ids_used()[0] >> 2 & 1u) == 1u);
    REQUIRE((volume.ids_used()[0] >> 3 & 1u) == 1u);
    // A covering fill, comfortably bigger than the first ball's whole band,
    // repaints every cell the first fill touched: Id 2 is replaced
    // everywhere, not just where this fill's own band happens to land.
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 10.f), 3));
    REQUIRE((volume.ids_used()[0] >> 2 & 1u) == 0u);
    REQUIRE((volume.ids_used()[0] >> 3 & 1u) == 1u);
    // Subtracting everything clears every cell to air: Id 3 vanishes too,
    // and the chunk(s) collapse away entirely.
    REQUIRE_FALSE(volume.subtract(ball_at(0.f, 0.f, 0.f, 20.f)));
    REQUIRE(volume.ids_used() == std::array<std::uint64_t, 4>{});
    REQUIRE(volume.chunks().empty());
}

namespace {
// Two balls of Id 3 a cell apart either side of x = 0: smoothing fills the
// crease between them, pulling cells from beyond a voxel of the surface to
// within one, where their Id shows.
VoxelVolume crease_of_id_3() {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(-5.f, 0.f, 0.f, 4.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 0.f, 0.f, 4.f), 3));
    return volume;
}

// Every cell near the crease that is within a voxel of the surface, or
// inside, has Id 3.
void require_id_3_where_it_shows(const VoxelVolume& volume) {
    for (int z = -10; z <= 10; ++z) {
        for (int y = -10; y <= 10; ++y) {
            for (int x = -10; x <= 10; ++x) {
                const Cell cell = volume.cell(CellCoord{x, y, z});
                if (dequantize(cell.distance, 1.f) <= 1.f) {
                    REQUIRE(cell.material == 3);
                }
            }
        }
    }
}
}  // namespace

TEST_CASE("V12 smoothing gives a cell it brings to the surface its neighbours' Id, not the default", "[terrain]") {
    VoxelVolume volume = crease_of_id_3();
    REQUIRE(dequantize(volume.cell(CellCoord{0, 0, 0}).distance, 1.f) > 1.f);
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 0);
    require_id_3_where_it_shows(volume);
    for (int i = 0; i < 8; ++i) {
        REQUIRE_FALSE(volume.smooth(Vec3{0.f, 0.f, 0.f}, 6.f, 1.f));
    }
    REQUIRE(dequantize(volume.cell(CellCoord{0, 0, 0}).distance, 1.f) <= 1.f);
    require_id_3_where_it_shows(volume);
}

TEST_CASE("V13 smoothing replaces a dug-out cell's old Id as it comes back to the surface", "[terrain]") {
    // Id 5 dug out of the crease leaves its Id on the air cells there.
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 2.f), 5));
    REQUIRE_FALSE(volume.subtract(ball_at(0.f, 0.f, 0.f, 3.5f)));
    REQUIRE_FALSE(volume.fill(ball_at(-5.f, 0.f, 0.f, 4.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 0.f, 0.f, 4.f), 3));
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 5);
    for (int i = 0; i < 8; ++i) {
        REQUIRE_FALSE(volume.smooth(Vec3{0.f, 0.f, 0.f}, 6.f, 1.f));
    }
    require_id_3_where_it_shows(volume);
}

TEST_CASE("V14 smoothing with no radius changes nothing; a far-off ball is refused", "[terrain]") {
    VoxelVolume volume = crease_of_id_3();
    const std::uint64_t revision = volume.revision();
    REQUIRE_FALSE(volume.smooth(Vec3{0.f, 0.f, 0.f}, 0.f, 1.f));
    REQUIRE(volume.revision() == revision);
    REQUIRE(volume.smooth(Vec3{1e30f, 0.f, 0.f}, 4.f, 1.f));
    REQUIRE(volume.smooth(Vec3{std::nanf(""), 0.f, 0.f}, 4.f, 1.f));
    REQUIRE(volume.revision() == revision);
}

TEST_CASE("VR1 replace_everywhere changes one Id across far-apart chunks", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(5000.f, 0.f, 0.f, 5.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 900.f, 0.f, 5.f), 4));
    volume.replace_everywhere(3, 7);
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 7);
    REQUIRE(volume.cell(CellCoord{5000, 0, 0}).material == 7);
    REQUIRE(volume.cell(CellCoord{0, 900, 0}).material == 4);
    REQUIRE((volume.ids_used()[0] >> 3 & 1u) == 0u);
    REQUIRE((volume.ids_used()[0] >> 7 & 1u) == 1u);
}

TEST_CASE("VR2 replace_everywhere leaves chunks without the Id untouched", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(300.f, 0.f, 0.f, 5.f), 4));
    const ChunkMap before = volume.chunks();
    REQUIRE(volume.replace_everywhere(3, 7) > 0u);
    for (const auto& [coord, chunk] : before) {
        if (coord.x >= 8) REQUIRE(volume.chunks().at(coord) == chunk);   // the far ball's chunks: same pointers
    }
    REQUIRE(volume.replace_everywhere(9, 1) == 0u);
}

TEST_CASE("VR3 revision moves with every change, and ids_used follows it", "[terrain]") {
    VoxelVolume volume;
    const std::uint64_t r0 = volume.revision();
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 2));
    REQUIRE(volume.revision() != r0);
    REQUIRE((volume.ids_used()[0] >> 2 & 1u) == 1u);
    const std::uint64_t r1 = volume.revision();
    REQUIRE_FALSE(volume.fill(ball_at(1000.f, 0.f, 0.f, 0.f), 2));   // a no-op edit
    REQUIRE(volume.revision() == r1);
}

TEST_CASE("VR4 move-assign keeps revision moving forward and refreshes ids_used", "[terrain]") {
    VoxelVolume a;
    REQUIRE_FALSE(a.fill(ball_at(0.f, 0.f, 0.f, 5.f), 2));
    const std::uint64_t before = a.revision();
    REQUIRE((a.ids_used()[0] >> 2 & 1u) == 1u);

    VoxelVolume b;
    REQUIRE_FALSE(b.fill(ball_at(0.f, 0.f, 0.f, 5.f), 5));
    a = std::move(b);
    // Forward, never backward or repeated, so a poller watching a's revision
    // never mistakes this for "nothing changed".
    REQUIRE(a.revision() > before);
    REQUIRE((a.ids_used()[0] >> 5 & 1u) == 1u);
    REQUIRE((a.ids_used()[0] >> 2 & 1u) == 0u);   // a's old Id is gone with its old chunks_

    // Moving in a fresh, never-edited volume (as Terrain's clear does) must
    // not reset the revision back down to the fresh side's 0.
    const std::uint64_t before2 = a.revision();
    a = VoxelVolume{};
    REQUIRE(a.revision() > before2);
    REQUIRE(a.chunks().empty());
    REQUIRE(a.ids_used() == std::array<std::uint64_t, 4>{});
}

TEST_CASE("AV1 an .avox round-trips every chunk", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 9.f), 2));
    Shape block;
    block.kind = Shape::Kind::Block;
    block.frame = matrix4_translation(0.f, -40.f, 0.f);
    block.size = Vec3{64.f, 40.f, 64.f};   // whole solid chunks inside
    REQUIRE_FALSE(volume.fill(block, 5));
    const std::vector<std::byte> bytes = encode_avox(volume);
    VoxelVolume back;
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), back));
    REQUIRE(back.voxel_size() == 1.f);
    REQUIRE(back.chunks().size() == volume.chunks().size());
    for (const auto& [coord, chunk] : volume.chunks()) {
        const ChunkPtr& other = back.chunks().at(coord);
        REQUIRE(other->is_uniform() == chunk->is_uniform());
        for (int i = 0; i < kChunkCells; ++i) {
            REQUIRE(other->cell(i) == chunk->cell(i));
        }
    }
}

TEST_CASE("AV2 a damaged .avox is refused and leaves nothing", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 4.f), 1));
    const std::vector<std::byte> good = encode_avox(volume);
    VoxelVolume back;
    // A flipped byte inside a frame: zstd's checksum catches it.
    std::vector<std::byte> bytes = good;
    bytes[bytes.size() - 20] ^= std::byte{0x5a};
    REQUIRE(decode_avox(bytes.data(), bytes.size(), back).has_value());
    REQUIRE(back.chunks().empty());
    // A flipped byte in the index: the index CRC catches it.
    bytes = good;
    bytes[32 + 3] ^= std::byte{0x01};
    REQUIRE(decode_avox(bytes.data(), bytes.size(), back).has_value());
    // Cut short.
    REQUIRE(decode_avox(good.data(), 3, back).has_value());
    REQUIRE(decode_avox(good.data(), good.size() - 1, back).has_value());
}

TEST_CASE("AV3 the header says AVOX 1.0, VoxelSize, and 32, and an empty file is only a header", "[terrain]") {
    const std::vector<std::byte> bytes = encode_avox(VoxelVolume{});
    REQUIRE(bytes.size() == 32u);
    REQUIRE(static_cast<char>(bytes[0]) == 'A');
    REQUIRE(static_cast<char>(bytes[3]) == 'X');
    VoxelVolume back;
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), back));
    REQUIRE(back.chunks().empty());
}

TEST_CASE("AV4 a surface chunk compresses to a small fraction of its 64 KB", "[terrain]") {
    VoxelVolume volume;
    // A slope across one chunk: every cell in its band varies.
    Shape slope;
    slope.kind = Shape::Kind::Wedge;
    slope.frame = matrix4_translation(16.f, 16.f, 16.f);
    slope.size = Vec3{32.f, 32.f, 32.f};
    REQUIRE_FALSE(volume.fill(slope, 1));
    const ChunkPtr& chunk = volume.chunks().at(ChunkCoord{0, 0, 0});
    REQUIRE_FALSE(chunk->is_uniform());
    INFO(chunk->encoded().size());
    REQUIRE(chunk->encoded().size() < 8192u);
}

TEST_CASE("AV5 an unchanged chunk is not compressed again; a decoded one keeps its frame", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 9.f), 2));
    const ChunkPtr chunk = volume.chunks().at(ChunkCoord{0, 0, 0});
    const std::byte* first = chunk->encoded().data();
    (void)encode_avox(volume);
    REQUIRE(chunk->encoded().data() == first);   // the same cached frame
    const std::vector<std::byte> bytes = encode_avox(volume);
    VoxelVolume back;
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), back));
    REQUIRE(back.chunks().at(ChunkCoord{0, 0, 0})->encoded() == chunk->encoded());
}

TEST_CASE("AV6 one thread and many decode the same", "[terrain]") {
    VoxelVolume volume;
    for (int i = 0; i < 12; ++i) {
        REQUIRE_FALSE(volume.fill(ball_at(static_cast<float>(i * 40), 0.f, 0.f, 9.f), static_cast<std::uint8_t>(i + 1)));
    }
    const std::vector<std::byte> bytes = encode_avox(volume);
    VoxelVolume one;
    VoxelVolume many;
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), one, 1));
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), many, 8));
    REQUIRE(one.chunks().size() == many.chunks().size());
    for (const auto& [coord, chunk] : one.chunks()) {
        REQUIRE(many.chunks().at(coord)->encoded() == chunk->encoded());
    }
}

TEST_CASE("AV7 4,096 dense chunks decode in under a second", "[.][terrain-bench]") {
    VoxelVolume volume;
    // A 16 x 16 x 16-chunk block of rolling surface: every chunk dense.
    for (int cz = 0; cz < 16; ++cz) {
        for (int cx = 0; cx < 16; ++cx) {
            for (int cy = 0; cy < 16; ++cy) {
                REQUIRE_FALSE(volume.fill(ball_at(cx * 32.f + 16.f, cy * 32.f + 16.f, cz * 32.f + 16.f, 15.f),
                                          static_cast<std::uint8_t>(1 + (cx + cy + cz) % 4)));
            }
        }
    }
    REQUIRE(volume.chunks().size() >= 4096u);
    const std::vector<std::byte> bytes = encode_avox(volume);
    const auto start = std::chrono::steady_clock::now();
    VoxelVolume back;
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), back));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO(seconds << " s for " << bytes.size() << " bytes");
    REQUIRE(seconds < 1.0);
}

namespace {
// Mirrors AvoxFile.cpp's private little-endian writers and CRC-32, only so
// these two tests can hand-build or patch raw .avox bytes without reaching
// into AvoxFile.cpp's internals.
void put_u16(std::vector<std::byte>& bytes, std::size_t at, std::uint16_t v) {
    bytes[at] = std::byte{static_cast<std::uint8_t>(v & 0xffu)};
    bytes[at + 1] = std::byte{static_cast<std::uint8_t>((v >> 8) & 0xffu)};
}
void put_u32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<std::size_t>(i)] = std::byte{static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu)};
    }
}
void put_i32(std::vector<std::byte>& bytes, std::size_t at, std::int32_t v) {
    put_u32(bytes, at, static_cast<std::uint32_t>(v));
}
void put_u64(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        bytes[at + static_cast<std::size_t>(i)] = std::byte{static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu)};
    }
}
void put_f32(std::vector<std::byte>& bytes, std::size_t at, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(bytes, at, bits);
}
std::uint32_t test_crc32(const std::byte* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : (crc >> 1);
        }
    }
    return ~crc;
}
}  // namespace

TEST_CASE("AV8 a dense frame that decodes to all-air is dropped like a missing chunk", "[terrain]") {
    // A dense frame built directly (bypassing encode_avox's own uniform
    // check), whose cells are all air: a reader must drop this the same way
    // it drops a missing chunk, not keep a uniform-air entry around.
    std::vector<Cell> cells(static_cast<std::size_t>(kChunkCells), Cell{kAirDistance, 0});
    const std::vector<std::byte> frame = encode_chunk_frame(cells.data());

    std::vector<std::byte> bytes(32 + 32 + frame.size());
    put_i32(bytes, 32 + 0, 0);
    put_i32(bytes, 32 + 4, 0);
    put_i32(bytes, 32 + 8, 0);
    bytes[32 + 12] = std::byte{1};  // form: dense
    bytes[32 + 13] = std::byte{0};
    bytes[32 + 14] = std::byte{0};
    bytes[32 + 15] = std::byte{0};
    put_u64(bytes, 32 + 16, 64);
    put_u32(bytes, 32 + 24, static_cast<std::uint32_t>(frame.size()));
    put_u32(bytes, 32 + 28, 0);
    std::memcpy(bytes.data() + 64, frame.data(), frame.size());

    bytes[0] = std::byte{'A'};
    bytes[1] = std::byte{'V'};
    bytes[2] = std::byte{'O'};
    bytes[3] = std::byte{'X'};
    put_u16(bytes, 4, 1);
    put_u16(bytes, 6, 0);
    put_f32(bytes, 8, 1.f);
    put_u32(bytes, 12, static_cast<std::uint32_t>(kChunkSize));
    put_u32(bytes, 16, 1);
    put_u32(bytes, 20, test_crc32(bytes.data() + 32, 32));
    put_u64(bytes, 24, 0);

    VoxelVolume back;
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), back));
    REQUIRE(back.chunks().empty());
}

TEST_CASE("AV9 a duplicate or out-of-order index entry is refused", "[terrain]") {
    VoxelVolume volume;
    // Both balls stay well inside their own chunk's interior (center 16,
    // reach radius + the 4-cell band), so each is exactly one chunk: one at
    // ChunkCoord{0,0,0}, the other at ChunkCoord{6,0,0}.
    REQUIRE_FALSE(volume.fill(ball_at(16.f, 16.f, 16.f, 2.f), 1));
    REQUIRE_FALSE(volume.fill(ball_at(216.f, 16.f, 16.f, 2.f), 1));
    REQUIRE(volume.chunks().size() == 2u);
    std::vector<std::byte> bytes = encode_avox(volume);
    // Overwrite the second entry's coordinate with the first's: a duplicate,
    // not two strictly increasing (z, y, x) rows.
    std::memcpy(bytes.data() + 32 + 32, bytes.data() + 32, 12);
    // Recompute the index CRC, so the order/duplicate check is what rejects
    // this, not the CRC.
    put_u32(bytes, 20, test_crc32(bytes.data() + 32, 64));
    VoxelVolume back;
    REQUIRE(decode_avox(bytes.data(), bytes.size(), back).has_value());
}
