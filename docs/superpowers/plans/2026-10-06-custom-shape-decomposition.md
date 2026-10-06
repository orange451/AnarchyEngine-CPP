# Custom Shape Decomposition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An unanchored `Custom` PhysicsObject collides as its Mesh, concave parts included, by splitting the Mesh into convex pieces with V-HACD. The pieces are stored in the Mesh's AMESH file, which goes to version 1.1.

**Architecture:** AMESH 1.1 gains an optional section of convex pieces (points in mesh space). `engine_core/ConvexDecomposition` wraps V-HACD, keeps a memory cache keyed by a geometry hash, and owns `ConvexDecomposer`, a one-thread queue the Engine runs each stopped step to decompose Custom meshes and write the pieces into their files. `PhysicsWorld` gives a body several shapes and builds an unanchored Custom as one hull per piece.

**Tech Stack:** C++17, CMake FetchContent, Box3D (pinned), V-HACD 4.1 (`VHACD.h`), Catch2 (sandbox), the hand-rolled AMESH test runner (`tests/AmeshTest.cpp`).

**Spec:** `docs/superpowers/specs/2026-10-06-custom-shape-decomposition-design.md`

## Global Constraints

- Work in the worktree `/Users/yaoli/Documents/AnarchyEngine-CPP-custom-decomposition` on branch `custom-decomposition`. Never commit on `main`; other sessions move `main` underneath worktrees.
- Configure this worktree's own build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP`. Never pass `FETCHCONTENT_BASE_DIR` pointing at the main checkout's `build/_deps`.
- Code must build with Apple clang 13 / libc++ 13 (this Mac) and MSVC 19.23 / C++17. No C++20 library features. `std::filesystem::file_time_type` counts in `__int128` on libc++ 13: convert with `std::chrono::duration_cast<std::chrono::nanoseconds>(...).count()` before printing or `std::to_string`.
- Box3D is included only by `src/engine_core/PhysicsWorld.cpp`. V-HACD is included only by `src/engine_core/ConvexDecomposition.cpp`.
- Decomposition settings (fixed): at most 32 pieces, 100,000 voxels, at most 64 points per piece, 1% volume error, flood fill, shrink wrap on, V-HACD async off. `kRecipe = 1`.
- AMESH limits: `kMaxPieces = 256`, `kMaxPiecePoints = 128`, at least 4 points per piece, every point finite. `FLAG_HULLS = 1u << 6`.
- Warning text: `PhysicsObject <name>: Custom fell back to Hull (<reason>)`.
- Comments match the surrounding code: short, plain sentences saying what a thing is or why.
- Building: `cmake --build build --target sandbox amesh-tests --parallel`. Running: `./build/amesh-tests`, `./build/sandbox "[physics]"`, `./build/sandbox "[shapes]"`, `./build/sandbox "[decomposition]"`.
- At most one Anarchy Studio open at a time; quit it as soon as the manual check is done.

## Review Focus

1. **An imported mesh with LODs gets pieces.** Expected: the file keeps its LODs, and pieces come from the finest LOD. Pinned by M5 in Task 3.
2. **Pressing Play while the studio's worker is still decomposing.** Expected: play decomposes on its own (stalling, accepted), nothing is written to the file during play, and the worker's result is written after Stop. Pinned by Q3 in Task 7.
3. **A Mesh whose decomposition finds nothing.** Expected: one fallback warning, a Hull, and no retry every frame in the studio, since a Mesh is queued once per file stamp. Pinned by P29 in Task 5, and by Q1 and Q4 in Task 7.
4. **A non-uniform Size.** Expected: the pieces stretch with the Mesh, fitted by the whole Mesh's bounds, so a stretched cup still holds a ball. Pinned by P31 in Task 5.
5. **Two PhysicsObjects sharing one Mesh.** Expected: the Mesh is decomposed once, and both bodies collide as pieces. Pinned by Q1 in Task 7.

---

## File Structure

| File | Change | Responsibility |
| --- | --- | --- |
| `src/amesh/amesh.hpp` | Modify | 1.1 header fields, `FLAG_HULLS`, limits, `ConvexPiece`, `Data::pieces` |
| `src/amesh/amesh.cpp` | Modify | Read, validate, and write the pieces section |
| `tests/AmeshTest.cpp` | Modify | A1–A4 and the existing version and reserved-field checks |
| `CMakeLists.txt` | Modify | Fetch V-HACD; add `ConvexDecomposition.cpp` and the new sandbox test file |
| `src/engine_core/ConvexDecomposition.hpp/.cpp` | Create | `decompose`, the memory cache, `known_pieces`, `pieces_for`, `ConvexDecomposer` |
| `sandbox/convex_decomposition_tests.cpp` | Create | D1, D2, Q1–Q4 |
| `src/engine_instances/AssetInstances.hpp/.cpp` | Modify | `Mesh::file_pieces`, `store_pieces`, `file_stamp`; `edit_geometry` drops pieces |
| `sandbox/mesh_shapes_tests.cpp` | Modify | M1–M5 |
| `src/engine_core/PhysicsWorld.hpp/.cpp` | Modify | Several shapes per body; pieces for an unanchored Custom; outline pieces; test accessors |
| `sandbox/physics_tests.cpp` | Modify | P16 update, P27–P33 |
| `src/runner/GameView.hpp/.cpp` | Modify | Pass known pieces to the outline; refresh it when the file changes |
| `src/engine_core/Engine.hpp/.cpp` | Modify | Own a `ConvexDecomposer`; run it each stopped step |

---

### Task 1: AMESH 1.1 pieces section

**Files:**
- Modify: `src/amesh/amesh.hpp`
- Modify: `src/amesh/amesh.cpp` (`Layout`, `Plan`, `CheckHeader`, `Validate`, `Decode`, `write`)
- Test: `tests/AmeshTest.cpp`

**Interfaces:**
- Produces: `anarchy::amesh::ConvexPiece { std::vector<std::array<float, 3>> points; }`, `Data::piece_recipe` (`std::uint32_t`), `Data::pieces` (`std::vector<ConvexPiece>`), `FLAG_HULLS`, `kMaxPieces`, `kMaxPiecePoints`, `kMinPiecePoints`, header fields `piece_count` and `piece_point_total`.

- [ ] **Step 1: Configure the worktree build**

Run: `cd /Users/yaoli/Documents/AnarchyEngine-CPP-custom-decomposition && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP && cmake --build build --target amesh-tests --parallel && ./build/amesh-tests`
Expected: `AMESH checks passed`.

- [ ] **Step 2: Write the failing tests**

In `tests/AmeshTest.cpp`, add after `SkinnedQuad()`:

```cpp
// A tetrahedron of side s at (x, 0, 0), as a piece.
ConvexPiece Tetra(float x, float s = 1) {
    ConvexPiece piece;
    piece.points = {{x, 0, 0}, {x + s, 0, 0}, {x, s, 0}, {x, 0, s}};
    return piece;
}

// The quad with three pieces of recipe 7.
Data PiecedQuad() {
    Data data = Quad();
    data.piece_recipe = 7;
    data.pieces = {Tetra(0), Tetra(2), Tetra(4, 2)};
    data.pieces[2].points.push_back({5, 1, 1});
    return data;
}
```

Add these test functions before `TestGpuMeshWithoutGl`:

```cpp
void TestPiecesRoundTrip() {
    const Data data = PiecedQuad();
    const std::vector<std::byte> bytes = write(data);
    const auto header = Peek<AEHeader>(bytes, 0);
    Expect(header.version_major == 1 && header.version_minor == 1, "a file is written as 1.1");
    Expect((header.flags & FLAG_HULLS) != 0, "pieces set FLAG_HULLS");
    Expect(header.piece_count == 3 && header.piece_point_total == 13, "the header counts the pieces and their points");
    Expect(bytes.size() == kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize + 4 + 3 * 4 + 13 * 12 + kCrcSize,
           "the section is a recipe, a count per piece, and the points");
    const Data back = read(bytes);
    Expect(back.piece_recipe == 7, "the recipe round-trips");
    Expect(back.pieces.size() == 3 && back.pieces[2].points.size() == 5, "the pieces round-trip");
    Expect(back.pieces[1].points[1] == std::array<float, 3>{3, 0, 0}, "a piece's points round-trip");
    Expect(write(back) == bytes, "a read file rewrites to the same bytes");
}

void TestVersionOneZeroReads() {
    std::vector<std::byte> bytes = write(Quad());
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, version_minor), 0);
    Recrc(bytes);
    const Data back = read(bytes);
    Expect(back.vertices.size() == 4 && back.pieces.empty(), "a 1.0 file reads, with no pieces");
}

void TestPiecesRejected() {
    const std::vector<std::byte> good = write(PiecedQuad());
    const std::size_t section = kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize;
    const std::size_t points = section + 4 + 3 * 4;

    std::vector<std::byte> bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, version_minor), 0);
    Recrc(bytes);
    ExpectRejected(bytes, offsetof(AEHeader, flags), "a 1.0 file with FLAG_HULLS");

    bytes = good;
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, piece_count), kMaxPieces + 1);
    ExpectRejected(bytes, offsetof(AEHeader, piece_count), "piece_count over 256");

    bytes = write(Quad());
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, piece_count), 1);
    Recrc(bytes);
    ExpectRejected(bytes, offsetof(AEHeader, piece_count), "piece_count without FLAG_HULLS");

    bytes = good;
    Poke<std::uint32_t>(bytes, section + 4, 3);
    Poke<std::uint32_t>(bytes, section + 8, 5);
    Recrc(bytes);
    ExpectRejected(bytes, section + 4, "a piece of 3 points");

    bytes = good;
    Poke<std::uint32_t>(bytes, section + 4, 5);
    Recrc(bytes);
    ExpectRejected(bytes, section + 4, "piece counts that disagree with piece_point_total");

    bytes = good;
    Poke<float>(bytes, points, std::nanf(""));
    Recrc(bytes);
    ExpectRejected(bytes, points, "a point that is not finite");

    Data data = PiecedQuad();
    data.pieces[0].points.resize(kMaxPiecePoints + 1, {0, 0, 0});
    ExpectWriteRejected(data, "a piece of 129 points");
    data = PiecedQuad();
    data.pieces[0].points.resize(3);
    ExpectWriteRejected(data, "a piece of 3 points");
    data = PiecedQuad();
    data.pieces[1].points[0][1] = INFINITY;
    ExpectWriteRejected(data, "a piece point that is not finite");
}

void TestNoPiecesSameSize() {
    const std::vector<std::byte> bytes = write(Quad());
    const auto header = Peek<AEHeader>(bytes, 0);
    Expect(bytes.size() == kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize + kCrcSize,
           "a mesh without pieces is as large as in 1.0");
    Expect(header.piece_count == 0 && header.piece_point_total == 0 && (header.flags & FLAG_HULLS) == 0,
           "a mesh without pieces has both piece words 0 and no FLAG_HULLS");
}
```

