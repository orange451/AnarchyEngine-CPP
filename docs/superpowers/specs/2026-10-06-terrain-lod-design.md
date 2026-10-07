# Terrain LOD Design

2026-10-06 · sub-project 1d of the terrain roadmap (`2026-10-06-terrain-roadmap-design.md`). Builds on the terrain surface (plan `2026-10-06-terrain-surface.md`: Surface Nets chunk meshes, `TerrainMesher`, `TerrainWorld`, terrain bodies, the terrain shader) and the terrain core spec (`2026-10-06-terrain-core-design.md`).

## Goal

Distant terrain looks nearly the same as it does up close, at a cost that lets one Terrain be about 4 km across (around 32,000 surface chunks, some 130 million triangles at full detail) and still be seen from anywhere on it.

Roblox's distant terrain looks chopped and banded because it makes LODs by coarsening the voxels: surfaces move, thin parts vanish, slopes terrace, and normals from the coarse shape change the lighting. Anarchy simplifies meshes instead, chooses detail by its error on screen, and shades distant terrain from the full-detail voxels. Full detail exists only where it is seen up close or touched by physics.

## Decisions

### Shape of the hierarchy

| Question | Decision |
| --- | --- |
| Hierarchy | An octree of LOD nodes per Terrain. A level-0 node is one chunk. A level-*L* node covers 2^*L* chunks on each side. Levels go up until one node covers the island (about 7 levels for 4 km). Only nodes with surface exist. |
| Level 0 | The chunk's Surface Nets mesh from the terrain surface plan, unchanged. |
| Higher levels | A node's 8 children's meshes, merged and simplified with [meshoptimizer](https://github.com/zeux/meshoptimizer) (MIT, fetched with FetchContent and pinned to a tag, included only by the LOD builder's source) by quadric error metrics. The target error is 0.25 × VoxelSize × 2^*L*. Each level is about a quarter of the triangles of the one below. |
| What a node records | Its mesh, its bounds, its measured geometric error in studs (the largest distance from its surface to the true surface, as meshoptimizer reports it, scaled to studs), and its border edges. |
| Borders between levels | Skirts: along each node's border edges, a strip folded inward along −normal, as deep as twice the node's error (at least one VoxelSize), with the border vertices' normals and materials. Where a node meets a neighbor at another level, the skirt fills the gap, which is under a pixel. Borders simplify freely, so far terrain carries no full-detail seams. |
| Later | The octree and its node records stay as they are if a Nanite-style cluster graph ever replaces skirts as the way levels join. That change would not touch streaming, meshing, or shading. |

### Drawing

| Question | Decision |
| --- | --- |
| Selection | Each frame, on the render thread, each Terrain's octree is walked from the top. A node is drawn when its error projected to the screen is under 1 pixel: `error × (viewport height / (2 × tan(fov / 2))) / distance`, with distance from the camera to the nearest point of the node's bounds. Otherwise its children are tested. A node whose children are not ready yet is drawn instead of them. |
| The threshold | 1 pixel, an engine constant. It may become a graphics quality setting later; it is never a Terrain property. |
| Culling | Nodes outside the view frustum are skipped, children and all. |
| Shadows | Shadow passes draw the nodes the camera selected. |
| Switching levels | When a node is replaced by its children, or the reverse, both are drawn for about 0.25 s with complementary dither patterns in the terrain shader, fading one out as the other fades in. No popping. |
| Shading at distance | A node's vertices take their normal from the full-resolution distance field's gradient at that point, and their material Ids and weights from the full-resolution cells around it, not from the simplified triangles. Lighting and material edges stay where they are as detail drops. |

### Memory

| Question | Decision |
| --- | --- |
| Voxels | Always in RAM, as now. |
| Levels 2 and up | Always in RAM, as compact meshes: 16-bit positions quantized to the node's bounds, normals packed into two bytes (octahedral), material Ids and weights in four bytes each, 16-bit indices. About 1/12 of the full-detail triangle count: around 100–150 MB for a 4 km island. |
| Levels 0 and 1 | Only near the camera: built from voxels as the camera approaches, by the mesher pool, nearest first; dropped when the camera leaves, with hysteresis so a camera at a boundary does not rebuild repeatedly. |
| GPU | Holds the nodes being drawn, and one ring of finer nodes around the camera ready for the next step. An upload not drawn for a few seconds is freed. |

### Building and edits

| Question | Decision |
| --- | --- |
| First build | Coarse levels are built from finer ones, so the first build meshes every chunk once: about 15 s in the background for a 4 km island. Chunks near the camera come first. Far terrain appears region by region as each region's levels finish; until then it is not drawn. Full-detail meshes far from the camera are dropped once their parents exist. |
| Disk cache | Not now. Later, saving levels 2 and up beside the `.avox` would make the first build instant. The `.avox` format is not changed by this. |
| An edit | The edited chunks are re-meshed as in the surface plan. Their ancestors are marked stale and rebuilt bottom-up on the mesher pool. A level-1 rebuild re-meshes from voxels any of its 8 children not in memory. Higher levels use their siblings in RAM. |
| Bursts | A stale node is rebuilt at most once every 100 ms, so a brush stroke or a digging script does not rebuild a node per edit. |
| Edits out of view | Ancestors are rebuilt the same way, so what is visible from afar stays right. |
| Stop | A Stop's restore marks only the chunks it changed, as now; their ancestors follow. |

### Physics

| Question | Decision |
| --- | --- |
| Where colliders exist | Full-detail chunk colliders (the surface plan's chunk shapes) exist only within 3 chunks (96 × VoxelSize studs) of a dynamic PhysicsObject or a PlayerController. They are built as bodies approach and dropped some seconds after the last one leaves. Anchored bodies need none. This replaces the surface plan's "a shape for every chunk". |
| No falling through | A body with no terrain colliders around it (spawned, teleported, or unanchored) has them built in that physics sync, synchronously, before Box3D steps. |
| Raycasts at any distance | `workspace:Raycast` casts against Box3D, then, for each Terrain the ray crosses where there are no colliders, marches the voxel distance field along the ray. Each step can advance by the stored distance; the last step finds the zero crossing. It takes the nearest of all hits. A terrain hit reports the voxel's Material as a collider hit would. The answer does not depend on which colliders are loaded. |

### Out of scope

A Nanite-style cluster graph, a disk cache of LOD meshes, a graphics quality setting for the threshold, and LOD for colliders.

## Architecture

| Unit | What it does | Depends on |
| --- | --- | --- |
| `terrain/LodNode` | A node's compact mesh, bounds, error, border edges; packing and unpacking | `amesh::Data` |
| `terrain/LodBuilder` | Merge 8 children, simplify to a target error, measure the error, re-shade from full-resolution voxels, add skirts. Pure; runs on mesher workers. The only file that includes meshoptimizer | meshoptimizer, `VoxelVolume` (read-only chunks) |
| `terrain/LodTree` | One Terrain's octree: which nodes exist, which are stale, which levels 0–1 are resident, which need building next for a camera position | `LodNode` |
| `TerrainWorld` (extended) | Feeds level-0 meshes and camera positions to the trees, queues LOD builds and chunk meshes on the pool, publishes each Terrain's drawable nodes | `LodTree`, `TerrainMesher` |
| `PhysicsWorld` (extended) | Collider streaming around dynamic bodies; synchronous colliders for a body without them; voxel ray marching for terrain without colliders | `TerrainWorld`, `VoxelVolume` |
| Renderer (extended) | Per-frame selection by pixel error, frustum culling of nodes, dithered cross-fades, a node upload cache with a few seconds' grace | `TerrainView` nodes |

The snapshot carries each Terrain's node set behind a shared pointer with a revision, as it carries chunk lists now. Selection runs on the render thread, which knows the viewport and camera. The simulation side only decides what exists and is resident.

## Testing

- **Simplification:** for a rolling test surface, each node's error measured against the true surface is within its budget; the triangle count falls by about 4× per level.
- **Skirts:** every border edge of every node has a skirt. A render check of a ball across a level boundary, from a moving camera, finds no background pixels through the seam.
- **Selection:** the pixel-error formula against hand-worked cases; moving the camera changes the selection only where expected; frustum culling drops nodes behind the camera.
- **Shading:** a LOD node's vertex normals and Ids match the full-resolution field at those points.
- **Memory:** moving the camera across a large synthetic island loads levels 0–1 near it and drops them behind it; RAM for levels 2 and up stays within the budget per triangle.
- **Edits:** an edit marks and rebuilds every ancestor; a burst of 50 edits to one chunk rebuilds each ancestor at most once per 100 ms window.
- **Physics:** a box dropped far from the camera gets colliders and lands; a body teleported onto bare terrain does not fall through; a ray at distant terrain with no colliders hits the same point and Material as with colliders loaded.
- **Benchmarks** (Release): selection under 0.5 ms per frame for a 4 km island; building every level of a 4,096-chunk island under 5 s.

## Implementation notes

Task 1 (`terrain/LodNode`, meshoptimizer) fixed five things the decisions above leave open:

- **meshoptimizer's fetch.** FetchContent pinned to tag `v0.22` (`https://github.com/zeux/meshoptimizer.git`), built by `cmake/meshoptimizer`'s own `CMakeLists.txt` rather than the library's: it globs `src/*.cpp` into a STATIC lib, exposes `src` as a SYSTEM include, and builds `/W3` with `/O2` even in Debug -- the same shape as `cmake/zstd`'s wrapper. `engine_core` links it `PRIVATE`; nothing calls it yet (that starts with `LodBuilder`, the only file that will include its header).
- **The compact RAM budget (ruling R1).** The plan's "12 B/vertex" was an arithmetic error. The actual format is 16 B/vertex -- 6 B position (3 x u16) + 2 B normal (octahedral) + 4 B material Ids + 4 B weights -- plus 6 B/triangle for 16-bit indices (12 B/triangle when a node is large enough to need `indices32`, at 4 B per index). `CompactMesh::bytes()` counts whichever index vector is in use.
- **NodeKey's arithmetic.** `node_of` floor-divides (not truncates) a chunk coordinate by 2^level component-wise, matching `VoxelChunk.cpp`'s own `floor_div` for `chunk_of`, so negative chunk coordinates land in the node a truncating divide would place one node too far from the origin. `parent_of` floor-divides a node's coordinates by 2 at level + 1; `children_of` is its inverse at level - 1 (`children_of(parent_of(k))` always contains `k`). `node_bounds` at level 0 is exactly a chunk's 32-cell box (`kChunkSize * voxel_size` on a side); at level *L* it is `kChunkSize * 2^L * voxel_size`.
- **pack()'s quantization box (ruling R2).** `pack()` quantizes strictly to the `[bounds_min, bounds_max]` box its caller passes -- it does not compute its own bounds from the mesh. Callers (the LOD builder, from Task 2 on) pass the union of the node's own bounds and the mesh's AABB, since Surface Nets' boundary vertices and skirts can sit outside the node's box. A degenerate axis (`bounds_max == bounds_min` on that axis, e.g. a flat node) quantizes to 0 on that axis instead of dividing by zero.
- **What `ids` and `weights` carry.** `amesh::Vertex::rgba` is already 4 bytes, so `CompactMesh::ids` copies it verbatim (exact, no quantization) rather than reusing the skinning `bone` field -- Surface Nets meshes carry their material Id in `rgba[0]` (`rgba[1..3]` unused today, reserved for future material blending) and have no bones. `weights` quantizes `amesh::Vertex::weight` (float 0..1) to a byte each, `/255` on the way back; for an unskinned mesh these round-trip to 0 exactly. `unpack()` leaves tangent, uv and bone at `Vertex`'s defaults, since `pack()` never carried them, and sets the AABB from the unpacked positions via `amesh::compute_aabb`.
