# Custom Shape Decomposition Design

2026-10-06 · builds on the Box3D physics design (`2026-09-30-box3d-physics-design.md`), the `Custom` value of `Enum.PhysicsShape`, and the AMESH format (`src/amesh/amesh.hpp`).

## Goal

An unanchored `Custom` PhysicsObject collides as its Mesh, concave parts included, instead of as one convex hull around it. Box3D gives a triangle mesh contacts only on a static body, so a moving Custom is built from several convex pieces that together follow the Mesh. The pieces come from a convex decomposition, which is slow, so its result is stored in the Mesh's AMESH file and read back from there.

The user does not choose between two shapes. `Custom` means "collide as this Mesh" whether the PhysicsObject is anchored or not:

- **Anchored:** a Box3D mesh shape of every triangle, as today.
- **Unanchored:** one hull shape per convex piece, all on one body.

## Decisions

| Question | Decision |
| --- | --- |
| Which shapes decompose | `Custom` only. `Hull` stays one hull: the cheap choice, made on purpose. |
| Decomposer | [V-HACD 4](https://github.com/kmammou/v-hacd) (`include/VHACD.h`, one header, BSD-3-Clause), fetched with FetchContent and pinned to a commit. CoACD fits better but brings OpenVDB, TBB, and Eigen. |
| Who includes V-HACD | `engine_core/ConvexDecomposition.cpp` only. It knows nothing of Box3D, as Box3D stays in `PhysicsWorld.cpp`. |
| Decomposition settings | Fixed, not exposed: at most 32 pieces, 100,000 voxels, at most 64 points per piece, 1% volume error, flood fill, shrink wrap on. The set is named by one number, the *recipe*, `kRecipe = 1`. Changing any setting bumps it. |
| Where pieces are stored | The Mesh's AMESH file, in a new section behind a new flag. AMESH goes to version 1.1. |
| What is stored | Each piece's points in the Mesh's own space (as its vertex positions are), not Box3D's `b3HullData`, whose layout is Box3D's to change. Hulls are rebuilt from the points with `b3CreateHull` when a body is made, which takes about a millisecond for 32 pieces. |
| When the studio decomposes | While stopped, each Engine step scans `DataModel::physics_bodies` for a Custom PhysicsObject with a Mesh whose file has no pieces of the current recipe. One scan covers setting Shape or Mesh, undo, paste, and opening a project. Anchored or not, since a script can unanchor it during play. |
| Where the studio decomposes | On one worker thread. The body uses the single hull until the pieces arrive. The result is written into the file on the main thread. |
| When play decomposes | When an unanchored Custom's body is made and its Mesh has no pieces of the current recipe, in its file or in memory. This happens on the simulation thread and stalls it, which is accepted for now. The result is cached in memory and never written to the file. |
| Session geometry | A Mesh edited during play (MeshShapes) has no file pieces that match it. It decomposes at play time into the memory cache, as above. |
| Geometry edits | `Mesh::edit_geometry` drops the pieces when it rewrites the file, since they no longer match. Reimport writes a fresh file without them. |
| Imported meshes with LODs | `store_pieces` keeps the file's LODs and everything else it holds; only the pieces change. Pieces come from the finest LOD, as `vertex_positions` gives it. |
| Undo | Writing pieces is not an undo step. Undoing the Shape change leaves them in the file, where they are a harmless cache. |
| Mass | Each piece's density is `Mass / (sum of the pieces' volumes)`, so the body's mass is `Mass` exactly. Overlap between pieces is counted twice in that sum, which can shift the center of mass slightly; accepted. |
| Fallback | No pieces (decomposition failed, or Box3D built no hull from any piece) falls back to the single hull, then to Box, each warning once as `Hull` does today. |
| Skinned meshes | Decomposed in their bind pose, as `vertex_positions` gives them. Nothing special. |
| Out of scope | User-facing decomposition settings, a progress indicator, decomposing `Hull`, moving the play-time decomposition off the simulation thread. |

## Architecture

### 1. AMESH 1.1 (`amesh/amesh.{hpp,cpp}`)

`kVersionMinor` becomes 1. The reader accepts 1.0 and 1.1. A 1.0 file with `FLAG_HULLS` set is rejected. `write()` writes 1.1 only when there are pieces and 1.0 otherwise, so a mesh without pieces stays readable by builds from before pieces, and a file that loses its pieces goes back to 1.0.

The header's two reserved words get names and keep their offsets, so the header stays 64 bytes:

| Field | Was | Rule |
| --- | --- | --- |
| `piece_count` (u32) | `reserved1` | 0 when `FLAG_HULLS` is clear; else 1 to `kMaxPieces` (256) |
| `piece_point_total` (u32) | `reserved2` | 0 when `FLAG_HULLS` is clear; else the sum of the pieces' point counts |

`FLAG_HULLS = 1u << 6` joins `kKnownFlags`. When it is set, a section follows the subsets and comes before the CRC:

```
u32   recipe                                   4 bytes
u32   piece_points[piece_count]                4 bytes each; each 4 to kMaxPiecePoints (128)
f32   points[piece_point_total][3]            12 bytes each, finite
```

A file without pieces is byte-for-byte as large as before. The header comment's layout table gains the section.

`Data` gains:

```cpp
struct ConvexPiece {
    std::vector<std::array<float, 3>> points;  // the Mesh's own space
};
std::uint32_t piece_recipe = 0;      // meaningful only with pieces
std::vector<ConvexPiece> pieces;     // empty: FLAG_HULLS clear
```

`write()` sets `FLAG_HULLS` exactly when `pieces` is not empty, and throws `AEMeshError` for a count, a point count, or a point outside the rules above. `amesh_self_test` covers a round trip with pieces.

### 2. `ConvexDecomposition` (`engine_core/ConvexDecomposition.{hpp,cpp}`)

```cpp
namespace engine_core {
inline constexpr std::uint32_t kRecipe = 1;
// The pieces of a mesh, in its own space, by V-HACD with the recipe's settings.
// Empty when V-HACD finds none. Thread-safe; takes seconds on a large mesh.
std::vector<amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                          const std::vector<std::uint32_t>& triangles);
}
```

V-HACD's own async mode stays off: callers choose the thread.

`ConvexDecomposition` also owns two things the rest of the engine shares:

- **The memory cache.** It holds pieces keyed by a 64-bit hash of the points, the triangles, and `kRecipe`, and keeps the last 64 meshes it was given. The play path and the studio both fill it, so ten PhysicsObjects on one Mesh decompose once.
- **The studio queue**, `ConvexDecomposer`, owned by the Engine. `update(DataModel&)` runs once per Engine step while stopped, under the step's write lock. It first collects finished work: if the Mesh still exists and its `file_stamp()` is unchanged, it calls `Mesh::store_pieces` and adds the result to the memory cache; a stale result is dropped. Then it scans `physics_bodies` for Custom PhysicsObjects with a Mesh. For each Mesh with a file, no pieces of `kRecipe` in it, and no work queued for its current stamp, it reads the Mesh's points and triangles on this thread and queues them for its one worker thread. A Mesh is queued at most once per stamp, so a Mesh whose decomposition finds nothing is not retried every frame. The destructor drops unstarted work and joins the thread.

Shared helpers, used by PhysicsWorld and the Scene View outline:

```cpp
// The Mesh's pieces without decomposing: its file's, else the memory cache's.
bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points,
                  const std::vector<std::uint32_t>& triangles, std::vector<amesh::ConvexPiece>& out);