Register them in `main()`'s list: `TestPiecesRoundTrip, TestVersionOneZeroReads, TestPiecesRejected, TestNoPiecesSameSize,`. Add `#include <array>` to the test's includes.

Update the existing checks that the new version changes:
- `TestHeaderOffsets`: `reserved1` → `piece_count`, `reserved2` → `piece_point_total` (offsets 56 and 60 stay).
- `TestStaticMeshLayout`: `header.version_minor == 0` → `== 1`, and its message `"the header says AESH 1.1, 64 bytes"`.
- `TestReaderRejects`: the version check pokes `version_minor` to `2` with the message `"version 1.2"`. The unknown-flag check uses `(1u << 7)`. Replace the reserved2 check with:

```cpp
    bytes = good;
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, piece_point_total), 1);
    ExpectRejected(bytes, offsetof(AEHeader, piece_count), "piece_point_total without FLAG_HULLS");
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build build --target amesh-tests --parallel`
Expected: compile errors: `ConvexPiece`, `piece_count`, `FLAG_HULLS`, `kMaxPieces` are not declared.

- [ ] **Step 4: Implement the format in `amesh.hpp`**

Header comment: change `version 1.0` to `version 1.1 (1.0 still reads)` and add the section to the layout table, after the subsets line:

```
//     u32         piece_recipe                     4 bytes,      FLAG_HULLS
//     u32         piece_points[piece_count]        4 bytes each, FLAG_HULLS
//     f32         piece_xyz[piece_point_total][3] 12 bytes each, FLAG_HULLS
```

and after the "A static mesh is a skinned mesh…" paragraph:

```
// Pieces are convex point sets in the mesh's own space that together cover it,
// for a physics body that cannot use the triangles. piece_recipe names the
// settings that made them, so a reader can tell when they are stale.
```

Constants and types:

```cpp
inline constexpr std::uint16_t kVersionMinor = 1;
inline constexpr std::uint16_t FLAG_HULLS = 1u << 6;
inline constexpr std::uint16_t kKnownFlags =
    FLAG_SKINNED | FLAG_LODS | FLAG_SUBSETS | FLAG_VERTEX_COLOR | FLAG_TANGENTS | FLAG_UNORM_UV | FLAG_HULLS;
inline constexpr std::uint32_t kMaxPieces = 256;
inline constexpr std::uint32_t kMinPiecePoints = 4;
inline constexpr std::uint32_t kMaxPiecePoints = 128;
```

In `AEHeader`, replace the two reserved words:

```cpp
    std::uint32_t piece_count;        // 0 if !FLAG_HULLS else 1 to kMaxPieces
    std::uint32_t piece_point_total;  // 0 if !FLAG_HULLS else the pieces' points, summed
```

Before `struct Data`:

```cpp
// A convex set of points in the mesh's own space.
struct ConvexPiece {
    std::vector<std::array<float, 3>> points;
};
```

In `Data`, after `high_quality_lods`:

```cpp
    // Convex pieces covering the mesh, and which settings made them. No pieces: no FLAG_HULLS.
    std::uint32_t piece_recipe = 0;
    std::vector<ConvexPiece> pieces;
```

Add `#include <array>`.

- [ ] **Step 5: Implement reading and writing in `amesh.cpp`**

`Layout` gains `std::uint64_t pieces = 0;`. In `Plan`, after the subsets block:

```cpp
    l.pieces = at;
    if (h.flags & FLAG_HULLS) {
        at += sizeof(std::uint32_t) + std::uint64_t{h.piece_count} * sizeof(std::uint32_t) +
              std::uint64_t{h.piece_point_total} * 3 * sizeof(float);
    }
```

In `CheckHeader`, replace the version check and the two reserved checks:

```cpp
    if (h.version_major != kVersionMajor || h.version_minor > kVersionMinor) {
        throw AEMeshError(offsetof(AEHeader, version_major),
                          "version " + Num(h.version_major) + "." + Num(h.version_minor) + " is not 1.0 or 1.1");
    }
```

```cpp
    const bool hulls = (h.flags & FLAG_HULLS) != 0;
    if (hulls && h.version_minor == 0) {
        throw AEMeshError(offsetof(AEHeader, flags), "a 1.0 file cannot have FLAG_HULLS");
    }
    if (hulls ? h.piece_count < 1 || h.piece_count > kMaxPieces : h.piece_count != 0 || h.piece_point_total != 0) {
        throw AEMeshError(offsetof(AEHeader, piece_count),
                          "FLAG_HULLS and piece_count " + Num(h.piece_count) + " disagree, or it is over 256");
    }
    if (h.piece_point_total > std::uint64_t{h.piece_count} * kMaxPiecePoints) {
        throw AEMeshError(offsetof(AEHeader, piece_point_total),
                          "piece_point_total " + Num(h.piece_point_total) + " is over the limit");
    }
```

(The `version_minor` check sits before the flag check, so the hulls rule can read it.)

New check, called last in `Validate` (`CheckPieces(bytes, l);`):

```cpp
void CheckPieces(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    if ((h.flags & FLAG_HULLS) == 0) {
        return;
    }
    std::uint64_t total = 0;
    for (std::uint32_t k = 0; k < h.piece_count; ++k) {
        const std::uint64_t at = l.pieces + 4 + std::uint64_t{k} * 4;
        const auto count = Load<std::uint32_t>(bytes, at);
        if (count < kMinPiecePoints || count > kMaxPiecePoints) {
            throw AEMeshError(at, "piece " + Num(k) + " has " + Num(count) + " points; it needs 4 to 128");
        }
        total += count;
    }
    if (total != h.piece_point_total) {
        throw AEMeshError(l.pieces + 4, "the pieces' point counts sum to " + Num(total) + ", not piece_point_total " +
                                            Num(h.piece_point_total));
    }
    const std::uint64_t points = l.pieces + 4 + std::uint64_t{h.piece_count} * 4;
    for (std::uint64_t i = 0; i < total * 3; ++i) {
        if (!Finite({Load<float>(bytes, points + i * 4)})) {
            throw AEMeshError(points + i * 4, "piece point " + Num(i / 3) + " is not finite");
        }
    }
}
```

In `Decode`, before `return data;`:

```cpp
    if (h.flags & FLAG_HULLS) {
        data.piece_recipe = Load<std::uint32_t>(bytes, l.pieces);
        std::uint64_t point = l.pieces + 4 + std::uint64_t{h.piece_count} * 4;
        data.pieces.resize(h.piece_count);
        for (std::uint32_t k = 0; k < h.piece_count; ++k) {
            const auto count = Load<std::uint32_t>(bytes, l.pieces + 4 + std::uint64_t{k} * 4);
            data.pieces[k].points.resize(count);
            for (std::array<float, 3>& p : data.pieces[k].points) {
                p = {Load<float>(bytes, point), Load<float>(bytes, point + 4), Load<float>(bytes, point + 8)};
                point += 12;
            }
        }
    }
```

In `write`, with the other count checks at the top:

```cpp
    if (data.pieces.size() > kMaxPieces) {
        throw AEMeshError(offsetof(AEHeader, piece_count), Num(data.pieces.size()) + " pieces is over the limit");
    }
    std::uint64_t piece_points = 0;
    for (std::size_t k = 0; k < data.pieces.size(); ++k) {
        const auto& points = data.pieces[k].points;
        if (points.size() < kMinPiecePoints || points.size() > kMaxPiecePoints) {
            throw AEMeshError(offsetof(AEHeader, piece_point_total),
                              "piece " + Num(k) + " has " + Num(points.size()) + " points; it needs 4 to 128");
        }
        for (const auto& p : points) {
            if (!Finite({p[0], p[1], p[2]})) {
                throw AEMeshError(offsetof(AEHeader, piece_point_total), "piece " + Num(k) + " has a point that is not finite");
            }
        }
        piece_points += points.size();
    }
```

After the `FLAG_UNORM_UV` flag lines: `if (!data.pieces.empty()) { flags |= FLAG_HULLS; }`. After `header.subset_count = …`:

```cpp
    header.piece_count = static_cast<std::uint32_t>(data.pieces.size());
    header.piece_point_total = static_cast<std::uint32_t>(piece_points);
```

After the subsets loop, before the CRC store:

```cpp
    if (!data.pieces.empty()) {
        Store(out, l.pieces, data.piece_recipe);
        std::uint64_t point = l.pieces + 4 + data.pieces.size() * 4;
        for (std::size_t k = 0; k < data.pieces.size(); ++k) {
            Store(out, l.pieces + 4 + k * 4, static_cast<std::uint32_t>(data.pieces[k].points.size()));
            for (const auto& p : data.pieces[k].points) {
                Store(out, point, p[0]);
                Store(out, point + 4, p[1]);
                Store(out, point + 8, p[2]);
                point += 12;
            }
        }
    }
```

In `amesh_self_test`, after the unit-triangle round trip, add a check that one tetrahedron piece round-trips:

```cpp
        Data pieced = triangle;
        pieced.piece_recipe = 1;
        pieced.pieces.push_back(ConvexPiece{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}});
        const Data pieced_back = read(write(pieced));
        if (pieced_back.pieces.size() != 1 || pieced_back.pieces[0].points != pieced.pieces[0].points ||
            pieced_back.piece_recipe != 1) {
            return false;
        }
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build build --target amesh-tests --parallel && ./build/amesh-tests`
Expected: `AMESH checks passed`.

- [ ] **Step 7: Check nothing else reads the reserved fields**

Run: `grep -rn "reserved1\|reserved2" src tests sandbox`
Expected: no matches. Fix any that appear by using the new names.

- [ ] **Step 8: Commit**

```bash
git add src/amesh/amesh.hpp src/amesh/amesh.cpp tests/AmeshTest.cpp
git commit -m "AMESH 1.1: an optional section of convex pieces

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: V-HACD and `decompose`

**Files:**
- Modify: `CMakeLists.txt`
- Create: `src/engine_core/ConvexDecomposition.hpp`, `src/engine_core/ConvexDecomposition.cpp`
- Create: `sandbox/convex_decomposition_tests.cpp`

**Interfaces:**
- Consumes: `anarchy::amesh::ConvexPiece`, `kMaxPieces`, `kMaxPiecePoints` (Task 1).
- Produces: `engine_core::kRecipe` (`std::uint32_t`, 1); `std::vector<anarchy::amesh::ConvexPiece> engine_core::decompose(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles)`.

This task proves V-HACD builds on this toolchain before anything depends on it. If V-HACD fails to compile with Apple clang 13 / libc++ 13, stop and report the error. Don't patch V-HACD or swap libraries without asking.

- [ ] **Step 1: Write the failing test**

Create `sandbox/convex_decomposition_tests.cpp`:

```cpp
// ConvexDecomposition: V-HACD's pieces of a mesh, the memory cache, and the
// studio's queue that writes pieces into a Mesh's file.

#include "support.hpp"

#include "ConvexDecomposition.hpp"
#include "MeshShapes.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {

using engine_core::Vec3;

struct Geometry {
    std::vector<Vec3> points;
    std::vector<std::uint32_t> triangles;
};

Geometry geometry_of(const anarchy::amesh::Data& data) {
    Geometry out;
    for (const auto& v : data.vertices) {
        out.points.push_back(Vec3{v.p[0], v.p[1], v.p[2]});
    }
    out.triangles = data.indices;
    return out;
}

// An L: a bar along X from 0 to 3, one high, and a post on its left end up to 3.
Geometry ell() {
    anarchy::amesh::Data data;
    engine_core::add_box(data, Vec3{3.f, 1.f, 1.f}, Vec3{1.5f, 0.5f, 0.f});
    engine_core::add_box(data, Vec3{1.f, 2.f, 1.f}, Vec3{0.5f, 2.f, 0.f});
    return geometry_of(data);
}

}  // namespace

TEST_CASE("D1 an L splits into at least two convex pieces inside its bounds", "[decomposition]") {
    const Geometry l = ell();
    const std::vector<anarchy::amesh::ConvexPiece> pieces = engine_core::decompose(l.points, l.triangles);
    REQUIRE(pieces.size() >= 2);
    for (const auto& piece : pieces) {
        REQUIRE(piece.points.size() >= 4);
        REQUIRE(piece.points.size() <= 64);
        for (const auto& p : piece.points) {
            REQUIRE(p[0] >= -0.05f);
            REQUIRE(p[0] <= 3.05f);
            REQUIRE(p[1] >= -0.05f);
            REQUIRE(p[1] <= 3.05f);
            REQUIRE(p[2] >= -0.55f);
            REQUIRE(p[2] <= 0.55f);
        }
    }
}

TEST_CASE("D1b nothing to decompose gives no pieces", "[decomposition]") {
    REQUIRE(engine_core::decompose({}, {}).empty());
}
```

Add `sandbox/convex_decomposition_tests.cpp` to the `add_executable(sandbox …)` list in `CMakeLists.txt`, after `sandbox/physics_tests.cpp`.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake -S . -B build && cmake --build build --target sandbox --parallel`
Expected: `ConvexDecomposition.hpp` not found.

- [ ] **Step 3: Fetch V-HACD**

In `CMakeLists.txt`, after the Box3D block (`add_subdirectory(cmake/box3d …)`):

```cmake
# V-HACD splits a Custom PhysicsObject's Mesh into convex pieces: one
# BSD-3-Clause header by Khaled Mamou, pinned by tag. Only fetched here;
# ConvexDecomposition.cpp compiles it.
FetchContent_Declare(
    vhacd
    GIT_REPOSITORY https://github.com/kmammou/v-hacd.git
    GIT_TAG v4.1.0
)
FetchContent_GetProperties(vhacd)
if(NOT vhacd_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(vhacd)
endif()
```

Add `src/engine_core/ConvexDecomposition.cpp` to the engine_core sources after `src/engine_core/PhysicsWorld.cpp`. After `target_link_libraries(engine_core PRIVATE box3d)`:

```cmake
# V-HACD too: only ConvexDecomposition.cpp includes it. SYSTEM, so -Wextra does not report on it.
target_include_directories(engine_core SYSTEM PRIVATE "${vhacd_SOURCE_DIR}/include")
```

Then check the API names this task uses against the fetched header:

Run: `cmake -S . -B build && grep -n "m_maxConvexHulls\|m_resolution\|m_maxNumVerticesPerCH\|m_minimumVolumePercentErrorAllowed\|m_shrinkWrap\|m_fillMode\|m_asyncACD\|GetNConvexHulls\|GetConvexHull\|CreateVHACD()\|m_points" build/_deps/vhacd-src/include/VHACD.h | head -20`
Expected: each name appears. If one differs, use the header's name in Step 4.

- [ ] **Step 4: Implement `decompose`**

Create `src/engine_core/ConvexDecomposition.hpp`:

```cpp
#pragma once

// A Custom PhysicsObject's Mesh as convex pieces, for a body that moves: Box3D
// gives a triangle mesh contacts only on a static body. V-HACD makes them.

#include "Vector3.hpp"
#include "amesh.hpp"

#include <cstdint>
#include <vector>

namespace engine_core {

// Which settings made a set of pieces. Bump it whenever a setting in
// ConvexDecomposition.cpp changes, so pieces stored with the old ones are made again.
inline constexpr std::uint32_t kRecipe = 1;

// The pieces of a mesh, in its own space. Empty when there is nothing to
// decompose or V-HACD finds no piece. Any thread; seconds on a large mesh.
std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles);

}  // namespace engine_core
```

Create `src/engine_core/ConvexDecomposition.cpp`:

```cpp
#include "ConvexDecomposition.hpp"

#pragma warning(push, 0)
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"
#pragma warning(pop)

#include <utility>

namespace engine_core {

std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles) {
    std::vector<anarchy::amesh::ConvexPiece> pieces;
    if (points.empty() || triangles.size() < 3) {
        return pieces;
    }
    std::vector<float> flat;
    flat.reserve(points.size() * 3);
    for (const Vec3& p : points) {
        flat.push_back(p.x);
        flat.push_back(p.y);
        flat.push_back(p.z);
    }
    // The settings kRecipe names.
    VHACD::IVHACD::Parameters parameters;
    parameters.m_maxConvexHulls = 32;
    parameters.m_resolution = 100000;
    parameters.m_maxNumVerticesPerCH = 64;
    parameters.m_minimumVolumePercentErrorAllowed = 1;
    parameters.m_fillMode = VHACD::FillMode::FLOOD_FILL;
    parameters.m_shrinkWrap = true;
    // The caller picks the thread.
    parameters.m_asyncACD = false;

    VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
    if (vhacd->Compute(flat.data(), static_cast<std::uint32_t>(points.size()), triangles.data(),
                       static_cast<std::uint32_t>(triangles.size() / 3), parameters)) {
        for (std::uint32_t index = 0; index < vhacd->GetNConvexHulls(); ++index) {
            VHACD::IVHACD::ConvexHull hull;
            if (!vhacd->GetConvexHull(index, hull) || hull.m_points.size() < anarchy::amesh::kMinPiecePoints) {
                continue;
            }
            anarchy::amesh::ConvexPiece piece;
            for (const VHACD::Vertex& v : hull.m_points) {
                if (piece.points.size() == anarchy::amesh::kMaxPiecePoints) {
                    break;
                }
                piece.points.push_back(
                    {static_cast<float>(v.mX), static_cast<float>(v.mY), static_cast<float>(v.mZ)});
            }
            pieces.push_back(std::move(piece));
            if (pieces.size() == anarchy::amesh::kMaxPieces) {
                break;
            }
        }
    }
    vhacd->Clean();
    vhacd->Release();
    return pieces;
}

}  // namespace engine_core
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -20 && ./build/sandbox "[decomposition]"`
Expected: a clean build (no warnings from project code), then `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/engine_core/ConvexDecomposition.hpp src/engine_core/ConvexDecomposition.cpp sandbox/convex_decomposition_tests.cpp
git commit -m "Split a mesh into convex pieces with V-HACD

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Mesh pieces in its file

**Files:**
- Modify: `src/engine_instances/AssetInstances.hpp` (class `Mesh`)
- Modify: `src/engine_instances/AssetInstances.cpp` (`read_file`, `edit_geometry`, `on_reuse`, new methods)
- Test: `sandbox/mesh_shapes_tests.cpp`

**Interfaces:**
- Consumes: `Data::pieces`, `Data::piece_recipe` (Task 1).
- Produces on `engine_core::Mesh`:
  - `bool file_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece>& out) const`
  - `std::optional<std::string> store_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece> pieces)`
  - `std::string file_stamp() const`

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/mesh_shapes_tests.cpp`:

