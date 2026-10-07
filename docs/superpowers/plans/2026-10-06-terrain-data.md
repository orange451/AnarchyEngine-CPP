# Terrain Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The data half of terrain core: `Terrain` and `TerrainMaterial` instances, sparse copy-on-write voxel chunks with shape edits, the `.avox` file, Play/Stop/undo/paste through place bytes, project save and load, and the full Lua API. Nothing is meshed or drawn yet (that is the terrain surface plan).

**Architecture:** Pure voxel code lives in `src/engine_core/terrain/` with no engine dependencies: `VoxelChunk` (one 32³ chunk, uniform or dense, immutable once shared), `ShapeDistance` (signed distance functions), `VoxelVolume` (the chunk map and every edit), `AvoxFile` (the file), `TerrainStash` (place-byte tokens). `Terrain` (a `PVInstance`) owns a `VoxelVolume`; `TerrainMaterial` children name which Material each Id draws as. Lua methods live in `src/engine_core/TerrainBindings.cpp`.

**Tech Stack:** C++20, Luau, Catch2 (`sandbox` target), CMake with the Visual Studio 2019 generator (multi-config), MSVC 14.23.

**Spec:** `docs/superpowers/specs/2026-10-06-terrain-core-design.md` (this plan covers its instances, voxels, place bytes, saving, and Lua API; meshing, physics, rendering, and the Configure Terrain tab are later plans).

## Global Constraints

- Terrain properties visible to users: `Transform: Matrix4`, `VoxelSize: number` (read-only, always 1), `CanCollide: boolean` (default true). Hidden saved property: `DataPath: string`.
- A Transform with scale or shear is refused: `Terrain cannot be scaled`.
- TerrainMaterial: `Id: number` read-only, 1–255, saved; `Material: Material?` saved; `Name` editable, starting as its Material's name. Several TerrainMaterials may share one Material.
- TerrainMaterial is never shown in Explorer, its Parent cannot be changed once set (`TerrainMaterial cannot be reparented`), `Instance.new("TerrainMaterial")` raises, and it may only be in a Terrain (`A TerrainMaterial must be in a Terrain`).
- At most 255 TerrainMaterials per Terrain: `Terrain can hold at most 255 Materials`. A new one takes the lowest Id no TerrainMaterial of that Terrain holds.
- Cell: int8 distance + uint8 material Id. Distance stored as `round(d / (4 × VoxelSize) × 127)`, clamped to ±127; negative inside. Id 0 is the default material; air cells (distance +127) store Id 0.
- Sample points: cell `(i, j, k)` samples Terrain-local `(i, j, k) × VoxelSize`.
- Chunk: 32³ cells, keyed by integer chunk coordinates; missing chunks are air. Uniform chunks hold one cell value; dense chunks hold 32,768 of each. A dense chunk that becomes uniform collapses; an all-air chunk is removed.
- Copy-on-write: chunk data is `std::shared_ptr<const ChunkData>`; an edit clones only the chunks it changes.
- Per-call limit: 16,777,216 cells. Error: `Terrain edit too large: split it into smaller calls`.
- Fill: `d = min(d, s)`; Id becomes the material's where `s < d_old` and `s < VoxelSize`. Subtract: `d = max(d, -s)`. Paint: Id becomes the material's where `s ≤ 0` and `d ≤ VoxelSize`.
- Voxel methods take a `TerrainMaterial` of this Terrain or `nil`. A Material raises `Pass a TerrainMaterial (see Terrain:GetMaterials)`; another Terrain's raises `TerrainMaterial belongs to another Terrain`.
- `ReadVoxels`/`WriteVoxels` use integer cell coordinates and carry material Ids as numbers; `WriteVoxels` accepts whole numbers 0–255 and raises otherwise.
- `.avox` layout exactly as the spec's "The `.avox` file" section; little-endian; only non-air chunks written; CRC-32 at the end.
- Only `src/engine_core/PhysicsWorld.cpp` includes Box3D; this plan includes none.
- Comments follow the codebase: plain English, say which thread. Project code builds warning-free at /W4.
- Commit messages: a plain imperative sentence, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target sandbox --parallel`. Run: `build/Debug/sandbox.exe "[terrain]"`.

## Review Focus

1. **A Terrain edited and never saved, then the project is saved.** Its voxels must reach disk and come back on reopen, with `DataPath` assigned during the save. Pinned in Task 7 (TS1).
2. **Stop after editing during play.** Voxels and TerrainMaterials added during play must be gone; voxels edited in edit mode before Play must survive. Pinned in Task 6 (TP1, TP2).
3. **Undo of deleting a Terrain.** The voxels come back. Pinned in Task 6 (TP3).
4. **A shape far outside an island, or a zero radius.** Must be a no-op, not a crash or a giant allocation. Pinned in Task 2 (V7) and Task 8 (TL6).
5. **Deleting a TerrainMaterial, adding another from a script, then undoing the delete.** No two TerrainMaterials may share an Id; the one holding it keeps it. Pinned in Task 5 (TM4, TM6, TM7).

## File Structure

| File | Responsibility |
| --- | --- |
| `src/engine_core/terrain/VoxelChunk.{hpp,cpp}` | Constants, `ChunkCoord`, `Cell`, quantize/dequantize, `ChunkData` |
| `src/engine_core/terrain/ShapeDistance.{hpp,cpp}` | `Shape` (ball, box, cylinder, wedge) and its signed distance and bounds in Terrain-local space |
| `src/engine_core/terrain/VoxelVolume.{hpp,cpp}` | Chunk map; fill, subtract, paint, replace, read, write, clear; dirty chunks; Ids in use |
| `src/engine_core/terrain/AvoxFile.{hpp,cpp}` | Encode/decode `.avox` bytes |
| `src/engine_core/terrain/TerrainStash.{hpp,cpp}` | Token → chunk-map snapshot for place bytes |
| `src/engine_instances/TerrainMaterial.{hpp,cpp}` | The TerrainMaterial instance |
| `src/engine_instances/Terrain.{hpp,cpp}` | The Terrain instance: properties, materials, edits in world or local space, place bytes, save and load |
| `src/engine_core/TerrainBindings.cpp` | Terrain's Lua methods |
| `sandbox/terrain_voxel_tests.cpp` | Tasks 1–3 |
| `sandbox/terrain_instance_tests.cpp` | Tasks 5–7 |
| `sandbox/terrain_lua_tests.cpp` | Tasks 8–9 |

---

### Task 1: Chunks and shape distances

**Files:**
- Create: `src/engine_core/terrain/VoxelChunk.hpp`, `src/engine_core/terrain/VoxelChunk.cpp`, `src/engine_core/terrain/ShapeDistance.hpp`, `src/engine_core/terrain/ShapeDistance.cpp`
- Create: `sandbox/terrain_voxel_tests.cpp`
- Modify: `CMakeLists.txt`: add the four sources to the `studio_core` source list beside `src/engine_core/PhysicsWorld.cpp` (around line 580), and the test file to `add_executable(sandbox ...)` (around line 862)

**Interfaces:**
- Produces (namespace `engine_core::terrain`):

```cpp
inline constexpr int kChunkSize = 32;
inline constexpr int kChunkCells = kChunkSize * kChunkSize * kChunkSize;
// The distance band either side of the surface, in cells.
inline constexpr float kBandCells = 4.f;
inline constexpr std::int8_t kAirDistance = 127;
inline constexpr std::int8_t kSolidDistance = -127;