// known_pieces, else decompose now and add the result to the cache.
std::vector<amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                           const std::vector<std::uint32_t>& triangles);
```

### 3. `Mesh` (`engine_instances/AssetInstances.{hpp,cpp}`)

```cpp
// The pieces in the AMESH file, when it has some of this recipe and this
// session made no geometry. Cached on the file's time on disk, as bounds is.
// Needs the DataModel lock; a read lock is enough.
bool file_pieces(std::uint32_t recipe, std::vector<amesh::ConvexPiece>& out) const;

// Reads the file, sets its pieces, and writes it back beside and renamed over,
// as edit_geometry does, keeping its LODs. Refused while playing or with no
// file. Not an undo step. Returns why nothing changed.
std::optional<std::string> store_pieces(std::uint32_t recipe, std::vector<amesh::ConvexPiece> pieces);

// What the file is now: its Path, time on disk, and size, as one string.
// Empty with no Path or no file.
std::string file_stamp() const;
```

`edit_geometry` clears `pieces` before it writes. The atomic write that `edit_geometry` uses becomes a private helper that both methods share.

### 4. `PhysicsWorld` (`engine_core/PhysicsWorld.cpp`)

**Several shapes per body.** `Body::shape` becomes `std::vector<b3ShapeId> shapes`. Every site that touches it loops: `drop_shape`, friction and bounciness updates, density updates, and the contact and probe lookups. Every piece's `userData` is the PhysicsObject's id, as a single shape's is now.

**Pieces for an unanchored Custom.** Lookup order:

1. the Mesh's `file_pieces(kRecipe)`
2. the memory cache
3. `decompose()` on the simulation thread, with the result added to the cache

Every piece goes through the same fitting as the whole Mesh. `fit_points` splits into a fit taken from the bounds of every Mesh point and the application of that fit, so a piece is scaled and centered by the Mesh's bounds and not by its own. Each fitted piece then goes through `build_hull` (64 points, retry with 32). A piece Box3D builds no hull from is skipped. Densities are set as in Decisions, then `b3CreateHullShape` runs once per piece.

The existing once-only warning ("a Custom collides as its whole mesh only while Anchored…") is removed. A Custom with no usable pieces warns once: `PhysicsObject <name>: Custom fell back to Hull (<reason>)`. It then falls through to the existing Hull path, whose Box fallback still applies.

**Anchoring.** Toggling Anchored on a Custom already drops the shape and makes a new one. It now drops every piece. Once the pieces are cached, unanchoring costs only the hull builds.

**Outlines.** An unanchored Custom draws each piece's hull edges when pieces can be had without decomposing (`known_pieces`). Otherwise it draws the single hull as today. Drawing an outline never starts a decomposition. `collision_outline` takes the pieces as a new argument, which `GameView` fills.

**For tests.** `float body_mass(InstanceId) const` and `std::vector<float> shape_frictions(InstanceId) const`: the body's mass and each of its shapes' friction, 0 and empty with no body.

### 5. Engine (`engine_core/Engine.{hpp,cpp}`)

The Engine owns a `ConvexDecomposer` and calls `update(game_)` in its step, under the write lock, after commands are drained, while `simulation_running()` is false. PhysicsObject itself is unchanged.

### 6. Build (`CMakeLists.txt`)

V-HACD is header-only, so it needs no CMake subdirectory of its own. V-HACD is fetched with FetchContent and compiled once, in `ConvexDecomposition.cpp` with `ENABLE_VHACD_IMPLEMENTATION`. It is linked PRIVATE to `engine_core`, and its include directory is SYSTEM. It must build with the engine's oldest toolchains, MSVC 19.23 and Apple clang 13 / libc++ 13. The plan's first task proves that before anything depends on it.

## Testing

**`tests/AmeshTest.cpp`**
- A1: Data with three pieces round-trips, with points, counts, and recipe intact. The file is 1.1 with `FLAG_HULLS` set.
- A2: A 1.0 file (the writer's output for a mesh without pieces) reads, with no pieces.
- A3: A 1.0 file with `FLAG_HULLS` set is rejected, and so are `piece_count` above 256, a piece with fewer than 4 or more than 128 points, a `piece_point_total` that disagrees with the counts, and a non-finite point.
- A4: Data without pieces writes a 1.0 file of the same size as before pieces, with both piece words 0.

**`sandbox/convex_decomposition_tests.cpp` (new)**
- D1: An L of two boxes gives at least two pieces, each with at most 64 points, all within the mesh's bounds.
- D2: The same points and triangles twice hit the memory cache the second time; a different mesh does not.

**`sandbox/mesh_shapes_tests.cpp`**
- M1: `store_pieces` then `file_pieces(kRecipe)` returns them, and the file is 1.1.
- M2: `file_pieces` with another recipe finds none.
- M3: `AddBox` after `store_pieces` leaves the file with no pieces.
- M4: `store_pieces` while playing is refused, and the file is unchanged.
- M5: `store_pieces` on a file with LODs keeps them.
- Q1: A stopped `update` decomposes a Custom's Mesh on the worker and a later `update` writes the pieces into the file. Two PhysicsObjects on one Mesh queue it once.
- Q2: A result whose Mesh was edited, or deleted, before it was collected is dropped.

**`sandbox/physics_tests.cpp`**
- P1: An unanchored Custom of an open-topped hollow box (a cup) catches a small sphere dropped into it: the sphere comes to rest below the cup's rim. As a single hull, the sphere would rest on top.
- P2: The cup's body mass equals its Mass.
- P3: Toggling the cup's Anchored during play, both ways, keeps it colliding as its Mesh. With pieces in its file, `decompose()` is not called.
- P4: A Custom whose Mesh gives no pieces falls back to a hull and warns once.
- P5: Friction set during play reaches every piece.

**Manual (studio):** set a PhysicsObject to Custom with a cup Mesh. The studio keeps responding while it decomposes; the outline then shows the pieces, and the `.amesh` file has grown. Play and drop a ball in.