```cpp
namespace {

std::vector<anarchy::amesh::ConvexPiece> two_tetras() {
    anarchy::amesh::ConvexPiece a{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    anarchy::amesh::ConvexPiece b{{{2, 0, 0}, {3, 0, 0}, {2, 1, 0}, {2, 0, 1}}};
    return {a, b};
}

}  // namespace

TEST_CASE("M1 pieces stored in a Mesh's file are read back for their recipe", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))");
    const std::string before = cube.file_stamp();
    REQUIRE_FALSE(before.empty());
    std::vector<anarchy::amesh::ConvexPiece> found;
    REQUIRE_FALSE(cube.file_pieces(5, found));

    REQUIRE_FALSE(cube.store_pieces(5, two_tetras()));
    REQUIRE(cube.file_stamp() != before);
    REQUIRE(cube.file_pieces(5, found));
    REQUIRE(found.size() == 2);
    REQUIRE(found[1].points[0] == std::array<float, 3>{2, 0, 0});
    const Data file = rig.file_of(cube);
    REQUIRE(file.piece_recipe == 5);
    REQUIRE(file.vertices.size() == 24);
}

TEST_CASE("M2 pieces of another recipe are not found", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))");
    REQUIRE_FALSE(cube.store_pieces(5, two_tetras()));
    std::vector<anarchy::amesh::ConvexPiece> found;
    REQUIRE_FALSE(cube.file_pieces(6, found));
    REQUIRE(found.empty());
}

TEST_CASE("M3 a shape added after pieces drops them", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))");
    REQUIRE_FALSE(cube.store_pieces(5, two_tetras()));
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1), Vector3.new(3, 0, 0))");
    std::vector<anarchy::amesh::ConvexPiece> found;
    REQUIRE_FALSE(cube.file_pieces(5, found));
    REQUIRE(rig.file_of(cube).pieces.empty());
}

TEST_CASE("M4 pieces are not stored while playing, or without a file", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& bare = rig.mesh("Bare");
    REQUIRE(bare.file_stamp().empty());
    REQUIRE(bare.store_pieces(5, two_tetras()));
    engine_core::Mesh& cube = rig.mesh("Cube");
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))");
    const std::string stamp = cube.file_stamp();
    rig.game.start_simulation();
    REQUIRE(cube.store_pieces(5, two_tetras()));
    REQUIRE(cube.file_stamp() == stamp);
}

TEST_CASE("M5 storing pieces keeps a file's LODs", "[shapes]") {
    ShapeRig rig;
    anarchy::amesh::Data lodded;
    engine_core::add_box(lodded, Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f});
    engine_core::add_box(lodded, Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f});
    lodded.lods = {{0, 12}, {12, 12}};
    const std::vector<std::byte> bytes = anarchy::amesh::write(lodded);
    std::filesystem::create_directories(rig.resources / "meshes");
    std::ofstream(rig.resources / "meshes" / "lodded.amesh", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    engine_core::Mesh& mesh = rig.mesh("Lodded");
    REQUIRE_FALSE(mesh.set_path("meshes/lodded.amesh"));

    REQUIRE_FALSE(mesh.store_pieces(5, two_tetras()));
    const Data file = rig.file_of(mesh);
    REQUIRE(file.lods.size() == 2);
    REQUIRE(file.pieces.size() == 2);
}
```

Add `#include <array>` and `#include <cstddef>` to the file's includes.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile errors: `file_stamp`, `store_pieces`, `file_pieces` are not members of `Mesh`.

- [ ] **Step 3: Declare the methods**

In `AssetInstances.hpp`, in `class Mesh`'s public section after `origin_offset()`:

```cpp
    // The convex pieces in the AMESH file, when it has some of recipe and this
    // session made no geometry. Read again only when file_stamp changes. Needs
    // the DataModel lock; a read lock is enough.
    bool file_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece>& out) const;

    // Reads the file, sets its pieces, and writes it back as edit_geometry
    // does, keeping its LODs. Refused while playing or with no file. Not an
    // undo step. Returns why nothing changed.
    std::optional<std::string> store_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece> pieces);

    // The file as it is now: Path, time on disk, and size, as one string.
    // Empty with no Path or no file.
    std::string file_stamp() const;
```

Change `read_file`'s declaration to `…, anarchy::amesh::Data& out, bool allow_lods = false) const;` and add, after it:

```cpp
    // Writes data to path under root beside the file, then renames it over, so
    // a reader never sees half a mesh.
    static std::optional<std::string> write_file(const std::filesystem::path& root, const std::string& path,
                                                 const anarchy::amesh::Data& data);
```

Add the cache fields after the bounds cache fields:

```cpp
    // file_pieces, as last read, and the file_stamp it was read at.
    mutable std::mutex pieces_mutex_;
    mutable std::string pieces_stamp_;
    mutable std::uint32_t pieces_recipe_ = 0;
    mutable std::vector<anarchy::amesh::ConvexPiece> pieces_;
```

- [ ] **Step 4: Implement them**

In `AssetInstances.cpp`:

`read_file`: take `bool allow_lods`, and change the LOD check to `if (!allow_lods && out.lods.size() >= 2)`.

Move the write in `edit_geometry` (from `std::vector<std::byte> bytes;` through the rename) into `write_file`:

```cpp
std::optional<std::string> Mesh::write_file(const std::filesystem::path& root, const std::string& path,
                                            const anarchy::amesh::Data& data) {
    const std::filesystem::path file = root / std::filesystem::u8path(path);
    std::vector<std::byte> bytes;
    try {
        bytes = anarchy::amesh::write(data);
    } catch (const std::exception& failure) {
        return std::string("The shapes do not fit in an AMESH file: ") + failure.what();
    }
    // Written beside the file and renamed over it, so a reader never sees half a mesh.
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::filesystem::path partial = file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return "Could not write " + path;
        }
    }
    std::filesystem::rename(partial, file, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return "Could not write " + path;
    }
    return std::nullopt;
}
```

`edit_geometry`'s stopped branch becomes `edit(data); data.lods.clear(); data.pieces.clear();` then `if (std::optional<std::string> error = write_file(root, path, data)) { return error; }` followed by the existing `set_path` tail. In the playing branch, add `data.pieces.clear();` after `data.lods.clear();`.

New methods:

```cpp
std::string Mesh::file_stamp() const {
    const std::filesystem::path root = resources_root();
    if (root.empty() || path().empty()) {
        return {};
    }
    const std::filesystem::path file = root / std::filesystem::u8path(path());
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
    if (error) {
        return {};
    }
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error) {
        return {};
    }
    // libc++'s file clock counts in __int128, which to_string does not take.
    const long long nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return path() + "|" + std::to_string(nanoseconds) + "|" + std::to_string(size);
}

bool Mesh::file_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece>& out) const {
    out.clear();
    if (session_geometry().data) {
        return false;
    }
    const std::string stamp = file_stamp();
    if (stamp.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    if (pieces_stamp_ != stamp) {
        pieces_stamp_ = stamp;
        anarchy::amesh::Data data;
        read_file(resources_root(), path(), data, true);
        pieces_recipe_ = data.piece_recipe;
        pieces_ = std::move(data.pieces);
    }
    if (pieces_.empty() || pieces_recipe_ != recipe) {
        return false;
    }
    out = pieces_;
    return true;
}

std::optional<std::string> Mesh::store_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece> pieces) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (simulation_running()) {
        return std::string("Pieces are stored only while the place is stopped");
    }
    if (file_stamp().empty()) {
        return std::string("the Mesh has no file");
    }
    const std::filesystem::path root = resources_root();
    anarchy::amesh::Data data;
    if (std::optional<std::string> error = read_file(root, path(), data, true)) {
        return error;
    }
    data.piece_recipe = recipe;
    data.pieces = std::move(pieces);
    return write_file(root, path(), data);
}
```

In `Mesh::on_reuse`, add:

```cpp
    std::lock_guard<std::mutex> pieces_lock(pieces_mutex_);
    pieces_stamp_.clear();
    pieces_.clear();
```

Add `#include <chrono>` if it is missing.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[shapes]"`
Expected: `All tests passed`, including the earlier shape tests.

- [ ] **Step 6: Commit**

```bash
git add src/engine_instances/AssetInstances.hpp src/engine_instances/AssetInstances.cpp sandbox/mesh_shapes_tests.cpp
git commit -m "Store a Mesh's convex pieces in its AMESH file

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The memory cache

**Files:**
- Modify: `src/engine_core/ConvexDecomposition.hpp`, `src/engine_core/ConvexDecomposition.cpp`
- Test: `sandbox/convex_decomposition_tests.cpp`

**Interfaces:**
- Consumes: `decompose` (Task 2); `Mesh::file_pieces` (Task 3).
- Produces (namespace `engine_core`):
  - `bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles, std::vector<anarchy::amesh::ConvexPiece>& out)`: true when known, even if known to be empty.
  - `std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles)`
  - `void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles, std::vector<anarchy::amesh::ConvexPiece> pieces)`
  - `std::uint64_t decompose_count()`, `void clear_piece_cache()`

- [ ] **Step 1: Write the failing test**

Append to `sandbox/convex_decomposition_tests.cpp` (add `#include "AssetInstances.hpp"`):