struct ChunkCoord {
    int x = 0, y = 0, z = 0;
    bool operator==(const ChunkCoord& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct ChunkCoordHash { std::size_t operator()(const ChunkCoord& c) const; };

struct Cell {
    std::int8_t distance = kAirDistance;
    std::uint8_t material = 0;
    bool operator==(const Cell& o) const { return distance == o.distance && material == o.material; }
};

std::int8_t quantize(float studs, float voxel_size);
float dequantize(std::int8_t stored, float voxel_size);
// The chunk holding cell c, and c's index inside it (x fastest).
ChunkCoord chunk_of(int cx, int cy, int cz);
int cell_index(int lx, int ly, int lz);
// Air keeps no material: (127, m) becomes (127, 0).
Cell normalized(Cell cell);

class ChunkData {
public:
    static std::shared_ptr<const ChunkData> uniform(Cell value);
    static const std::shared_ptr<const ChunkData>& air();
    bool is_uniform() const { return uniform_; }
    Cell cell(int index) const;
    // A dense copy to edit before it is shared.
    std::shared_ptr<ChunkData> clone_dense() const;
    void set(int index, Cell value);   // only on a copy no one else holds
    // Recomputes the Id usage mask; uniform when every cell is equal.
    void finish();
    bool is_air() const { return uniform_ && value_.distance == kAirDistance; }
    // Bit i set when a solid or band cell uses Id i.
    const std::array<std::uint64_t, 4>& ids_used() const { return used_; }
private:
    bool uniform_ = true;
    Cell value_{};
    std::vector<std::int8_t> distances_;
    std::vector<std::uint8_t> materials_;
    std::array<std::uint64_t, 4> used_{};
};
using ChunkPtr = std::shared_ptr<const ChunkData>;
```

```cpp
// A shape in Terrain-local space. frame places a box, cylinder, or wedge
// (rigid); ball uses center.
struct Shape {
    enum class Kind { Ball, Block, Cylinder, Wedge };
    Kind kind = Kind::Ball;
    Vec3 center{};
    float radius = 0.f;
    Matrix4 frame = matrix4_identity();
    Vec3 size{};       // Block and Wedge: full size; Cylinder: (2r, height, 2r)
};
// Signed distance from local point p to the shape's surface, in studs; negative inside.
float shape_distance(const Shape& shape, Vec3 p);
// The shape's local-space bounds, grown by margin on every side.
void shape_bounds(const Shape& shape, float margin, Vec3& min, Vec3& max);
```

- [ ] **Step 1: Write the failing tests**

Create `sandbox/terrain_voxel_tests.cpp`:

```cpp
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
    REQUIRE(near(shape_distance(ball, Vec3{1.f, 2.f, 3.f}), -2.f, 1e-5f));
    REQUIRE(near(shape_distance(ball, Vec3{4.f, 2.f, 3.f}), 1.f, 1e-5f));

    Shape block;
    block.kind = Shape::Kind::Block;
    block.frame = matrix4_translation(0.f, 10.f, 0.f);
    block.size = Vec3{2.f, 4.f, 6.f};
    REQUIRE(near(shape_distance(block, Vec3{0.f, 10.f, 0.f}), -1.f, 1e-5f));
    REQUIRE(near(shape_distance(block, Vec3{0.f, 13.f, 0.f}), 1.f, 1e-5f));
    REQUIRE(near(shape_distance(block, Vec3{2.f, 13.f, 0.f}), std::sqrt(2.f), 1e-5f));

    Shape cylinder;
    cylinder.kind = Shape::Kind::Cylinder;
    cylinder.size = Vec3{2.f, 4.f, 2.f};   // radius 1, height 4, along Y
    REQUIRE(near(shape_distance(cylinder, Vec3{3.f, 0.f, 0.f}), 2.f, 1e-5f));
    REQUIRE(near(shape_distance(cylinder, Vec3{0.f, 5.f, 0.f}), 3.f, 1e-5f));
}

TEST_CASE("VC6 a wedge is solid under its slope and empty above it", "[terrain]") {
    Shape wedge;
    wedge.kind = Shape::Kind::Wedge;
    wedge.size = Vec3{4.f, 4.f, 4.f};
    // Its tall side is at +Z; it slopes down toward -Z.
    REQUIRE(shape_distance(wedge, Vec3{0.f, -1.5f, 1.5f}) < 0.f);
    REQUIRE(shape_distance(wedge, Vec3{0.f, 1.5f, -1.5f}) > 0.f);
    REQUIRE(shape_distance(wedge, Vec3{0.f, 5.f, 0.f}) > 0.f);
}

TEST_CASE("VC7 shape bounds cover the shape and its margin", "[terrain]") {
    Shape ball;
    ball.center = Vec3{10.f, 0.f, 0.f};
    ball.radius = 3.f;
    Vec3 min{}, max{};
    shape_bounds(ball, 4.f, min, max);
    REQUIRE(near(min.x, 3.f, 1e-5f));
    REQUIRE(near(max.x, 17.f, 1e-5f));

    Shape block;
    block.kind = Shape::Kind::Block;
    // Turned 90 degrees about Y: its 2-wide X becomes Z.
    block.frame = matrix4_rotation_y(1.5707964f);
    block.size = Vec3{2.f, 2.f, 8.f};
    shape_bounds(block, 0.f, min, max);
    REQUIRE(near(max.x, 4.f, 1e-4f));
    REQUIRE(near(max.z, 1.f, 1e-4f));
}
```

Use the codebase's own matrix helpers for `matrix4_translation` and a Y rotation (check `src/engine_datatypes/Matrix4.hpp` / `VectorMath.hpp` for the exact names, e.g. `matrix4_rotation_y` or an axis-angle builder) and adjust VC7 to them.

- [ ] **Step 2: Run to verify they fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target sandbox --parallel`
Expected: compile errors: `terrain/VoxelChunk.hpp` not found.

- [ ] **Step 3: Implement `VoxelChunk`**

`VoxelChunk.hpp` holds the declarations in Interfaces above, with this file comment:

```cpp
// One chunk of a Terrain's voxels: 32 cells on a side. Each cell is a signed
// distance to the surface (int8, ±4 cells wide, scaled by VoxelSize, negative
// inside) and a material Id (0 the default, 1-255 a TerrainMaterial's Id).
// A chunk is shared by pointer and never changed once shared: an edit clones
// it (clone_dense), changes the copy, and finishes it. Any thread may read a
// shared chunk.
```

`VoxelChunk.cpp`:

```cpp
#include "terrain/VoxelChunk.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::terrain {

std::size_t ChunkCoordHash::operator()(const ChunkCoord& c) const {
    std::size_t h = static_cast<std::size_t>(static_cast<std::uint32_t>(c.x)) * 73856093u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(c.y)) * 19349663u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(c.z)) * 83492791u;
    return h;
}

std::int8_t quantize(float studs, float voxel_size) {
    const float scaled = studs / (kBandCells * voxel_size) * 127.f;
    const float clamped = std::clamp(scaled, -127.f, 127.f);
    return static_cast<std::int8_t>(std::lround(clamped));
}

float dequantize(std::int8_t stored, float voxel_size) {
    return static_cast<float>(stored) / 127.f * kBandCells * voxel_size;
}

namespace {
int floor_div(int value, int by) { return value >= 0 ? value / by : -((-value + by - 1) / by); }
}  // namespace

ChunkCoord chunk_of(int cx, int cy, int cz) {
    return ChunkCoord{floor_div(cx, kChunkSize), floor_div(cy, kChunkSize), floor_div(cz, kChunkSize)};
}

int cell_index(int lx, int ly, int lz) { return lx + kChunkSize * (ly + kChunkSize * lz); }

Cell normalized(Cell cell) {
    if (cell.distance == kAirDistance) {
        cell.material = 0;
    }
    return cell;
}

std::shared_ptr<const ChunkData> ChunkData::uniform(Cell value) {
    auto chunk = std::make_shared<ChunkData>();
    chunk->value_ = normalized(value);
    chunk->finish();
    return chunk;
}

const std::shared_ptr<const ChunkData>& ChunkData::air() {
    static const std::shared_ptr<const ChunkData> empty = uniform(Cell{});
    return empty;
}

Cell ChunkData::cell(int index) const {
    if (uniform_) {
        return value_;
    }
    return Cell{distances_[static_cast<std::size_t>(index)], materials_[static_cast<std::size_t>(index)]};
}

std::shared_ptr<ChunkData> ChunkData::clone_dense() const {
    auto copy = std::make_shared<ChunkData>();
    copy->uniform_ = false;
    if (uniform_) {
        copy->distances_.assign(kChunkCells, value_.distance);
        copy->materials_.assign(kChunkCells, value_.material);
    } else {
        copy->distances_ = distances_;
        copy->materials_ = materials_;
    }
    return copy;
}

void ChunkData::set(int index, Cell value) {
    value = normalized(value);
    distances_[static_cast<std::size_t>(index)] = value.distance;
    materials_[static_cast<std::size_t>(index)] = value.material;
}

void ChunkData::finish() {
    used_ = {};
    if (uniform_) {
        if (value_.distance != kAirDistance) {
            used_[value_.material >> 6] |= 1ull << (value_.material & 63);
        }
        return;
    }
    bool same = true;
    const Cell first{distances_[0], materials_[0]};
    for (int i = 0; i < kChunkCells; ++i) {
        const Cell c{distances_[static_cast<std::size_t>(i)], materials_[static_cast<std::size_t>(i)]};
        if (c.distance != kAirDistance) {
            used_[c.material >> 6] |= 1ull << (c.material & 63);
        }
        same = same && c == first;
    }
    if (same) {
        uniform_ = true;
        value_ = first;
        distances_.clear();
        distances_.shrink_to_fit();
        materials_.clear();
        materials_.shrink_to_fit();
    }
}

}  // namespace engine_core::terrain
```

`ChunkData` needs a public default constructor for `std::make_shared` (or a private one with a friend factory); keep the members private as shown.

- [ ] **Step 4: Implement `ShapeDistance`**

```cpp
// ShapeDistance.cpp
#include "terrain/ShapeDistance.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::terrain {
namespace {

Vec3 to_frame(const Matrix4& inverse, Vec3 p) {
    const float* m = inverse.m;
    return Vec3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

float length3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

float box_distance(Vec3 q, Vec3 half) {
    const float dx = std::fabs(q.x) - half.x;
    const float dy = std::fabs(q.y) - half.y;
    const float dz = std::fabs(q.z) - half.z;
    const float outside = length3(std::max(dx, 0.f), std::max(dy, 0.f), std::max(dz, 0.f));
    const float inside = std::min(std::max(dx, std::max(dy, dz)), 0.f);
    return outside + inside;
}

}  // namespace

float shape_distance(const Shape& shape, Vec3 p) {
    if (shape.kind == Shape::Kind::Ball) {
        return length3(p.x - shape.center.x, p.y - shape.center.y, p.z - shape.center.z) - shape.radius;
    }
    const Vec3 q = to_frame(matrix4_inverse(shape.frame), p);
    const Vec3 half{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.z * 0.5f};
    switch (shape.kind) {
    case Shape::Kind::Block:
        return box_distance(q, half);
    case Shape::Kind::Cylinder: {
        const float radial = std::sqrt(q.x * q.x + q.z * q.z) - half.x;
        const float axial = std::fabs(q.y) - half.y;
        const float outside = std::sqrt(std::max(radial, 0.f) * std::max(radial, 0.f) +
                                        std::max(axial, 0.f) * std::max(axial, 0.f));
        return outside + std::min(std::max(radial, axial), 0.f);
    }
    case Shape::Kind::Wedge: {
        // Solid under the plane from the bottom front edge (y = -h, z = -d)
        // to the top back edge (y = +h, z = +d).
        const float nl = std::sqrt(half.z * half.z + half.y * half.y);
        const float plane = nl > 0.f ? (q.y * half.z - q.z * half.y) / nl : 0.f;
        return std::max(box_distance(q, half), plane);
    }
    default:
        return 0.f;
    }
}
```

`matrix4_inverse` is recomputed per call above for clarity; cache it: give `Shape` a `Matrix4 inverse` filled once by a `prepare(Shape&)` helper, or compute it in `VoxelVolume` before the cell loop and pass a prepared shape. The plan's `VoxelVolume` (Task 2) calls `prepare_shape(shape)` once per edit:

```cpp
// Fills in what shape_distance needs, once per edit.
void prepare_shape(Shape& shape);   // sets shape.inverse = matrix4_inverse(shape.frame)
```

Add `Matrix4 inverse = matrix4_identity();` to `Shape`, make `shape_distance` use `shape.inverse`, and call `prepare_shape` in tests before `shape_distance` (update VC5–VC7 accordingly).

`shape_bounds`: for a ball, `center ± (radius + margin)`. For the others, transform the 8 corners of the frame-space box `±half` (a cylinder's box is `±(r, h/2, r)`) by `shape.frame` and take the component min/max, then grow by `margin`.

- [ ] **Step 5: Run tests**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target sandbox --parallel` then `build/Debug/sandbox.exe "[terrain]"`
Expected: VC1–VC7 pass.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/terrain/VoxelChunk.hpp src/engine_core/terrain/VoxelChunk.cpp src/engine_core/terrain/ShapeDistance.hpp src/engine_core/terrain/ShapeDistance.cpp sandbox/terrain_voxel_tests.cpp CMakeLists.txt
git commit -m "Add terrain voxel chunks and shape distances"
```

---

### Task 2: The voxel volume

**Files:**
- Create: `src/engine_core/terrain/VoxelVolume.hpp`, `src/engine_core/terrain/VoxelVolume.cpp`
- Modify: `sandbox/terrain_voxel_tests.cpp`, `CMakeLists.txt` (add the source)

**Interfaces:**
- Consumes: Task 1.
- Produces:

```cpp
namespace engine_core::terrain {

struct CellCoord {
    int x = 0, y = 0, z = 0;
};

using ChunkMap = std::unordered_map<ChunkCoord, ChunkPtr, ChunkCoordHash>;

// A Terrain's voxels: every chunk that is not all air. SimulationThread only;
// chunks handed out are shared and immutable.
class VoxelVolume {
public:
    static constexpr std::int64_t kMaxCellsPerEdit = 16777216;

    explicit VoxelVolume(float voxel_size = 1.f) : voxel_size_(voxel_size) {}
    float voxel_size() const { return voxel_size_; }

    const ChunkMap& chunks() const { return chunks_; }
    // Replaces every chunk; chunks whose pointer changed, and their neighbors, become dirty.
    void set_chunks(ChunkMap chunks);
    Cell cell(CellCoord c) const;

    // Each returns why it did nothing (too large), else nullopt. shape is in
    // Terrain-local space; prepare_shape is called inside.
    std::optional<std::string> fill(Shape shape, std::uint8_t material);
    std::optional<std::string> subtract(Shape shape);
    std::optional<std::string> paint(Shape shape, std::uint8_t material);
    std::optional<std::string> replace(CellCoord min, CellCoord max, std::uint8_t from, std::uint8_t to);
    // Cells min..max inclusive, x fastest: distances in studs, Ids.
    std::optional<std::string> read(CellCoord min, CellCoord max, std::vector<float>& distances,
                                    std::vector<std::uint8_t>& materials) const;
    std::optional<std::string> write(CellCoord min, CellCoord max, const std::vector<float>& distances,
                                     const std::vector<std::uint8_t>& materials);
    void clear();

    // Ids that some solid or band cell uses; bit i of word i / 64.
    std::array<std::uint64_t, 4> ids_used() const;
    // Chunks changed since the last take_dirty, and every neighbor of each.
    void take_dirty(std::vector<ChunkCoord>& out);
    bool has_dirty() const { return !dirty_.empty(); }

private:
    float voxel_size_;
    ChunkMap chunks_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_;
};

}  // namespace engine_core::terrain
```

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/terrain_voxel_tests.cpp` (add `#include "terrain/VoxelVolume.hpp"`):

```cpp
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
    REQUIRE(volume.cell(CellCoord{6, 0, 0}).material == 3);   // band cell just outside: painted
    REQUIRE(volume.cell(CellCoord{20, 0, 0}).distance == kAirDistance);
    REQUIRE((volume.ids_used()[0] >> 3 & 1u) == 1u);
}

TEST_CASE("V2 subtracting what was filled leaves no chunks", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 1));
    REQUIRE_FALSE(volume.subtract(ball_at(0.f, 0.f, 0.f, 12.f)));
    REQUIRE(volume.chunks().empty());
    REQUIRE(volume.ids_used() == std::array<std::uint64_t, 4>{});
}

TEST_CASE("V3 an edit clones only the chunks it changes", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 3.f), 1));
    REQUIRE_FALSE(volume.fill(ball_at(100.f, 0.f, 0.f, 3.f), 1));
    const ChunkMap before = volume.chunks();
    REQUIRE_FALSE(volume.fill(ball_at(100.f, 0.f, 0.f, 2.f), 2));
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
```

(`#include <chrono>`.) V9 is hidden (`[.]`); run it in Release only.

- [ ] **Step 2: Run to verify they fail**

Expected: compile error, `terrain/VoxelVolume.hpp` not found.

- [ ] **Step 3: Implement**

The core of every shape edit, in `VoxelVolume.cpp`:

```cpp
namespace {

// The cells an edit of shape visits: its bounds grown by the band, in cells.
bool cell_box(const Shape& shape, float voxel_size, CellCoord& min, CellCoord& max, std::int64_t& count) {
    Vec3 lo{}, hi{};
    shape_bounds(shape, kBandCells * voxel_size, lo, hi);
    min = CellCoord{static_cast<int>(std::floor(lo.x / voxel_size)), static_cast<int>(std::floor(lo.y / voxel_size)),
                    static_cast<int>(std::floor(lo.z / voxel_size))};
    max = CellCoord{static_cast<int>(std::ceil(hi.x / voxel_size)), static_cast<int>(std::ceil(hi.y / voxel_size)),
                    static_cast<int>(std::ceil(hi.z / voxel_size))};
    count = static_cast<std::int64_t>(max.x - min.x + 1) * (max.y - min.y + 1) * (max.z - min.z + 1);
    return std::isfinite(lo.x) && std::isfinite(hi.x);
}

const char* kTooLarge = "Terrain edit too large: split it into smaller calls";

}  // namespace

// Runs change on every cell in min..max, chunk by chunk. change returns the
// new cell. A chunk is cloned only if some cell changes; one that ends all
// air is dropped.
template <typename Change>
void VoxelVolume::edit(CellCoord min, CellCoord max, Change change) {
    const ChunkCoord c0 = chunk_of(min.x, min.y, min.z);
    const ChunkCoord c1 = chunk_of(max.x, max.y, max.z);
    for (int cz = c0.z; cz <= c1.z; ++cz) {
        for (int cy = c0.y; cy <= c1.y; ++cy) {
            for (int cx = c0.x; cx <= c1.x; ++cx) {
                const ChunkCoord coord{cx, cy, cz};
                const auto found = chunks_.find(coord);
                const ChunkPtr& old = found != chunks_.end() ? found->second : ChunkData::air();
                std::shared_ptr<ChunkData> copy;
                const int x0 = std::max(min.x, cx * kChunkSize), x1 = std::min(max.x, cx * kChunkSize + kChunkSize - 1);
                const int y0 = std::max(min.y, cy * kChunkSize), y1 = std::min(max.y, cy * kChunkSize + kChunkSize - 1);
                const int z0 = std::max(min.z, cz * kChunkSize), z1 = std::min(max.z, cz * kChunkSize + kChunkSize - 1);
                for (int z = z0; z <= z1; ++z) {
                    for (int y = y0; y <= y1; ++y) {
                        for (int x = x0; x <= x1; ++x) {
                            const int index = cell_index(x - cx * kChunkSize, y - cy * kChunkSize, z - cz * kChunkSize);
                            const Cell before = (copy ? copy->cell(index) : old->cell(index));
                            const Cell after = normalized(change(x, y, z, before));
                            if (after == before) {
                                continue;
                            }
                            if (!copy) {
                                copy = old->clone_dense();
                            }
                            copy->set(index, after);
                        }
                    }
                }
                if (!copy) {
                    continue;
                }
                copy->finish();
                if (copy->is_air()) {
                    chunks_.erase(coord);
                } else {
                    chunks_[coord] = std::move(copy);
                }
                mark_dirty(coord);
            }
        }
    }
}
```

(`copy->cell(index)` on a dense copy reads the vector; declare `edit` and `mark_dirty` as private members in the header.) `mark_dirty(coord)` inserts the coord and its 26 neighbors into `dirty_`.

The operations, each beginning with the limit check:

```cpp
std::optional<std::string> VoxelVolume::fill(Shape shape, std::uint8_t material) {
    prepare_shape(shape);
    CellCoord min{}, max{};
    std::int64_t count = 0;
    if (!cell_box(shape, voxel_size_, min, max, count)) {
        return std::nullopt;
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    const float vs = voxel_size_;
    edit(min, max, [&](int x, int y, int z, Cell cell) {
        const float s = shape_distance(shape, Vec3{x * vs, y * vs, z * vs});
        const float old = dequantize(cell.distance, vs);
        const std::int8_t next = quantize(std::min(old, s), vs);
        Cell out{next, cell.material};
        if (s < old && s < vs) {
            out.material = material;
        }
        return out;
    });
    return std::nullopt;
}
```

- `subtract`: same frame; `out.distance = quantize(std::max(old, -s), vs)`; Id unchanged (normalized clears it if the cell became pure air).
- `paint`: `if (s <= 0.f && old <= vs) out.material = material;` distance unchanged.
- `replace(min, max, from, to)`: limit check on the box; `if (cell.distance != kAirDistance && cell.material == from) cell.material = to;`.
- `read`: limit check; for every cell min..max (x fastest) push `dequantize(cell.distance)` and `cell.material`.
- `write`: limit check; sizes must equal the box's cell count, else return `"distances and materials must each hold one value per cell"`; set each cell to `{quantize(d), id}` via `edit`.
- `clear`: mark every chunk dirty, then `chunks_.clear()`.
- `set_chunks(next)`: for every coord in either map whose pointer differs (missing counts as air), `mark_dirty(coord)`; then `chunks_ = std::move(next)`.
- `ids_used`: OR of every chunk's `ids_used()`.
- `take_dirty`: move `dirty_` into `out` (clearing `out` first), clear `dirty_`.
- A zero-radius ball: `shape_bounds` still gives a small box; every cell's `s >= 0`, so `min(old, s)` with old = +4 studs becomes `s`, which would create band cells. Guard: in `fill`, return `std::nullopt` without editing when the shape has no volume (ball radius ≤ 0, or any size component ≤ 0).

- [ ] **Step 4: Run tests**

Run: `build/Debug/sandbox.exe "[terrain]"`, then build Release and run `build/Release/sandbox.exe "[terrain-bench]"`.
Expected: V1–V8 pass; V9 passes in Release.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/terrain/VoxelVolume.hpp src/engine_core/terrain/VoxelVolume.cpp sandbox/terrain_voxel_tests.cpp CMakeLists.txt
git commit -m "Add the terrain voxel volume and its shape edits"
```

---

### Task 3: The `.avox` file

**Files:**
- Create: `src/engine_core/terrain/AvoxFile.hpp`, `src/engine_core/terrain/AvoxFile.cpp`
- Modify: `sandbox/terrain_voxel_tests.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces:

```cpp
namespace engine_core::terrain {
// The bytes of an .avox file holding volume's chunks.
std::vector<std::byte> encode_avox(const VoxelVolume& volume);
// Reads bytes into out. Returns why they are not a valid .avox file; out is
// then left empty.
std::optional<std::string> decode_avox(const std::byte* data, std::size_t size, VoxelVolume& out);
}
```

- [ ] **Step 1: Write the failing tests**

```cpp
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
        for (int i = 0; i < kChunkCells; i += 97) {
            REQUIRE(other->cell(i) == chunk->cell(i));
        }
    }
}

TEST_CASE("AV2 a damaged .avox is refused and leaves nothing", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 4.f), 1));
    std::vector<std::byte> bytes = encode_avox(volume);
    bytes[bytes.size() / 2] ^= std::byte{0x5a};
    VoxelVolume back;
    REQUIRE(decode_avox(bytes.data(), bytes.size(), back).has_value());
    REQUIRE(back.chunks().empty());
    REQUIRE(decode_avox(bytes.data(), 3, back).has_value());
}

TEST_CASE("AV3 the header says AVOX 1.0, VoxelSize, and 32", "[terrain]") {
    const std::vector<std::byte> bytes = encode_avox(VoxelVolume{});
    REQUIRE(bytes.size() == 4 + 2 + 2 + 4 + 4 + 4 + 4);
    REQUIRE(static_cast<char>(bytes[0]) == 'A');
    REQUIRE(static_cast<char>(bytes[3]) == 'X');
}
```

- [ ] **Step 2: Run to verify they fail** (compile error: header missing).

- [ ] **Step 3: Implement**

Write little-endian helpers (`put_u16`, `put_u32`, `put_i32`, `put_f32` with `std::memcpy` into a 4-byte buffer; the engine only targets little-endian x64/arm64, so a static_assert on `std::endian::native == std::endian::little` is not available in C++20 on MSVC 14.23 — write bytes explicitly by shifting). Layout exactly as the spec:

```
"AVOX" u16 1 u16 0 f32 voxel_size u32 32 u32 chunk_count
per chunk: i32 x, y, z; u8 form (0 uniform, 1 dense)
  uniform: i8 distance, u8 id
  dense:   u32 byte_length, then (u16 run, i8 distance, u8 id) triples covering 32,768 cells x-fastest
u32 crc32 of everything before it
```

CRC-32: check whether the codebase already has one (`grep -rn crc32 src/amesh`): AMESH has a CRC; reuse its function if it is exported, else write the standard reflected 0xEDB88320 table version in `AvoxFile.cpp`. Decoding checks magic, version major 1, chunk_size 32, voxel_size finite and > 0, every length against the remaining bytes, run totals exactly 32,768, and the CRC; any failure clears `out` and returns a reason such as `"not an .avox file"`, `"damaged .avox file"`. Decoded dense chunks call `finish()`; air chunks are skipped. `out` is replaced with a new `VoxelVolume(voxel_size)` and filled via `set_chunks`.

- [ ] **Step 4: Run tests**; **Step 5: Commit** (`Add the .avox terrain file format`).

---

### Task 4: Engine support: paste-only creatables and locked parents

**Files:**
- Modify: `src/engine_core/LuaApi.hpp` (`register_lua_creatable`, ~line 299-306), `src/engine_core/LuaApi.cpp` (~392-417)
- Modify: `src/engine_core/ScriptBindings.cpp` (`instance_new`, ~264-297)
- Modify: `src/engine_core/DataModel.hpp` (virtuals ~140-193), `src/engine_core/DataModel.cpp` (`parent_error`, ~1681-1736)
- Test: `sandbox/terrain_instance_tests.cpp` (create)

**Interfaces:**
- Produces:
  - `void register_lua_creatable(const char* class_name, LuaCreate create, bool from_scripts = true);` A class registered with `from_scripts = false` is made by paste, duplicate, and undo (`lua_create_instance`) but refused by `Instance.new` with `"<Class> cannot be made with Instance.new"` and left out of `lua_creatable_names()` (the Insert popup).
  - `bool lua_script_creatable(const char* class_name);`
  - `virtual bool parent_locked() const { return false; }` on `DataModel`: when true, `parent_error` refuses any change of a parent that is not `kNoParent`: `"<Class> cannot be reparented"`. Moving from `kNoParent` (load, paste, undo revive) and `destroy` are unaffected.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/terrain_instance_tests.cpp` with a local test class registered only in the test, so this task does not depend on Task 5:

```cpp
// Terrain and TerrainMaterial instances, and the engine rules they rely on.

#include "support.hpp"

#include "DataModel.hpp"
#include "Folder.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::InstanceId;

// A Folder that keeps its parent, for the locked-parent rule.
class LockedFolder : public engine_core::Folder {
public:
    using Folder::Folder;
    bool parent_locked() const override { return true; }
};

std::string reason(const std::optional<std::string>& value) { return value ? *value : std::string(); }

}  // namespace

TEST_CASE("TE1 a locked parent refuses to change, but can be set from none", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    LockedFolder& locked = game.create<LockedFolder>();
    REQUIRE_FALSE(game.parent_error(locked.id(), workspace_of(game)));
    game.set_parent(locked.id(), workspace_of(game));
    engine_core::Folder& other = game.create<engine_core::Folder>();
    game.set_parent(other.id(), workspace_of(game));
    REQUIRE(reason(game.parent_error(locked.id(), other.id())) == "Folder cannot be reparented");
    REQUIRE(reason(game.parent_error(locked.id(), engine_core::DataModel::kNoParent)) == "Folder cannot be reparented");
    game.destroy(locked.id());
}
```

(`Folder`'s class_name is "Folder"; the message uses `class_name()`.) For the creatable flag, add a test after Task 5 exists (TM6 below covers `Instance.new("TerrainMaterial")`); here test the registry directly:

```cpp
TEST_CASE("TE2 a paste-only creatable is made by lua_create_instance but not by scripts", "[terrain]") {
    engine_core::register_lua_creatable(
        "TestPasteOnly", [](engine_core::DataModel& world) -> engine_core::DataModel& {
            return world.create<engine_core::Folder>();
        }, false);
    REQUIRE(engine_core::lua_creatable_known("TestPasteOnly"));
    REQUIRE_FALSE(engine_core::lua_script_creatable("TestPasteOnly"));
    REQUIRE(engine_core::lua_script_creatable("Folder"));
    const auto names = engine_core::lua_creatable_names();
    REQUIRE(std::find(names.begin(), names.end(), std::string("TestPasteOnly")) == names.end());
}
```

Check `lua_creatable_names()`'s actual return type and adapt the find.

- [ ] **Step 2: Run to verify they fail** (no `parent_locked`, no third argument).

- [ ] **Step 3: Implement**

`DataModel.hpp`, beside `hidden_in_explorer`:

```cpp
    // True for an instance that stays under the parent it was first given,
    // as a TerrainMaterial stays in its Terrain. Load, paste, and undo set
    // its parent from none; destroy still takes it out.
    virtual bool parent_locked() const { return false; }
```

`DataModel.cpp` `parent_error`, after the service and Core checks and before the `new_parent == kNoParent` early return:

```cpp
    if (const DataModel* object = instance(id);
        object != nullptr && object->parent_locked() && parent(id) != kNoParent && parent(id) != new_parent) {
        return std::string(object->class_name()) + " cannot be reparented";
    }
```

`LuaApi`: add `bool from_scripts = true;` to the creatable record, the third parameter, `lua_script_creatable`, and filter `lua_creatable_names()` by `from_scripts`.

`ScriptBindings.cpp` `instance_new`: before `lua_creatable_known(name)` handling, add

```cpp
            if (lua_creatable_known(name) && !lua_script_creatable(name)) {
                luaL_error(state, "%s cannot be made with Instance.new", name);
            }
```

- [ ] **Step 4: Run tests** (`[terrain]` and the full suite: `build/Debug/sandbox.exe`). **Step 5: Commit** (`Let a class keep its parent, and be made by paste but not by scripts`).

---

### Task 5: The `Terrain` and `TerrainMaterial` instances

**Files:**
- Create: `src/engine_instances/TerrainMaterial.{hpp,cpp}`, `src/engine_instances/Terrain.{hpp,cpp}`
- Modify: `src/engine_core/ScriptBindings.cpp` (factories and `register_lua_creatable`, ~201, ~241)
- Modify: `src/engine_core/Project.cpp` (`class_registry`, ~150)
- Modify: `src/engine_core/Containment.cpp` (`placement_error`, near the Prefab/Model rule ~121)
- Modify: `src/engine_core/LuaApi.cpp` (property docs, near ~1055)
- Modify: `src/engine_instances/README.md`
- Modify: `CMakeLists.txt` (sources)
- Test: `sandbox/terrain_instance_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1–4 (`VoxelVolume`, `parent_locked`, `register_lua_creatable(..., false)`).
- Produces:

```cpp
// TerrainMaterial.hpp
class TerrainMaterial : public DataModel {
public:
    static constexpr int kMaxId = 255;
    TerrainMaterial(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "TerrainMaterial"; }
    bool hidden_in_explorer() const override { return true; }
    bool parent_locked() const override { return true; }
    int material_id() const { return id_; }
    // Load, paste, and Terrain::add_material only; scripts read Id.
    std::optional<std::string> set_material_id(int value);
    LuaSlot material() const;                       // Material? slot
    InstanceId material_instance() const;           // 0 when none or gone
    std::optional<std::string> set_material(const LuaSlot& value);
protected:
    void on_reuse() override;
    void on_parent_changed(InstanceId previous, InstanceId next) override;
private:
    int id_ = 0;
    InstanceRef material_;
};
```

```cpp
// Terrain.hpp
class Terrain : public PVInstance {
public:
    Terrain(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override { return "Terrain"; }

    Matrix4 transform() const override { return transform_; }
    std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override { return set_transform(transform); }
    double voxel_size() const { return volume_.voxel_size(); }
    bool can_collide() const { return can_collide_; }
    std::optional<std::string> set_can_collide(bool value);
    const std::string& data_path() const { return data_path_; }

    // The TerrainMaterial children, by Id.
    std::vector<TerrainMaterial*> materials() const;
    TerrainMaterial* material_by_id(int id) const;
    std::vector<TerrainMaterial*> materials_for(InstanceId material) const;
    // Makes one on the lowest free Id, named after material (or "TerrainMaterial").
    std::optional<std::string> add_material(InstanceId material, TerrainMaterial*& out);
    // The lowest Id no TerrainMaterial child holds; 0 when all 255 are taken.
    int free_id() const;

    terrain::VoxelVolume& volume() { return volume_; }
    const terrain::VoxelVolume& volume() const { return volume_; }

protected:
    void on_reuse() override;

private:
    Matrix4 transform_ = matrix4_identity();
    bool can_collide_ = true;
    std::string data_path_;
    terrain::VoxelVolume volume_;
};
```

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/terrain_instance_tests.cpp` (add `#include "Terrain.hpp"`, `"TerrainMaterial.hpp"`, `"AssetInstances.hpp"`, `"Containment.hpp"`, `"Project.hpp"`):

```cpp
namespace {

engine_core::Terrain& add_terrain(engine_core::DataModel& game) {
    engine_core::Terrain& terrain = game.create<engine_core::Terrain>();
    game.set_parent(terrain.id(), workspace_of(game));
    return terrain;
}

engine_core::Material& add_material_asset(engine_core::DataModel& game, const char* name) {
    engine_core::Material& material = game.create<engine_core::Material>();
    game.set_name(material.id(), name);
    game.set_parent(material.id(), game.scene_service("Assets"));   // check the Assets service's name
    return material;
}

}  // namespace

TEST_CASE("TM1 a Terrain's properties are checked and saved", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("Terrain"));
    REQUIRE(engine_core::project_class_known("TerrainMaterial"));
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE(terrain.voxel_size() == 1.0);
    REQUIRE(terrain.can_collide());
    REQUIRE_FALSE(terrain.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f)));
    Matrix4 scaled = engine_core::matrix4_identity();
    scaled.m[0] = 2.f;
    REQUIRE(*terrain.set_transform(scaled) == "Terrain cannot be scaled");
    REQUIRE_FALSE(terrain.set_can_collide(false));
    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    REQUIRE(saved.has("Transform"));   // use PropertyBag's real lookup
    REQUIRE(saved.has("CanCollide"));
}

TEST_CASE("TM2 TerrainMaterials take the lowest free Id, up to 255", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::Material& rock = add_material_asset(game, "Rock");
    engine_core::TerrainMaterial* a = nullptr;
    engine_core::TerrainMaterial* b = nullptr;
    REQUIRE_FALSE(terrain.add_material(rock.id(), a));
    REQUIRE_FALSE(terrain.add_material(rock.id(), b));   // the same Material twice is fine
    REQUIRE(a->material_id() == 1);
    REQUIRE(b->material_id() == 2);
    REQUIRE(game.name(a->id()) == "Rock");
    REQUIRE(terrain.materials_for(rock.id()).size() == 2u);
    game.destroy(a->id());
    engine_core::TerrainMaterial* c = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, c));
    REQUIRE(c->material_id() == 1);
    for (int i = 0; i < 253; ++i) {
        engine_core::TerrainMaterial* more = nullptr;
        REQUIRE_FALSE(terrain.add_material(0, more));
    }
    engine_core::TerrainMaterial* full = nullptr;
    REQUIRE(*terrain.add_material(0, full) == "Terrain can hold at most 255 Materials");
}

TEST_CASE("TM3 a TerrainMaterial lives only in a Terrain, hidden, and keeps its parent", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, entry));
    REQUIRE(entry->hidden_in_explorer());
    REQUIRE(reason(engine_core::placement_error("Workspace", "TerrainMaterial", "x")) ==
            "A TerrainMaterial must be in a Terrain");
    engine_core::Terrain& other = add_terrain(game);
    REQUIRE(reason(game.parent_error(entry->id(), other.id())) == "TerrainMaterial cannot be reparented");
}

TEST_CASE("TM4 deleting a TerrainMaterial keeps its cells' Id for the next one", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, entry));
    engine_core::terrain::Shape ball;
    ball.radius = 4.f;
    REQUIRE_FALSE(terrain.volume().fill(ball, static_cast<std::uint8_t>(entry->material_id())));
    game.destroy(entry->id());
    REQUIRE(terrain.volume().cell(engine_core::terrain::CellCoord{0, 0, 0}).material == 1);
    REQUIRE(terrain.material_by_id(1) == nullptr);
    engine_core::TerrainMaterial* next = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, next));
    REQUIRE(terrain.material_by_id(1) == next);
}

TEST_CASE("TM5 Id is read-only to scripts and Material refuses a non-Material", "[terrain]") {
    ScriptRig rig;
    add_script(rig.game, "T", R"(
        local t = Instance.new("Terrain", workspace)
        _G.made = pcall(function() Instance.new("TerrainMaterial", t) end)
        _G.ok = (not _G.made)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
}
```

```cpp
TEST_CASE("TM6 undoing a delete after a script took the Id gives the revived one a new Id", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* a = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, a));
    const InstanceId a_id = a->id();
    begin_step(game, "Delete");
    game.destroy(a_id);
    end_step(game);
    // Not recorded: a script in the command bar, outside any step.
    engine_core::TerrainMaterial* b = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, b));
    REQUIRE(b->material_id() == 1);
    game.history().undo();
    auto* back = dynamic_cast<engine_core::TerrainMaterial*>(game.instance(a_id));
    REQUIRE(back != nullptr);
    REQUIRE(b->material_id() == 1);       // the holder keeps it
    REQUIRE(back->material_id() == 2);    // the revived one moves
    REQUIRE(terrain.materials().size() == 2u);
}

TEST_CASE("TM7 a TerrainMaterial pasted into a Terrain that uses its Id gets the lowest free one", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* a = nullptr;
    engine_core::TerrainMaterial* b = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, a));
    REQUIRE_FALSE(terrain.add_material(0, b));
    // What paste does: make one, load its saved properties (Id 1), then parent it.
    engine_core::TerrainMaterial& copy = game.create<engine_core::TerrainMaterial>();
    REQUIRE_FALSE(copy.set_material_id(1));
    game.set_parent(copy.id(), terrain.id());
    REQUIRE(a->material_id() == 1);
    REQUIRE(copy.material_id() == 3);
}
```

(Fix up `PropertyBag` lookups and the Assets service name from `sandbox/bloom_tests.cpp` and `AssetInstances.cpp`. TM5 checks `Instance.new("TerrainMaterial")` raises; the Id read-only check lands with `AddMaterial` in Task 9.)

- [ ] **Step 2: Run to verify they fail** (headers missing).

- [ ] **Step 3: Implement `TerrainMaterial`**

Follow `BloomEffect.cpp` for the shape of setters, `require_thread`, `refuse`, and the `ANARCHY_LUA_REGISTER` block. Properties:

```cpp
ANARCHY_LUA_REGISTER(register_terrain_material_lua) {
    LuaField id = lua_saved_property("Id", "number", read_id, write_id, "0");
    id.writable = false;   // scripts read it; load, Stop, undo, and paste still write it
    const LuaField fields[] = {
        id,
        lua_saved_property("Material", "Material?", read_material, write_material, "null"),
    };
    register_lua_class("TerrainMaterial", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("TerrainMaterial", {"Terrain"});
}
```

`set_material_id`: refuses anything outside 1–255 with `"Id must be a whole number from 1 to 255"`; records with `note_property_change("Id", ...)`. `set_material`: `set_instance_reference("Material", "Material", material_, value)`; when the TerrainMaterial's Name is still its old Material's name (or "TerrainMaterial"), rename it after the new one (as `set_model_part` renames a Model after its Mesh, `src/ide/PrefabModels.cpp:184-214`).

**The Id rule: whoever already holds an Id keeps it.** Every path that brings a TerrainMaterial into a Terrain sets its Id first and its parent second: load (`Project.cpp` `build`), paste and duplicate (`CutSet.cpp` `build_copy`), and undo of a delete (`revive_record` then `history_move`). So `on_parent_changed(previous, next)` is the one place uniqueness is enforced: when `next` is a Terrain and this TerrainMaterial's Id is 0, or another TerrainMaterial child of `next` already holds it, it takes `next`'s `free_id()`. The one already there never changes, so existing voxels never change what they look like. The reassignment is not an undo step of its own (it can happen inside an undo): write `id_` directly, `emit_property("Id")`, and `note_unrecorded_edit(id())`. If `free_id()` is 0 (all 255 taken), the newcomer keeps Id 0 and draws nothing; `materials()` skips Id 0. Stop's restore does not reparent through `set_parent` and needs no check: it puts back the exact snapshot taken at Play.

- [ ] **Step 4: Implement `Terrain`**

Transform follows `Dragger.cpp:46-60` and `:121-133`, saved with `lua_saved_property("Transform", "Matrix4", ...)`, plus the scale check:

```cpp
namespace {
// True when the matrix's axes are unit length and at right angles.
bool rigid(const Matrix4& m) {
    const float* a = m.m;
    auto dot = [&](int i, int j) { return a[i] * a[j] + a[i + 1] * a[j + 1] + a[i + 2] * a[j + 2]; };
    constexpr float tolerance = 1e-3f;
    return std::fabs(dot(0, 0) - 1.f) < tolerance && std::fabs(dot(4, 4) - 1.f) < tolerance &&
           std::fabs(dot(8, 8) - 1.f) < tolerance && std::fabs(dot(0, 4)) < tolerance &&
           std::fabs(dot(0, 8)) < tolerance && std::fabs(dot(4, 8)) < tolerance;
}
}  // namespace
```

`set_transform` refuses a non-finite matrix (`"Transform must be finite"`) and a non-rigid one (`"Terrain cannot be scaled"`).

Registration:

```cpp
    LuaField data_path = lua_hidden(lua_saved_property("DataPath", "string", read_data_path, write_data_path, "\"\""));
    data_path.writable = false;
    const LuaField fields[] = {
        lua_saved_property("Transform", "Matrix4", read_transform, write_transform, identity.c_str()),
        lua_property("VoxelSize", "number", false, read_voxel_size, nullptr),
        lua_saved_property("CanCollide", "boolean", read_can_collide, write_can_collide, "true"),
        data_path,
    };
    register_lua_class("Terrain", "PVInstance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Terrain", {"Workspace"});
```

`write_data_path` stores the string (Task 7 makes it load the file). `materials()` walks children (`first_child`/`next_sibling`), keeps `TerrainMaterial`s with Id ≥ 1, sorts by Id. `add_material`: `free_id()` 0 → the 255 error; else `create<TerrainMaterial>()`, `set_name` (Material's name or `"TerrainMaterial"`), `set_material_id`, set the Material reference, `set_parent(id, this->id())`. `on_reuse` resets every field and `volume_ = terrain::VoxelVolume{}`.

- [ ] **Step 5: Register**

- `ScriptBindings.cpp`: `DataModel& create_terrain(DataModel& world) { return world.create<Terrain>(); }`, the same for TerrainMaterial; `register_lua_creatable("Terrain", create_terrain);` and `register_lua_creatable("TerrainMaterial", create_terrain_material, false);`.
- `Project.cpp` `class_registry()`: both classes.
- `Containment.cpp` `placement_error`, beside the Prefab rule:

```cpp
    if (child_class == "TerrainMaterial" && holder_class != "Terrain") {
        return std::string("A TerrainMaterial must be in a Terrain");
    }
```

- `LuaApi.cpp` docs, e.g. `add("Terrain", "VoxelSize", "The size of one cell, in studs. Read-only; always 1.", "number", false, {});`, plus `Transform`, `CanCollide`, `TerrainMaterial.Id`, `TerrainMaterial.Material`.
- `src/engine_instances/README.md`: add both names to line 3's list and one paragraph each, in the file's style.

- [ ] **Step 6: Run tests** (`[terrain]`, then the full suite — the docs and analysis tests check every registered member is documented). **Step 7: Commit** (`Add the Terrain and TerrainMaterial instances`).

---

### Task 6: Place bytes: Play/Stop, undo, and paste keep voxels

**Files:**
- Create: `src/engine_core/terrain/TerrainStash.{hpp,cpp}`
- Modify: `src/engine_instances/Terrain.{hpp,cpp}` (`write_place`, `read_place`, `write_data_path`)
- Modify: `src/engine_core/Project.cpp` (`Rebuild::finish`, ~1088)
- Test: `sandbox/terrain_instance_tests.cpp`

**Interfaces:**
- Produces:

```cpp
namespace engine_core::terrain {
// Chunk maps named by a token, so place bytes (Play's capture, undo records)
// carry a Terrain's voxels without copying them: chunks are shared and
// immutable. SimulationThread.
class TerrainStash {
public:
    static std::uint64_t put(ChunkMap chunks, float voxel_size);
    // False when the token is unknown (cleared).
    static bool get(std::uint64_t token, ChunkMap& chunks, float& voxel_size);
    // Drops every entry; the project's place was rebuilt.
    static void clear();
};
}
```

  - `Terrain::write_place`: `[u32 base_length][base bytes from DataModel::write_place][u64 token]`.
  - `Terrain::read_place`: base bytes into `DataModel::read_place`, then `volume_.set_chunks(...)` from the token when known.
  - `Terrain` remembers the last token written while the place was stopped (`authored_token_`), for saving during play (Task 7).

- [ ] **Step 1: Write the failing tests**

```cpp
namespace {
engine_core::terrain::Shape ball_at(float x, float y, float z, float r) {
    engine_core::terrain::Shape s;
    s.center = engine_core::Vec3{x, y, z};
    s.radius = r;
    return s;
}
std::uint8_t id_at(const engine_core::Terrain& terrain, int x, int y, int z) {
    return terrain.volume().cell(engine_core::terrain::CellCoord{x, y, z}).material;
}
}  // namespace

TEST_CASE("TP1 Stop puts back the voxels edited during play", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 1));
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(terrain.volume().fill(ball_at(50.f, 0.f, 0.f, 4.f), 2));
    REQUIRE_FALSE(terrain.volume().subtract(ball_at(0.f, 0.f, 0.f, 10.f)));
    game.stop_simulation();
    REQUIRE(id_at(terrain, 0, 0, 0) == 1);
    REQUIRE(terrain.volume().cell(engine_core::terrain::CellCoord{50, 0, 0}).distance ==
            engine_core::terrain::kAirDistance);
}

TEST_CASE("TP2 TerrainMaterials added during play are gone after Stop", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* kept = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, kept));
    game.capture_place();
    game.start_simulation();
    engine_core::TerrainMaterial* temp = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, temp));
    game.stop_simulation();
    REQUIRE(terrain.materials().size() == 1u);
}

TEST_CASE("TP3 undoing a Terrain's delete brings its voxels back", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 3));
    const InstanceId id = terrain.id();
    begin_step(game, "Delete");
    game.destroy(id);
    end_step(game);
    game.history().undo();
    auto* back = dynamic_cast<engine_core::Terrain*>(game.instance(id));
    REQUIRE(back != nullptr);
    REQUIRE(id_at(*back, 0, 0, 0) == 3);
}

TEST_CASE("TP4 a pasted Terrain starts with its source's voxels and its own file", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 2));
    terrain.assign_data_path_for_test("terrain/Source.avox");   // see Step 3
    // Paste builds from saved properties (src/ide/CutSet.cpp build_copy).
    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    engine_core::Terrain& copy = game.create<engine_core::Terrain>();
    // load each saved property the way build_copy does (load_property per member)
    // ... then:
    REQUIRE(id_at(copy, 0, 0, 0) == 2);
    REQUIRE_FALSE(copy.data_path().empty());
    REQUIRE(copy.data_path() != terrain.data_path());   // its own file, assigned at paste
}
```

Write TP4's property loading loop exactly as `build_copy` in `src/ide/CutSet.cpp:164-189` does (`for (member : node.properties) made->load_property(key, value, error)`), using `PropertyBag`'s iteration API.

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement**

`TerrainStash.cpp`: a function-local `static std::unordered_map<std::uint64_t, Entry>` and a counter starting at 1, guarded by a `std::mutex` (place bytes are written on SimulationThread, but tests and tools may not be; the lock is cheap).

`Terrain::write_place`:

```cpp
void Terrain::write_place(std::vector<std::byte>& out) const {
    std::vector<std::byte> base;
    DataModel::write_place(base);
    const std::uint32_t length = static_cast<std::uint32_t>(base.size());
    const auto* l = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), l, l + sizeof(length));
    out.insert(out.end(), base.begin(), base.end());
    const std::uint64_t token = terrain::TerrainStash::put(volume_.chunks(), volume_.voxel_size());
    if (!simulation_running()) {
        authored_token_ = token;   // mutable
    }
    const auto* t = reinterpret_cast<const std::byte*>(&token);
    out.insert(out.end(), t, t + sizeof(token));
}
```

`read_place` reverses it; an unknown or missing token leaves the voxels as they are. Use the DataModel accessor for "is the simulation running" that exists (`state` flag via `DataModel::simulation_running()` on the world; inside an instance call the world through `world()` or the protected accessor the codebase uses — check how `Mesh` reads `world_generation()` in `AssetInstances.cpp:116`).

Paste: `write_data_path(value)`: if another live Terrain in this DataModel has `data_path() == value` (walk Workspace's descendants, or keep a small registry of live Terrains keyed by id in the stash), copy its `volume().chunks()` into this one (`set_chunks`) and leave this Terrain's `data_path_` empty so it gets its own file at the next save. Otherwise store the path (Task 7 adds reading the file). Give `Terrain` a `void assign_data_path_for_test(std::string)` only if no cleaner seam exists; prefer setting it through the `DataPath` registry write with a LuaSlot string, which is what load does.

`Project.cpp` `Rebuild::finish`: call `terrain::TerrainStash::clear();` before `capture_place`.

- [ ] **Step 4: Run tests**; **Step 5: Commit** (`Carry a Terrain's voxels through Play, undo, and paste without copying them`).

---

### Task 7: Save and load the `.avox` with the project

**Files:**
- Modify: `src/engine_core/DataModel.hpp` (new virtual, warning sink), `src/engine_core/DataModel.cpp`, `src/engine_core/DataModelState.hpp`, `src/engine_core/Engine.cpp` (wire the sink), `src/engine_core/Project.cpp` (`save_tree` ~2684, before `plan_files`)
- Modify: `src/engine_instances/Terrain.{hpp,cpp}`
- Test: `sandbox/terrain_instance_tests.cpp`

**Interfaces:**
- Produces:
  - `virtual std::optional<std::string> save_resources(const std::filesystem::path& root);` on `DataModel` (default: nothing, `nullopt`). `Project::save` calls it on every authored instance before building the JSON, so a property it changes (DataPath) is saved in the same save. A returned reason fails the save the way a file write failure does.
  - **A Terrain gets its DataPath the moment it first has authored voxels**, never during a save: on its first voxel edit while stopped (in `edit_volume`), when paste gives it a source's voxels (Task 6's `write_data_path`), and when a damaged file is found at load (below). The path is `terrain/<sanitize_file_name(Name)>.<guid>.avox`, set without an undo step (`emit_property("DataPath")` plus `note_unrecorded_edit(id())`). So DataPath is in every snapshot like any other property, and Task 6's paste test (TP4) expects the copy's DataPath to be its own new path rather than empty.
  - `Terrain::save_resources`: writes `encode_avox` of the authored voxels to `DataPath` when they changed since the last save or the file is missing, through the same `.partial`-then-rename writer `Mesh` uses (`write_file` in `AssetInstances.cpp:144`). It never assigns a path. Stopped, the authored voxels are the live ones. **During play it is no different from any other instance's save: it writes what the snapshot holds** — the voxels named by `authored_token_` (the token written at Play's capture) to the DataPath the snapshot holds — and never the runtime edits. A Terrain with no DataPath has no authored voxels and writes nothing.
  - Loading: writing `DataPath` (load, Stop restore, undo) with a path no live Terrain holds reads the file under `resources_root()` with `decode_avox`. A missing or damaged file leaves the Terrain empty and **prints one line to the Output window**:
    - `Terrain <Name>: its voxel file <DataPath> is missing, so it is empty`
    - `Terrain <Name>: its voxel file <DataPath> is damaged (<reason>), so it is empty`

