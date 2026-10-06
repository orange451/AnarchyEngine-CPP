# Terrain Roadmap

2026-10-06 · the overview that the terrain specs hang from. Each sub-project below gets its own spec, plan, and implementation, in order.

## Goal

A `Terrain` instance that is better than Roblox's: any number of them in Workspace, each an independent "island" with its own voxels, sculptable into any shape (hills, valleys, cliffs, overhangs, tunnels), painted with any Material in the project, and editable by scripts at runtime.

Two rules decide every trade-off:

1. **It just works.** The user never thinks about texture sizes, palettes, chunk files, or meshes. Whatever the engine needs internally (texture arrays, a material palette, a data file) is built and kept in step by the engine and hidden from the user.
2. **Super efficient, still stunning.** Work happens only where something changed, off the simulation thread where it can, and memory goes only to the surface of an island.

## How others do it, and why we differ

| Engine | Shape data | Overhangs and caves | Materials |
| --- | --- | --- | --- |
| Unity Terrain | Heightmap tiles | No | A few painted layers (splat maps) |
| Unreal Landscape | Heightmap components | No | Painted weight layers into one landscape material |
| Roblox | One global grid, occupancy 0..1 per cell | Yes | A fixed built-in list |
| **Anarchy** | **One sparse grid per Terrain, signed distance per cell** | **Yes** | **Any project Material** |

Heightmaps cannot make overhangs or tunnels, so Anarchy is volumetric. It stores a signed distance (how far the cell is from the surface, in studs) instead of Roblox's occupancy. Occupancy is a distance clamped to a one-cell band, which is why smoothing erodes and grow/shrink is guesswork in Roblox. A distance clamped to a wider band costs the same byte per cell and makes add, subtract, smooth, grow, shrink, blends, and future resampling exact and stable.

## Sub-projects

| # | Name | Spec | Depends on |
| --- | --- | --- | --- |
| 0 | Edit-mode physics and `workspace:Raycast` | `2026-10-06-edit-mode-physics-raycast-design.md` | — |
| 1 | Terrain core | `2026-10-06-terrain-core-design.md` | 0 |
| 2 | Sculpt tools and terrain undo | to be written | 1 |
| 3 | Multi-material rendering | to be written | 1 |
| 4 | Extras: LOD, generators, water, resampling | to be written | 1 |

### 0. Edit-mode physics and `workspace:Raycast`

The Box3D world exists while stopped. Bodies are kept in step with the tree but not simulated, as if everything were anchored. `workspace:Raycast` casts against it in edit and play mode alike. Terrain, the sculpt tools, and later Studio click-to-select all ray-cast through it.

### 1. Terrain core

The Terrain instance; hidden `TerrainMaterial` children that choose which Materials an island may use (at most 255; several may share one Material), edited in a Configure Terrain tab; sparse 32³ chunks of a 1-byte distance and a 1-byte material Id per cell, copy-on-write; Surface Nets meshing on a worker thread; chunk meshes drawn with each material's flat color; one static Box3D body per Terrain with a mesh shape per chunk; the Lua API; the `.avox` file. See its spec.

### 2. Sculpt tools and terrain undo (decided so far)

- A Luau Studio plugin, as `MoveTool.luau` is, aiming with `workspace:Raycast`.
- Brushes: Add, Subtract, Grow, Shrink, Smooth, Flatten, Paint. Brush shapes: ball, block, cylinder. Size and strength.
- Smooth and Grow/Shrink become `VoxelVolume` operations and Lua methods here.
- Undo: a new `ChangeHistoryService` mutation kind holding, per touched chunk, its chunk pointer before and after. Copy-on-write chunks (sub-project 1) make this a pointer swap, not a copy.

### 3. Multi-material rendering (decided so far)

- The user only assigns Materials. The engine builds hidden texture arrays from them, resizing every map to one common size (with mipmaps) so mismatched textures just work. Nothing about this is a property.
- Triplanar mapping, so cliffs and overhangs do not stretch.
- Up to four materials blend per vertex. Sub-project 1 already writes a vertex's material Ids into the unused bone-index channel and their weights into the bone-weight channel, so the mesh format does not change.
- The Configure Terrain tab shows the texture memory the configured materials cost.
- Per-material custom shaders (a `Shader` property on TerrainMaterial): chunks split their triangles into one draw per shader. This spec must decide how the edge between two shaders blends.

### 4. Extras (candidates, not committed)

- Level of detail for large islands, with seams stitched between levels.
- Generators (noise, heightmap import) and an Erode brush.
- Water.
- A Studio "Resample" action that changes VoxelSize (sub-project 1 keeps the data resample-ready: distances are in studs and the file records VoxelSize).
- `Terrain:Flush()`, to finish pending chunk colliders synchronously, if a script needs one.
- A smooth-union blend argument on the Fill methods.
- Per-material friction (Box3D carries a material per triangle).
- Compressing idle chunks in memory.