```cpp
TEST_CASE("D2 a mesh decomposes once, then comes from the cache", "[decomposition]") {
    engine_core::clear_piece_cache();
    engine_core::Game game;
    // No Path: a Mesh with no file, so only the cache can know its pieces.
    engine_core::Mesh& mesh = game.create<engine_core::Mesh>();
    const Geometry l = ell();
    std::vector<anarchy::amesh::ConvexPiece> known;
    REQUIRE_FALSE(engine_core::known_pieces(mesh, l.points, l.triangles, known));

    const std::uint64_t before = engine_core::decompose_count();
    const auto first = engine_core::pieces_for(mesh, l.points, l.triangles);
    REQUIRE(engine_core::decompose_count() == before + 1);
    const auto second = engine_core::pieces_for(mesh, l.points, l.triangles);
    REQUIRE(engine_core::decompose_count() == before + 1);
    REQUIRE(second.size() == first.size());
    REQUIRE(engine_core::known_pieces(mesh, l.points, l.triangles, known));

    // Other geometry is not the same entry.
    Geometry moved = l;
    moved.points[0].x += 0.25f;
    REQUIRE_FALSE(engine_core::known_pieces(mesh, moved.points, moved.triangles, known));

    // Known to have none is still known.
    engine_core::remember_pieces(moved.points, moved.triangles, {});
    REQUIRE(engine_core::known_pieces(mesh, moved.points, moved.triangles, known));
    REQUIRE(known.empty());
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile errors for `clear_piece_cache`, `known_pieces`, and the other new names.

- [ ] **Step 3: Implement the cache**

Add to `ConvexDecomposition.hpp` (with `class Mesh;` forward-declared in `namespace engine_core`):

```cpp
// Pieces kept in memory, keyed by the geometry they were made from and
// kRecipe: the last 64 meshes. Any thread.
void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                     std::vector<anarchy::amesh::ConvexPiece> pieces);

// The Mesh's pieces without decomposing: its file's, else the cache's for
// points and triangles (the Mesh's, as vertex_positions gives them). True when
// they are known, even known to be none.
bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                  std::vector<anarchy::amesh::ConvexPiece>& out);

// known_pieces, else decompose now and remember the result.
std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                                    const std::vector<std::uint32_t>& triangles);

// For tests: how many times decompose has run, and forgetting every cached piece.
std::uint64_t decompose_count();
void clear_piece_cache();
```

In `ConvexDecomposition.cpp` (add includes `AssetInstances.hpp`, `<atomic>`, `<cstring>`, `<deque>`, `<mutex>`):

```cpp
namespace {

constexpr std::size_t kCachedMeshes = 64;

std::atomic<std::uint64_t> decompositions{0};

struct Cache {
    std::mutex mutex;
    std::deque<std::pair<std::uint64_t, std::vector<anarchy::amesh::ConvexPiece>>> entries;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

// FNV-1a over the points, the triangles, and the recipe.
std::uint64_t geometry_key(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        }
    };
    for (const Vec3& p : points) {
        const float xyz[3] = {p.x, p.y, p.z};
        mix(xyz, sizeof(xyz));
    }
    mix(triangles.data(), triangles.size() * sizeof(std::uint32_t));
    mix(&kRecipe, sizeof(kRecipe));
    return hash;
}

}  // namespace
```

At the top of `decompose`, before the empty check: `++decompositions;`.

```cpp
void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                     std::vector<anarchy::amesh::ConvexPiece> pieces) {
    const std::uint64_t key = geometry_key(points, triangles);
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    for (auto& entry : kept.entries) {
        if (entry.first == key) {
            entry.second = std::move(pieces);
            return;
        }
    }
    kept.entries.emplace_back(key, std::move(pieces));
    if (kept.entries.size() > kCachedMeshes) {
        kept.entries.pop_front();
    }
}

bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                  std::vector<anarchy::amesh::ConvexPiece>& out) {
    if (mesh.file_pieces(kRecipe, out)) {
        return true;
    }
    const std::uint64_t key = geometry_key(points, triangles);
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    for (const auto& entry : kept.entries) {
        if (entry.first == key) {
            out = entry.second;
            return true;
        }
    }
    out.clear();
    return false;
}

std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                                    const std::vector<std::uint32_t>& triangles) {
    std::vector<anarchy::amesh::ConvexPiece> pieces;
    if (known_pieces(mesh, points, triangles, pieces)) {
        return pieces;
    }
    pieces = decompose(points, triangles);
    remember_pieces(points, triangles, pieces);
    return pieces;
}

std::uint64_t decompose_count() { return decompositions.load(); }