    A damaged file is never overwritten: on finding it, the Terrain takes a new DataPath right away (the usual `terrain/<Name>.<guid>.avox`, with a numeric suffix if that name is the damaged file), so the user can still recover the old one by hand.
  - **Output for engine instances:** `void DataModel::set_warning_sink(std::function<void(const std::string&)> sink);` and `void DataModel::warn(const std::string& text) const;` (stored in `DataModelState`; `warn` does nothing without a sink). `Engine`'s constructor wires it the way it wires `physics_.set_warning_sink` (`Engine.cpp:58-66`): `game_.set_warning_sink([this](const std::string& text) { scripts_->append_output(ScriptRuntime::OutputKind::Print, text); });`. Tests set their own sink and collect the lines.
  - Every voxel edit while stopped calls `note_unrecorded_edit(id())` and sets `voxels_changed_`; the Lua methods (Tasks 8–9) go through `Terrain` methods that do this, never through `volume()` directly.

- [ ] **Step 1: Write the failing tests**

Use the project round-trip helpers that `sandbox/project_tests.cpp` uses (a temporary folder, `Project::create`/`save`/`open` or their actual names):

```cpp
TEST_CASE("TS1 a Terrain's voxels are saved with the project and come back", "[terrain]") {
    // Make a temp project folder as project_tests.cpp does; add a Terrain in Workspace;
    // terrain.fill_ball(Vec3{0,0,0}, 4, 0, ...) via the Terrain method from Task 8 is not
    // available yet, so edit through a Terrain method that marks the change:
    //   terrain.edit_volume([](VoxelVolume& v){ return v.fill(ball, 2); });
    // Save. REQUIRE a file exists at resources/terrain/*.avox and terrain.data_path() is not empty.
    // Open the project into a fresh Game. Find the Terrain by name; REQUIRE its cell (0,0,0) has Id 2.
}

TEST_CASE("TS2 a missing or damaged .avox loads an empty Terrain and says so in Output", "[terrain]") {
    // As TS1, with game.set_warning_sink collecting lines. Delete the .avox before reopening.
    // REQUIRE the reopened Terrain's volume().chunks().empty(), exactly one line, and that it
    // contains "is missing, so it is empty".
    // Again with the file's middle byte flipped: one line containing "is damaged (".
    // REQUIRE the damaged Terrain's DataPath changed at load; edit it and save: the damaged file is
    // still on disk, byte for byte.
}

TEST_CASE("TS4 saving during play writes the voxels from Play's snapshot, not the runtime edits", "[terrain]") {
    // New project; add a Terrain and edit_volume a ball with Id 1 (it gets a DataPath now) — do NOT save.
    // capture_place; start_simulation; edit_volume a second ball elsewhere with Id 2.
    // Save while playing. stop_simulation. Reopen into a fresh Game:
    // REQUIRE the first ball is there (Id 1) and the second is not.
}

TEST_CASE("TS3 voxel edits while stopped mark the place unsaved; edits during play do not", "[terrain]") {
    // After a save: an edit_volume call makes game.history().dirty() true.
    // After another save, start_simulation, edit, REQUIRE dirty() is false.
}
```

Write these concretely from `project_tests.cpp`'s helpers. This task adds `Terrain::edit_volume(const std::function<std::optional<std::string>(terrain::VoxelVolume&)>&)`, which runs the edit, then marks the change (`note_unrecorded_edit` while stopped, `voxels_changed_ = true`). Tasks 8–9 call it.

- [ ] **Step 2–5:** run to fail, implement as in Interfaces (call `save_resources` in `save_tree` for each node of `world.authored_tree(want)` whose instance is live, before `plan_files`), run `[terrain]` and the project tests (`build/Debug/sandbox.exe "[project]"` — check the tag), commit (`Save and load a Terrain's voxels in its .avox file`).

---

### Task 8: Lua shape edits

**Files:**
- Create: `src/engine_core/TerrainBindings.cpp`
- Modify: `src/engine_core/ScriptBindings.hpp` (static declarations), `src/engine_core/LuaApi.cpp` (method docs), `CMakeLists.txt`
- Create: `sandbox/terrain_lua_tests.cpp`; add to the sandbox list

**Interfaces:**
- Consumes: `Terrain::edit_volume` (Task 7), `Terrain::material_by_id`, `terrain::Shape`.
- Produces Lua methods on Terrain: `FillBall`, `FillBlock`, `FillCylinder`, `FillWedge`, `SubtractBall`, `SubtractBlock`, `SubtractCylinder`, `SubtractWedge`, `PaintBall`, `PaintBlock`, `ReplaceMaterial`, with signatures exactly as the spec's Lua API table (`space` optional last, `Enum.TransformSpace`, World default).

- [ ] **Step 1: Write the failing tests**

```cpp
// Terrain's Lua methods.

#include "support.hpp"

#include "Terrain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

std::string all_text(const engine_core::ScriptRuntime::OutputBatch& batch) {
    std::string out;
    for (const auto& line : batch.lines) {
        out += line.text;
    }
    return out;
}

}  // namespace

TEST_CASE("TL1 FillBall and ReadVoxels agree", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local rock = t:AddMaterial(nil)
        t:FillBall(Vector3.new(0, 0, 0), 4, rock)
        local v = t:ReadVoxels(Vector3.new(0, 0, 0), Vector3.new(0, 0, 0))
        print(v.Materials[1][1][1] == rock.Id, v.Distances[1][1][1] < 0)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\n"));
}

TEST_CASE("TL2 Local space follows a moved and turned Terrain; World space does not", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t.Transform = Matrix4.new(100, 0, 0) * Matrix4.Angles(0, math.pi / 2, 0)
        t:FillBall(Vector3.new(0, 0, 0), 3, nil, Enum.TransformSpace.Local)
        t:FillBall(Vector3.new(100, 0, 20), 3, nil)
        local here = t:WorldToCell(Vector3.new(100, 0, 0))
        local far = t:WorldToCell(Vector3.new(100, 0, 20))
        local a = t:ReadVoxels(here, here).Distances[1][1][1]
        local b = t:ReadVoxels(far, far).Distances[1][1][1]
        print(here == Vector3.new(0, 0, 0), a < 0, b < 0)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\ttrue\n"));
}

TEST_CASE("TL3 Subtract, Paint, and ReplaceMaterial", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local a, b = t:AddMaterial(nil), t:AddMaterial(nil)
        t:FillBlock(Matrix4.new(0, 0, 0), Vector3.new(10, 10, 10), a)
        t:SubtractBall(Vector3.new(0, 0, 0), 2)
        t:PaintBlock(Matrix4.new(3, 0, 0), Vector3.new(2, 2, 2), b)
        local function at(x) return t:ReadVoxels(Vector3.new(x, 0, 0), Vector3.new(x, 0, 0)) end
        print(at(0).Distances[1][1][1] > 0, at(3).Materials[1][1][1] == b.Id)
        t:ReplaceMaterial(Vector3.new(-10, -10, -10), Vector3.new(10, 10, 10), b, a)
        print(at(3).Materials[1][1][1] == a.Id)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\n"));
    REQUIRE(has_line(out, "true\n"));
}

TEST_CASE("TL4 materials must be this Terrain's TerrainMaterials", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local u = Instance.new("Terrain", workspace)
        local mine = u:AddMaterial(nil)
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 2, mine) end)))
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 2, workspace) end)))
    )");
    rig.frames(1);
    const std::string text = all_text(rig.runtime.drain_output());
    INFO(text);
    REQUIRE(text.find("TerrainMaterial belongs to another Terrain") != std::string::npos);
    REQUIRE(text.find("Pass a TerrainMaterial (see Terrain:GetMaterials)") != std::string::npos);
}

TEST_CASE("TL5 too large an edit raises", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        print(select(2, pcall(function() t:FillBlock(Matrix4.new(), Vector3.new(400, 400, 400), nil) end)))
    )");
    rig.frames(1);
    REQUIRE(all_text(rig.runtime.drain_output()).find("Terrain edit too large: split it into smaller calls") !=
            std::string::npos);
}

TEST_CASE("TL6 a zero or negative radius changes nothing", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t:FillBall(Vector3.new(), 0, nil)
        t:FillBall(Vector3.new(), -3, nil)
        print(t:ReadVoxels(Vector3.new(), Vector3.new()).Distances[1][1][1] > 0)
    )");
    rig.frames(1);
    REQUIRE(has_line(rig.runtime.drain_output(), "true\n"));
}
```

`AddMaterial`, `ReadVoxels`, and `WorldToCell` come in Task 9; implement Task 8 and Task 9 back to back and run TL1–TL6 once both exist, or add a minimal `ReadVoxels` here. Prefer: this task implements `AddMaterial` (one-liner over `Terrain::add_material`), `WorldToCell`, and `ReadVoxels` first, since every test needs them, and Task 9 adds the rest. Check `Matrix4.new(x, y, z)` and `Matrix4.Angles` are the Luau constructor names (`src/engine_datatypes/Matrix4.cpp`'s library table) and adapt.

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement**

`TerrainBindings.cpp` defines the `ScriptBindings` statics (declared in `ScriptBindings.hpp` beside `mesh_add_box`) and its own `ANARCHY_LUA_REGISTER(register_terrain_methods)` with `register_lua_class("Terrain", nullptr, methods, n)`. Shared helpers in an anonymous namespace:

```cpp
// self as a Terrain, or raises.
Terrain& terrain_self(lua_State* state);   // as ScriptBindings::mesh_self, ScriptBindings.cpp:1186
// Argument index as this Terrain's material Id: 0 for nil.
std::uint8_t material_arg(lua_State* state, int index, const Terrain& terrain);
// The optional Enum.TransformSpace at index; World when none.
bool local_space(lua_State* state, int index);   // check_enum_arg(state, index, transform_space_enum())
// A world (or local) position or frame into the Terrain's space.
Vec3 to_local(const Terrain& terrain, Vec3 p, bool local);
Matrix4 to_local(const Terrain& terrain, const Matrix4& frame, bool local);
// Raises with the volume's refusal, if any.
void raise_if(lua_State* state, const std::optional<std::string>& error);
```

`material_arg`: nil → 0; a `TerrainMaterial` whose parent is `terrain.id()` → its Id; a TerrainMaterial of another parent → `"TerrainMaterial belongs to another Terrain"`; anything else → `"Pass a TerrainMaterial (see Terrain:GetMaterials)"`. Matrix4 arguments: read them the way existing bindings read a Matrix4 userdata (find the `to_matrix4`/check helper in `src/engine_datatypes/Matrix4.hpp`).

Each method builds a `terrain::Shape` (Ball: center, radius; Block/Wedge: frame, size; Cylinder: frame, `size = (2r, height, 2r)`), converts it into local space unless `space` is Local, and calls `terrain.edit_volume([&](VoxelVolume& v){ return v.fill(shape, id); })`, raising on a refusal. Argument positions: `FillBall(center, radius, material, space)` → 2, 3, 4, 5; `SubtractBall(center, radius, space)` → 2, 3, 4; and so on per the spec table. `ReplaceMaterial(min, max, from, to, space)`: converts both corners to cells (`WorldToCell` math) and takes the component-wise min/max.

`WorldToCell(p)`: `local = inverse(Transform) * p`; `Vector3(round(local.x / VoxelSize), ...)`. `CellToWorld(c)`: `Transform * (c * VoxelSize)`.

`ReadVoxels(min, max)`: rounds both to integers, orders them, calls `volume().read`, raises on refusal, and builds `{Distances = t, Materials = m}` where `t[i][j][k]` is cell `(min.x + i - 1, min.y + j - 1, min.z + k - 1)`.

`AddMaterial(material?)`: nil or a Material instance (raise `"material must be a Material"` otherwise), `terrain.add_material(...)`, push the result.

Docs in `LuaApi.cpp`: one `add("Terrain", "<Method>", ...)` per method with `P(...)` params and return types, each Fill/Subtract/Paint summary ending with "Meshes and colliders follow a frame or two later."

- [ ] **Step 4: Run tests** (`[terrain]`, full suite). **Step 5: Commit** (`Give Terrain its Lua shape edits`).

---

### Task 9: Lua materials, raw voxels, and helpers

**Files:**
- Modify: `src/engine_core/TerrainBindings.cpp`, `src/engine_core/ScriptBindings.hpp`, `src/engine_core/LuaApi.cpp`
- Test: `sandbox/terrain_lua_tests.cpp`

**Interfaces:**
- Produces Lua methods: `GetMaterials() -> {TerrainMaterial}`, `GetMaterialById(id) -> TerrainMaterial?`, `GetMaterialsFor(material) -> {TerrainMaterial}`, `RemoveMaterial(entry)`, `WriteVoxels(min, distances, materials)`, `CellToWorld(cell)`, `Clear()`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("TL7 material lookups", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local a, b = t:AddMaterial(nil), t:AddMaterial(nil)
        print(#t:GetMaterials(), t:GetMaterialById(2) == b, t:GetMaterialById(9))
        t:RemoveMaterial(a)
        print(#t:GetMaterials(), t:AddMaterial(nil).Id)
        print(pcall(function() b.Id = 7 end))
        print(pcall(function() b.Parent = workspace end))
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "2\ttrue\tnil\n"));
    REQUIRE(has_line(out, "1\t1\n"));
    REQUIRE(all_text(out).find("TerrainMaterial cannot be reparented") != std::string::npos);
}

TEST_CASE("TL8 WriteVoxels round-trips ReadVoxels exactly, unassigned Ids included", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local u = Instance.new("Terrain", workspace)
        local lo, hi = Vector3.new(-3, -3, -3), Vector3.new(3, 3, 3)
        local d, m = {}, {}
        for x = 1, 7 do d[x], m[x] = {}, {}
            for y = 1, 7 do d[x][y], m[x][y] = {}, {}
                for z = 1, 7 do d[x][y][z] = (x + y + z) / 4 - 3; m[x][y][z] = 42 end end end
        t:WriteVoxels(lo, d, m)
        local v = t:ReadVoxels(lo, hi)
        u:WriteVoxels(lo, v.Distances, v.Materials)
        local w = u:ReadVoxels(lo, hi)
        local same = true
        for x = 1, 7 do for y = 1, 7 do for z = 1, 7 do
            same = same and w.Distances[x][y][z] == v.Distances[x][y][z] and w.Materials[x][y][z] == v.Materials[x][y][z]
        end end end
        print(same, v.Materials[1][1][1])
        print(select(2, pcall(function() t:WriteVoxels(lo, {{{0}}}, {{{1.5}}}) end)))
        print(select(2, pcall(function() t:WriteVoxels(lo, {{{0}}}, {{{256}}}) end)))
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    const std::string text = all_text(out);
    INFO(text);
    REQUIRE(has_line(out, "true\t42\n"));
    REQUIRE(text.find("material Ids must be whole numbers from 0 to 255") != std::string::npos);
}

TEST_CASE("TL9 CellToWorld inverts WorldToCell; Clear empties the Terrain but keeps materials", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t.Transform = Matrix4.new(5, 6, 7)
        local m = t:AddMaterial(nil)
        t:FillBall(Vector3.new(5, 6, 7), 3, m)
        print(t:CellToWorld(t:WorldToCell(Vector3.new(8, 6, 7))) == Vector3.new(8, 6, 7))
        t:Clear()
        print(t:ReadVoxels(Vector3.new(), Vector3.new()).Distances[1][1][1] > 0, #t:GetMaterials())
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    REQUIRE(has_line(out, "true\n"));
    REQUIRE(has_line(out, "true\t1\n"));
}
```

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement**

- `GetMaterials`/`GetMaterialsFor`: push a list (as `instance_children`, `ScriptBindings.cpp:557-577`); register with `returns_list = true` and type `"TerrainMaterial"`.
- `GetMaterialById(id)`: a whole number 1–255, else nil.
- `RemoveMaterial(entry)`: must be a TerrainMaterial of this Terrain (same errors as `material_arg`, except nil raises `"RemoveMaterial needs a TerrainMaterial"`); then `destroy_error` check and `destroy`, as the Lua Destroy binding does (`ScriptBindings.cpp:549-552`).
- `WriteVoxels(min, distances, materials)`: both 3-deep tables of equal shape; the box is `min .. min + (nx-1, ny-1, nz-1)` from the outer lengths (every inner table must match: `"distances and materials must have the same shape"`); each material value must be an integer 0–255 (`lua_tonumber` then check `std::floor(v) == v`), else `"material Ids must be whole numbers from 0 to 255"`; each distance finite. Flatten x fastest and call `edit_volume(... v.write(...))`.
- `CellToWorld`, `Clear` (`edit_volume([](auto& v){ v.clear(); return std::nullopt; })`).
- Docs for each.

- [ ] **Step 4: Run tests** (`[terrain]`, then the full `build/Debug/sandbox.exe`). **Step 5: Release build** of `AnarchyStudio` to confirm it links: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target AnarchyStudio --parallel`. **Step 6: Commit** (`Give Terrain its Lua material lookups and raw voxel access`).