void clear_piece_cache() {
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    kept.entries.clear();
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[decomposition]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ConvexDecomposition.hpp src/engine_core/ConvexDecomposition.cpp sandbox/convex_decomposition_tests.cpp
git commit -m "Keep convex pieces in memory, keyed by the geometry they came from

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: PhysicsWorld builds an unanchored Custom from pieces

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp`, `src/engine_core/PhysicsWorld.cpp`
- Test: `sandbox/physics_tests.cpp`

**Interfaces:**
- Consumes: `pieces_for`, `remember_pieces`, `decompose`, `decompose_count`, `clear_piece_cache`, `kRecipe` (Tasks 2 and 4); `Mesh::store_pieces` (Task 3).
- Produces on `PhysicsWorld`: `float body_mass(InstanceId id) const`, `std::vector<float> shape_frictions(InstanceId id) const`. Internally, `Body::shapes` (`std::vector<b3ShapeId>`) replaces `Body::shape`, and there are `struct Fit`, `fit_of`, and `apply_fit`, which Task 6 reuses.

Box3D is included only here; the pieces arrive as plain points.

- [ ] **Step 1: Write the failing tests**

In `sandbox/physics_tests.cpp`, add `#include "ConvexDecomposition.hpp"`, `<filesystem>`, and this helper inside the anonymous namespace:

```cpp
// An open-topped box, 4 by 3 by 4, its floor half a unit thick and its walls
// too, standing on y = 0 in the Mesh's space.
void add_cup(anarchy::amesh::Data& data) {
    engine_core::add_box(data, Vec3{4.f, 0.5f, 4.f}, Vec3{0.f, 0.25f, 0.f});
    engine_core::add_box(data, Vec3{0.5f, 3.f, 4.f}, Vec3{-1.75f, 1.5f, 0.f});
    engine_core::add_box(data, Vec3{0.5f, 3.f, 4.f}, Vec3{1.75f, 1.5f, 0.f});
    engine_core::add_box(data, Vec3{3.f, 3.f, 0.5f}, Vec3{0.f, 1.5f, -1.75f});
    engine_core::add_box(data, Vec3{3.f, 3.f, 0.5f}, Vec3{0.f, 1.5f, 1.75f});
}

// An unanchored Custom cup of size, its bottom on the floor, and a ball of
// diameter 1 to drop into it at x.
struct CupScene {
    PhysicsObject* cup = nullptr;
    PhysicsObject* ball = nullptr;
    engine_core::Mesh* mesh = nullptr;
};

CupScene cup_scene(PhysicsRig& rig, Vec3 size, float ball_x = 0.f) {
    rig.floor();
    CupScene scene;
    scene.mesh = &rig.game.create<engine_core::Mesh>();
    scene.cup = &rig.body(at(0.f, size.y * 0.5f, 0.f), size, false);
    REQUIRE_FALSE(scene.cup->set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(scene.cup->set_mesh(instance_slot(scene.mesh->id())));
    REQUIRE_FALSE(scene.cup->set_mass(20.0));
    scene.ball = &rig.body(at(ball_x, 8.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(scene.ball->set_shape(static_cast<int>(PhysicsObject::Shape::Sphere)));
    return scene;
}
```

Replace the tail of P16, from `// Unanchored, it falls as a hull…` to the end of the test, and rename the test to `"P16 an anchored Custom collides as its whole mesh, and still falls when unanchored"`:

```cpp
    // Unanchored, it falls as convex pieces of the same mesh, with no warning.
    PhysicsObject& loose = rig.body(at(30.f, 10.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(loose.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(loose.set_mesh(instance_slot(mesh.id())));
    rig.seconds(0.5);
    REQUIRE(y_of(loose.transform()) < 10.f);
    REQUIRE(rig.warnings.empty());
    // Anchoring it makes it its whole mesh, and holds it there.
    loose.set_anchored(true);
    rig.steps(1);
    const float held = y_of(loose.transform());
    rig.seconds(0.25);
    REQUIRE(y_of(loose.transform()) == held);
    REQUIRE(rig.warnings.empty());
```

Add at the end of the file:

```cpp
TEST_CASE("P27 an unanchored Custom cup catches a ball", "[physics]") {
    engine_core::clear_piece_cache();
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.seconds(4.0);
    INFO(y_of(scene.ball->transform()));
    // On the cup's floor: 0.5 up, plus the ball's radius. A single hull would hold it at 3.5.
    REQUIRE(near(y_of(scene.ball->transform()), 1.f, 0.15f));
    REQUIRE(rig.physics.shape_frictions(scene.cup->id()).size() >= 2);
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("P28 a body of pieces weighs its Mass", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.steps(1);
    REQUIRE(near(rig.physics.body_mass(scene.cup->id()), 20.f, 0.01f));
    // Every shape kind still weighs its Mass, with one shape.
    REQUIRE(near(rig.physics.body_mass(scene.ball->id()), 1.f, 0.001f));
    REQUIRE(rig.physics.shape_frictions(scene.ball->id()).size() == 1);
}

TEST_CASE("P29 a Custom whose Mesh has no pieces falls back to a Hull and says so once", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    PhysicsObject& body = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(body.set_mesh(instance_slot(mesh.id())));
    rig.play();
    anarchy::amesh::Data cube;
    engine_core::add_box(cube, Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f});
    std::vector<Vec3> points;
    for (const auto& v : cube.vertices) {
        points.push_back(Vec3{v.p[0], v.p[1], v.p[2]});
    }
    // Known to split into nothing.
    engine_core::remember_pieces(points, cube.indices, {});
    REQUIRE_FALSE(mesh.edit_geometry([&cube](anarchy::amesh::Data& data) { data = cube; }));
    rig.seconds(3.0);
    REQUIRE(near(y_of(body.transform()), 1.f, 0.05f));
    REQUIRE(rig.warnings.size() == 1);
    REQUIRE(rig.warnings.front().find("Custom fell back to Hull") != std::string::npos);
    engine_core::clear_piece_cache();
}

TEST_CASE("P30 Friction set during play reaches every piece", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.steps(1);
    REQUIRE_FALSE(scene.cup->set_friction(0.125));
    rig.steps(1);
    const std::vector<float> frictions = rig.physics.shape_frictions(scene.cup->id());
    REQUIRE(frictions.size() >= 2);
    for (const float friction : frictions) {
        REQUIRE(near(friction, 0.125f, 1e-6f));
    }
}

TEST_CASE("P31 a cup stretched by Size still holds a ball", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{8.f, 3.f, 4.f}, 2.f);
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.seconds(4.0);
    INFO(x_of(scene.ball->transform()) << " " << y_of(scene.ball->transform()));
    REQUIRE(y_of(scene.ball->transform()) < 2.f);
    REQUIRE(std::fabs(x_of(scene.ball->transform())) < 4.f);
}

TEST_CASE("P32 pieces in the Mesh's file are used without decomposing, anchored or not", "[physics]") {
    PhysicsRig rig;
    const std::filesystem::path resources = std::filesystem::temp_directory_path() / "anarchy-physics-pieces-test";
    std::filesystem::remove_all(resources);
    std::filesystem::create_directories(resources);
    rig.game.set_resources_root(resources);
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    // Stopped: the cup goes into the Mesh's file, then its pieces do.
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    std::vector<Vec3> points;
    std::vector<std::uint32_t> triangles;
    REQUIRE_FALSE(scene.mesh->vertex_positions(points, &triangles));
    REQUIRE_FALSE(scene.mesh->store_pieces(engine_core::kRecipe, engine_core::decompose(points, triangles)));
    engine_core::clear_piece_cache();
    const std::uint64_t before = engine_core::decompose_count();

    rig.play();
    rig.seconds(4.0);
    REQUIRE(near(y_of(scene.ball->transform()), 1.f, 0.15f));
    scene.cup->set_anchored(true);
    rig.steps(1);
    scene.cup->set_anchored(false);
    rig.seconds(1.0);
    REQUIRE(near(y_of(scene.ball->transform()), 1.f, 0.15f));
    REQUIRE(engine_core::decompose_count() == before);
    std::error_code ignored;
    std::filesystem::remove_all(resources, ignored);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile errors: `body_mass`, `shape_frictions` are not members of `PhysicsWorld`.

- [ ] **Step 3: Give a body several shapes**

In `PhysicsWorld.hpp`, after `has_body`:

```cpp
    // For tests: the body's mass, and each of its shapes' friction. 0 and
    // empty when the PhysicsObject has no body.
    float body_mass(InstanceId id) const;
    std::vector<float> shape_frictions(InstanceId id) const;
```

In `PhysicsWorld.cpp`, in `struct Body`, replace `b3ShapeId shape = b3_nullShapeId;` with:

```cpp
        // Its shapes: one, or one per convex piece of an unanchored Custom.
        std::vector<b3ShapeId> shapes;
```

and update the `volume` comment to `// The shapes' volume, summed, which turns Mass into a density.`

Add to `Impl`, next to `drop_shape`:

```cpp
    // Keeps shape on the record, when Box3D made one.
    static void add_shape(Body& record, b3ShapeId shape) {
        if (b3Shape_IsValid(shape)) {
            record.shapes.push_back(shape);
        }
    }
```

`drop_shape` becomes:

```cpp
    // Destroys the record's shapes, and the mesh one held, if any.
    static void drop_shape(Body& record) {
        for (const b3ShapeId shape : record.shapes) {
            if (b3Shape_IsValid(shape)) {
                b3DestroyShape(shape, false);
            }
        }
        record.shapes.clear();
        if (record.mesh != nullptr) {
            b3DestroyMesh(record.mesh);
            record.mesh = nullptr;
        }
    }
```

Then go through every remaining use (`grep -n "record.shape\b" src/engine_core/PhysicsWorld.cpp`):
- each `record.shape = b3CreateXShape(...)` → `add_shape(record, b3CreateXShape(...))`, in both the controller shape (around line 1060) and `make_object_shape`
- each `if (!b3Shape_IsValid(record.shape))` → `if (record.shapes.empty())`
- the material and mass updates (around line 1257):

```cpp
            if (rigid != nullptr && (dirty & PhysicsObject::kDirtyMaterial) != 0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetFriction(shape, static_cast<float>(rigid->friction()));
                    b3Shape_SetRestitution(shape, static_cast<float>(rigid->bounciness()));
                }
            }
            if ((dirty & PhysicsObject::kDirtyMass) != 0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetDensity(shape, density(object, record.volume), false);
                }
                b3Body_ApplyMassFromShapes(record.body);
            }
```

Accessors, next to `has_body`:

```cpp
float PhysicsWorld::body_mass(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    return found != impl_->bodies.end() ? b3Body_GetMass(found->second.body) : 0.f;
}

std::vector<float> PhysicsWorld::shape_frictions(InstanceId id) const {
    std::vector<float> frictions;
    const auto found = impl_->bodies.find(id);
    if (found != impl_->bodies.end()) {
        for (const b3ShapeId shape : found->second.shapes) {
            frictions.push_back(b3Shape_GetFriction(shape));
        }
    }
    return frictions;
}
```

Run: `grep -n "b3Body_GetMass\|b3Shape_GetFriction" build/_deps/box3d-src/include/box3d/box3d.h`
Expected: both appear. If a name differs, use the header's name.

Build and run the existing physics tests before going on: `cmake --build build --target sandbox --parallel && ./build/sandbox "[physics]"`. Expect everything but P16 and P27–P32 to pass. This refactor doesn't change behavior.

- [ ] **Step 4: Split the fit**

Replace `fit_points` with:

```cpp
// How a Mesh's points are placed in the body's space: centered on their
// bounds, each axis scaled so the bounds are size, around center.
struct Fit {
    Vec3 middle;
    Vec3 scale;
    Vec3 center;
};

// The fit of mesh_points, which are not empty.
Fit fit_of(const std::vector<Vec3>& mesh_points, Vec3 size, Vec3 center) {
    Vec3 low = mesh_points.front();
    Vec3 high = low;
    for (const Vec3& p : mesh_points) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    auto fit = [](float target, float extent) { return extent > 1e-6f ? target / extent : 1.f; };
    return Fit{Vec3{(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f},
               Vec3{fit(size.x, high.x - low.x), fit(size.y, high.y - low.y), fit(size.z, high.z - low.z)}, center};
}

b3Vec3 apply_fit(const Fit& fit, Vec3 p) {
    return b3Vec3{(p.x - fit.middle.x) * fit.scale.x + fit.center.x, (p.y - fit.middle.y) * fit.scale.y + fit.center.y,
                  (p.z - fit.middle.z) * fit.scale.z + fit.center.z};
}

// A Mesh's points, which are not empty, fitted to size around center in the body's space.
void fit_points(const std::vector<Vec3>& mesh_points, Vec3 size, Vec3 center, std::vector<b3Vec3>& points) {
    const Fit fit = fit_of(mesh_points, size, center);
    points.clear();
    for (const Vec3& p : mesh_points) {
        points.push_back(apply_fit(fit, p));
    }
}

// Each piece, fitted as its whole Mesh is (fit), as a hull. Pieces Box3D
// builds no hull from are left out. The caller destroys the hulls.
std::vector<b3HullData*> piece_hulls(const std::vector<anarchy::amesh::ConvexPiece>& pieces, const Fit& fit) {
    std::vector<b3HullData*> hulls;
    std::vector<b3Vec3> points;
    for (const anarchy::amesh::ConvexPiece& piece : pieces) {
        points.clear();
        for (const auto& p : piece.points) {
            points.push_back(apply_fit(fit, Vec3{p[0], p[1], p[2]}));
        }
        if (b3HullData* hull = build_hull(points)) {
            hulls.push_back(hull);
        }
    }
    return hulls;
}
```

Put `piece_hulls` after `build_hull`. Add `#include "ConvexDecomposition.hpp"` to the includes.

- [ ] **Step 5: Build an unanchored Custom from pieces**

In `make_object_shape`, replace the `Custom` case's unanchored tail (the `warned_custom` warning and `[[fallthrough]]`) so the case reads:

```cpp
        case PhysicsObject::Shape::Custom:
            // Box3D gives a mesh contacts only on a static body, so an
            // unanchored Custom is convex pieces of its mesh instead.
            if (object.anchored()) {
                if (b3MeshData* mesh = make_mesh(game, object, size, record.center)) {
                    record.mesh = mesh;
                    record.volume = size.x * size.y * size.z;
                    def.density = density(object, record.volume);
                    add_shape(record, b3CreateMeshShape(record.body, &def, mesh, b3Vec3{1.f, 1.f, 1.f}));
                }
                break;
            }
            if (make_pieces(game, object, size, record, def)) {
                break;
            }
            [[fallthrough]];
```

Add to `Impl`, after `make_mesh`:

```cpp
    // An unanchored Custom as one hull per convex piece of its Mesh, fitted as
    // its whole Mesh is. False when it has none; with a Mesh that has points,
    // it says so once, and the caller makes it a Hull.
    bool make_pieces(DataModel& game, PhysicsObject& object, Vec3 size, Body& record, b3ShapeDef& def) {
        if (!fitted_mesh(game, object, size, record.center, true).empty()) {
            // No Mesh, or no points: the Hull it falls to says why.
            return false;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(game.instance(object.mesh_id()));
        const std::vector<anarchy::amesh::ConvexPiece> pieces = pieces_for(*mesh, mesh_points, triangles);
        std::vector<b3HullData*> hulls = piece_hulls(pieces, fit_of(mesh_points, size, record.center));
        if (hulls.empty()) {
            if (!object.warned_custom) {
                object.warned_custom = true;
                say("PhysicsObject " + game.name(object.id()) + ": Custom fell back to Hull (" +
                    (pieces.empty() ? "its Mesh split into no convex pieces"
                                    : "Box3D could not build a hull from any piece") +
                    ")");
            }
            return false;
        }
        object.warned_custom = false;
        record.volume = 0.f;
        for (b3HullData* hull : hulls) {
            record.volume += b3ComputeHullMass(hull, 1.f).mass;
        }
        def.density = density(object, record.volume);
        for (b3HullData* hull : hulls) {
            // Box3D copies the hull into the shape.
            add_shape(record, b3CreateHullShape(record.body, &def, hull));
            b3DestroyHull(hull);
        }
        return !record.shapes.empty();
    }
```

In `PhysicsObject.hpp`, update the comment on `warned_custom` to say it is for the "Custom fell back to Hull" warning. Only the comment changes.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[physics]"`
Expected: `All tests passed`. If P27 or P31 rest the ball too high, print `rig.physics.shape_frictions(cup).size()` (the piece count) under `INFO`. Report it rather than loosening the tolerance: it means V-HACD merged the cup's walls, and the recipe's settings need a decision.

- [ ] **Step 7: Run the whole sandbox**

Run: `./build/sandbox`
Expected: `All tests passed`. The player controller tests use the same shape code.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp src/engine_instances/PhysicsObject.hpp sandbox/physics_tests.cpp
git commit -m "Build an unanchored Custom PhysicsObject from convex pieces of its Mesh

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Outlines show the pieces

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp`, `src/engine_core/PhysicsWorld.cpp` (`collision_outline`)
- Modify: `src/runner/GameView.hpp`, `src/runner/GameView.cpp` (`readSelectedBodies`, `BodyOutline`)
- Test: `sandbox/physics_tests.cpp`

**Interfaces:**
- Consumes: `Fit`, `fit_of`, `piece_hulls` (Task 5); `known_pieces` (Task 4); `Mesh::file_stamp` (Task 3).
- Produces: `PhysicsWorld::collision_outline(const PhysicsObject&, Vec3 center, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles, std::vector<Vec3>& lines, float scale = 1.f, const std::vector<anarchy::amesh::ConvexPiece>* pieces = nullptr)`.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/physics_tests.cpp`:

```cpp
TEST_CASE("P33 an unanchored Custom's outline is its pieces when they are known", "[physics]") {
    PhysicsRig rig;
    PhysicsObject& cup = rig.body(at(0.f, 0.f, 0.f), Vec3{4.f, 3.f, 4.f}, false);
    REQUIRE_FALSE(cup.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    anarchy::amesh::Data data;
    add_cup(data);
    std::vector<Vec3> points;
    for (const auto& v : data.vertices) {
        points.push_back(Vec3{v.p[0], v.p[1], v.p[2]});
    }
    const auto pieces = engine_core::decompose(points, data.indices);
    REQUIRE(pieces.size() >= 2);

    std::vector<Vec3> hull_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, hull_lines);
    std::vector<Vec3> piece_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, piece_lines, 1.f, &pieces);
    REQUIRE(piece_lines.size() > hull_lines.size());
    for (const Vec3& p : piece_lines) {
        REQUIRE(std::fabs(p.x) <= 2.05f);
        REQUIRE(std::fabs(p.y) <= 1.55f);
        REQUIRE(std::fabs(p.z) <= 2.05f);
    }
    // Anchored, it is its triangles whatever pieces it has.
    cup.set_anchored(true);
    std::vector<Vec3> anchored_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, anchored_lines, 1.f, &pieces);
    std::vector<Vec3> triangle_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, triangle_lines);
    REQUIRE(anchored_lines.size() == triangle_lines.size());
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile error: too many arguments to `collision_outline`.

- [ ] **Step 3: Draw the pieces**

In `PhysicsWorld.hpp`, extend the declaration and its comment. Add `#include "amesh.hpp"`.

```cpp
    // … an anchored Custom is each edge of its triangles once, an unanchored
    // Custom with pieces (as known_pieces gives them) is each piece's hull,
    // and one that cannot be made is the Box it falls back to. Warns of nothing.
    static void collision_outline(const PhysicsObject& object, Vec3 center, const std::vector<Vec3>& points,
                                  const std::vector<std::uint32_t>& triangles, std::vector<Vec3>& lines,
                                  float scale = 1.f, const std::vector<anarchy::amesh::ConvexPiece>* pieces = nullptr);
```

In `PhysicsWorld.cpp`, update the definition's signature to match, and in the `Custom`/`Hull` case, after the anchored-Custom block and before `b3HullData* hull = build_hull(points);`:

```cpp
        if (object.shape() == PhysicsObject::Shape::Custom && pieces != nullptr && !pieces->empty()) {
            std::vector<b3HullData*> hulls = piece_hulls(*pieces, fit_of(mesh_points, size, center));
            for (b3HullData* hull : hulls) {
                outline_hull(*hull, lines);
                b3DestroyHull(hull);
            }
            if (!hulls.empty()) {
                return;
            }
        }
```

- [ ] **Step 4: Pass the pieces from the Scene View**

In `GameView.hpp`, add `#include "amesh.hpp"`, and `struct BodyOutline` gains:

```cpp
    // The Mesh's file as last read (Mesh::file_stamp), and the convex pieces
    // known for it then, for an unanchored Custom.
    std::string meshStamp;
    std::vector<anarchy::amesh::ConvexPiece> meshPieces;
```

In `GameView.cpp`'s `readSelectedBodies`, add `#include "ConvexDecomposition.hpp"`. Compute the stamp next to `meshRevision`:

```cpp
        const std::string meshStamp = mesh != nullptr ? mesh->file_stamp() : std::string();
```

Add `|| outline.meshStamp != meshStamp` to `meshChanged`. In the `if (meshChanged)` block, set `outline.meshStamp = meshStamp;`, then after reading the points:

```cpp
            // An unanchored Custom draws its pieces when they are known; the outline never decomposes.
            outline.meshPieces.clear();
            if (mesh != nullptr && shape == engine_core::PhysicsObject::Shape::Custom && !outline.meshPoints.empty()) {
                engine_core::known_pieces(*mesh, outline.meshPoints, outline.meshTriangles, outline.meshPieces);
            }
```

Pass `&outline.meshPieces` as the new last argument of the `collision_outline` call.

The studio's queue writes pieces into the file after the outline was made. A changed stamp counts as `meshChanged`, so the outline picks the pieces up without a reselect.

- [ ] **Step 5: Build everything and run the tests**

Run: `cmake --build build --parallel && ./build/sandbox "[physics]"`
Expected: a clean build of every target, then `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp src/runner/GameView.hpp src/runner/GameView.cpp sandbox/physics_tests.cpp
git commit -m "Outline an unanchored Custom as its convex pieces when they are known

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The studio's queue

**Files:**
- Modify: `src/engine_core/ConvexDecomposition.hpp`, `src/engine_core/ConvexDecomposition.cpp`
- Modify: `src/engine_core/Engine.hpp`, `src/engine_core/Engine.cpp`
- Test: `sandbox/convex_decomposition_tests.cpp`

**Interfaces:**
- Consumes: `decompose`, `remember_pieces`, `kRecipe` (Tasks 2 and 4); `Mesh::file_pieces`, `store_pieces`, `file_stamp`, `vertex_positions` (Task 3); `DataModel::physics_bodies`.
- Produces: `class engine_core::ConvexDecomposer { void update(DataModel&); bool idle() const; }`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/convex_decomposition_tests.cpp` (add includes `PhysicsObject.hpp`, `<chrono>`, `<filesystem>`, `<thread>`):

```cpp
namespace {

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A stopped game with a resources folder, a Mesh file holding an L, and the queue.
struct QueueRig {
    SimRole role;
    engine_core::Game game;
    engine_core::ConvexDecomposer decomposer;
    std::filesystem::path resources;
    engine_core::Mesh* mesh = nullptr;

    QueueRig() {
        resources = std::filesystem::temp_directory_path() / "anarchy-decomposer-test";
        std::filesystem::remove_all(resources);
        std::filesystem::create_directories(resources);
        game.set_resources_root(resources);
        mesh = &game.create<engine_core::Mesh>();
        REQUIRE_FALSE(mesh->edit_geometry([](anarchy::amesh::Data& data) {
            engine_core::add_box(data, Vec3{3.f, 1.f, 1.f}, Vec3{1.5f, 0.5f, 0.f});
            engine_core::add_box(data, Vec3{1.f, 2.f, 1.f}, Vec3{0.5f, 2.f, 0.f});
        }));
    }
    ~QueueRig() {
        std::error_code ignored;
        std::filesystem::remove_all(resources, ignored);
    }

    engine_core::PhysicsObject& custom(bool anchored) {
        auto& object = game.create<engine_core::PhysicsObject>();
        REQUIRE_FALSE(object.set_shape(static_cast<int>(engine_core::PhysicsObject::Shape::Custom)));
        REQUIRE_FALSE(object.set_mesh(instance_slot(mesh->id())));
        object.set_anchored(anchored);
        game.set_parent(object.id(), workspace_of(game));
        return object;
    }

    // Waits for the worker, at most ten seconds.
    void wait() {
        for (int tries = 0; tries < 1000 && !decomposer.idle(); ++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(decomposer.idle());
    }

    bool has_pieces() const {
        std::vector<anarchy::amesh::ConvexPiece> found;
        return mesh->file_pieces(engine_core::kRecipe, found);
    }
};

}  // namespace

TEST_CASE("Q1 a stopped update writes a Custom's pieces into its Mesh's file, once per Mesh", "[decomposition]") {
    QueueRig rig;
    rig.custom(true);
    rig.custom(false);
    const std::uint64_t before = engine_core::decompose_count();
    rig.decomposer.update(rig.game);
    rig.wait();
    REQUIRE_FALSE(rig.has_pieces());
    rig.decomposer.update(rig.game);
    REQUIRE(rig.has_pieces());
    REQUIRE(engine_core::decompose_count() == before + 1);
    // Already stored: nothing more to do.
    rig.decomposer.update(rig.game);
    REQUIRE(rig.decomposer.idle());
    REQUIRE(engine_core::decompose_count() == before + 1);
}

TEST_CASE("Q2 a result for a Mesh edited or deleted since is dropped", "[decomposition]") {
    QueueRig rig;
    rig.custom(false);
    rig.decomposer.update(rig.game);
    REQUIRE_FALSE(rig.mesh->edit_geometry([](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{1.f, 1.f, 1.f}, Vec3{5.f, 0.f, 0.f});
    }));
    rig.wait();
    // The stale result is dropped, and the new geometry is queued.
    rig.decomposer.update(rig.game);
    REQUIRE_FALSE(rig.has_pieces());
    rig.wait();
    rig.decomposer.update(rig.game);
    REQUIRE(rig.has_pieces());

    QueueRig other;
    other.custom(false);
    other.decomposer.update(other.game);
    other.game.destroy(other.mesh->id());
    other.wait();
    other.decomposer.update(other.game);
    REQUIRE(other.decomposer.idle());
}

TEST_CASE("Q3 nothing is written while playing; a result waits for Stop", "[decomposition]") {
    QueueRig rig;
    rig.custom(false);
    rig.decomposer.update(rig.game);
    rig.wait();
    const std::string stamp = rig.mesh->file_stamp();
    rig.game.capture_place();
    rig.game.start_simulation();
    // The Engine never updates the queue while playing; Stop brings the result in.
    rig.game.stop_simulation();
    REQUIRE(rig.mesh->file_stamp() == stamp);
    rig.decomposer.update(rig.game);
    REQUIRE(rig.has_pieces());
}

TEST_CASE("Q4 a Mesh that splits into nothing is not queued again until it changes", "[decomposition]") {
    QueueRig rig;
    REQUIRE_FALSE(rig.mesh->edit_geometry([](anarchy::amesh::Data& data) { data = anarchy::amesh::Data{}; }));
    rig.custom(false);
    const std::uint64_t before = engine_core::decompose_count();
    for (int frame = 0; frame < 5; ++frame) {
        rig.decomposer.update(rig.game);
        rig.wait();
    }
    // An empty Mesh has no points: nothing to queue at all.
    REQUIRE(engine_core::decompose_count() == before);
    REQUIRE_FALSE(rig.has_pieces());
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: `ConvexDecomposer` is not declared.

- [ ] **Step 3: Implement the queue**

Add to `ConvexDecomposition.hpp` (includes `types.hpp` for `InstanceId`, `<condition_variable>`, `<deque>`, `<mutex>`, `<string>`, `<thread>`, `<unordered_map>`; forward-declare `class DataModel;` beside `class Mesh;`):

```cpp
// The studio's decompositions: one worker thread that splits the Meshes of
// Custom PhysicsObjects while the place is stopped, so playing finds their
// pieces in their files.
class ConvexDecomposer {
public:
    ConvexDecomposer();
    // Drops work that has not started, and joins the worker.
    ~ConvexDecomposer();
    ConvexDecomposer(const ConvexDecomposer&) = delete;
    ConvexDecomposer& operator=(const ConvexDecomposer&) = delete;

    // On the gameplay thread, with the write lock, while stopped. Writes
    // finished pieces into their Meshes' files when the files are as they
    // were, then queues each Custom's Mesh whose file has no pieces of
    // kRecipe, once per file_stamp.
    void update(DataModel& game);
    // No work waiting or running. Finished work waits for the next update.
    bool idle() const;

private:
    struct Job {
        InstanceId mesh = 0;
        std::string stamp;
        std::vector<Vec3> points;
        std::vector<std::uint32_t> triangles;
        std::vector<anarchy::amesh::ConvexPiece> pieces;
    };
    void work();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> waiting_;
    std::vector<Job> done_;
    bool running_ = false;
    bool stopping_ = false;
    // The file_stamp each Mesh was last queued at. Gameplay thread only.
    std::unordered_map<InstanceId, std::string> queued_;
    std::vector<InstanceId> bodies_;
    std::thread thread_;
};
```

In `ConvexDecomposition.cpp` (includes `DataModel.hpp`, `PhysicsObject.hpp`):

```cpp
ConvexDecomposer::ConvexDecomposer() : thread_([this] { work(); }) {}

ConvexDecomposer::~ConvexDecomposer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        waiting_.clear();
    }
    wake_.notify_all();
    thread_.join();
}

bool ConvexDecomposer::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_.empty() && !running_;
}

void ConvexDecomposer::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !waiting_.empty(); });
        if (stopping_) {
            return;
        }
        Job job = std::move(waiting_.front());
        waiting_.pop_front();
        running_ = true;
        lock.unlock();
        job.pieces = decompose(job.points, job.triangles);
        lock.lock();
        running_ = false;
        done_.push_back(std::move(job));
    }
}

void ConvexDecomposer::update(DataModel& game) {
    std::vector<Job> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished.swap(done_);
    }
    for (Job& job : finished) {
        // Right for its geometry whatever became of the Mesh.
        remember_pieces(job.points, job.triangles, job.pieces);
        auto* mesh = dynamic_cast<Mesh*>(game.instance(job.mesh));
        if (mesh == nullptr || job.pieces.empty() || mesh->file_stamp() != job.stamp) {
            continue;
        }
        if (!mesh->store_pieces(kRecipe, std::move(job.pieces))) {
            // Its own write is not a change to queue again for.
            queued_[job.mesh] = mesh->file_stamp();
        }
    }

    game.physics_bodies(bodies_);
    for (const InstanceId id : bodies_) {
        const auto* object = dynamic_cast<const PhysicsObject*>(game.instance(id));
        if (object == nullptr || object->shape() != PhysicsObject::Shape::Custom) {
            continue;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(game.instance(object->mesh_id()));
        if (mesh == nullptr) {
            continue;
        }
        const std::string stamp = mesh->file_stamp();
        if (stamp.empty()) {
            continue;
        }
        const auto seen = queued_.find(mesh->id());
        if (seen != queued_.end() && seen->second == stamp) {
            continue;
        }
        queued_[mesh->id()] = stamp;
        std::vector<anarchy::amesh::ConvexPiece> stored;
        if (mesh->file_pieces(kRecipe, stored)) {
            continue;
        }
        Job job;
        job.mesh = mesh->id();
        job.stamp = stamp;
        if (mesh->vertex_positions(job.points, &job.triangles)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting_.push_back(std::move(job));
        }
        wake_.notify_one();
    }
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[decomposition]"`
Expected: `All tests passed`.

- [ ] **Step 5: Run the queue from the Engine**

In `Engine.hpp`, include `ConvexDecomposition.hpp` and add after `PhysicsWorld physics_;`:

```cpp
    // Splits Custom PhysicsObjects' Meshes into convex pieces while stopped.
    ConvexDecomposer decomposer_;
```

In `Engine.cpp`'s step lambda, right after the `"Commands"` block:

```cpp
                if (!game_.simulation_running()) {
                    PROFILE_SCOPE("Convex decomposition", profiler::Group::Physics);
                    decomposer_.update(game_);
                }
```

Run: `cmake --build build --parallel && ./build/sandbox`
Expected: a clean build of every target and `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/ConvexDecomposition.hpp src/engine_core/ConvexDecomposition.cpp src/engine_core/Engine.hpp src/engine_core/Engine.cpp sandbox/convex_decomposition_tests.cpp
git commit -m "Decompose Custom meshes on a worker while stopped and store the pieces

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Check it in the studio, and update the docs

**Files:**
- Modify: `docs/superpowers/specs/2026-09-30-box3d-physics-design.md` (Shapes row)
- Modify: `docs/superpowers/specs/2026-10-06-custom-shape-decomposition-design.md` (Build section: no `cmake/vhacd`)

- [ ] **Step 1: Build the app bundle**

Run: `cmake --build build --parallel && ls build/AnarchyStudio.app/Contents/Resources/themes`
Expected: the themes are listed. A bundle without them opens with the default theme.

- [ ] **Step 2: Manual check (one studio)**

Open a scratch copy of a project: `open build/AnarchyStudio.app --args <scratch project>`. Note its pid. In the command bar:

```lua
local m = Instance.new("Mesh", game.Assets.Meshes)
m.Name = "Cup"
m:AddBox(Vector3.new(4, 0.5, 4), Vector3.new(0, 0.25, 0))
m:AddBox(Vector3.new(0.5, 3, 4), Vector3.new(-1.75, 1.5, 0))
m:AddBox(Vector3.new(0.5, 3, 4), Vector3.new(1.75, 1.5, 0))
m:AddBox(Vector3.new(3, 3, 0.5), Vector3.new(0, 1.5, -1.75))
m:AddBox(Vector3.new(3, 3, 0.5), Vector3.new(0, 1.5, 1.75))
local cup = Instance.new("PhysicsObject", workspace)
cup.Size = Vector3.new(4, 3, 4)
cup.Transform = Matrix4.new(0, 5, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)
cup.Shape = Enum.PhysicsShape.Custom
cup.Mesh = m
local ball = Instance.new("PhysicsObject", workspace)
ball.Shape = Enum.PhysicsShape.Sphere
ball.Transform = Matrix4.new(0, 12, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)
```

(`Matrix4.new` takes the position, then the rotation's rows.) Check:
- The studio stays responsive right after the Shape is set.
- With the cup selected, its outline turns into several pieces within a few seconds.
- `ls -la <project>/resources/meshes/` shows the Cup file larger than it was.
- Playing, the ball lands inside the cup, below its rim.

Quit the studio by its pid as soon as these are checked.

- [ ] **Step 3: Update the docs**

In `2026-09-30-box3d-physics-design.md`, the Shapes row gains: "An unanchored Custom is convex pieces of its Mesh (see `2026-10-06-custom-shape-decomposition-design.md`)." In the decomposition spec's Build section, replace `(`CMakeLists.txt`, `cmake/vhacd`)` with `(`CMakeLists.txt`)`, and add: "V-HACD is header-only, so it needs no CMake subdirectory of its own."

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-09-30-box3d-physics-design.md docs/superpowers/specs/2026-10-06-custom-shape-decomposition-design.md
git commit -m "Docs: an unanchored Custom is convex pieces of its Mesh

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
