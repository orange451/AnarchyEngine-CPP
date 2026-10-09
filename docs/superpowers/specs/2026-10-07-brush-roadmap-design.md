# Brush Roadmap

2026-10-07 · the overview that the Brush specs hang from. Each sub-project below gets its own spec, plan, and implementation, in order.

## Goal

Precision geometry in the style of TrenchBroom: a `Brush` instance is one convex solid with a Material and texture alignment on each face, built and edited exactly on a grid. Terrain makes organic land; Brushes make architecture (walls, floors, stairs, trims) that line up and seal perfectly.

Three rules decide every trade-off:

1. **It just works.** No user-facing limit that TrenchBroom does not have. Engine limits (Box3D's 128-vertex hulls, draw-call cost) are handled inside the engine and never shown.
2. **Hyper-optimal.** Runs on low-end hardware: thousands of brushes cost a few hundred draws, memory goes to geometry only, work happens only where something changed.
3. **Looks great and modern.** Faces use ordinary PBR Materials with normal maps, the same look settings Terrain uses.

## How TrenchBroom models a brush, and what we keep

Read from TrenchBroom's source (`lib/TbMdlLib`: `Brush.h`, `BrushFace.h`, `BrushBuilder.h`):

| TrenchBroom | Anarchy |
| --- | --- |
| A brush is `std::vector<BrushFace>`; the polyhedron (`BrushGeometry`) is derived and cached | Same |
| A face is three points (doubles), the plane derived from them, a material name, a UV system, and offset/scale/rotation | Same, with a Material reference instead of a name |
| Geometry: clip a world-sized box by each face's plane; faces that cut nothing are dropped | Same |
| Vertex edits change the polyhedron, then rewrite faces from it (`updateFacesFromGeometry`) | Same (sub-project 2) |
| UV systems: Paraxial (Quake) and Parallel (Valve 220) | Parallel only; Paraxial is converted on `.map` import |
| No face or vertex limit | No face or vertex limit |
| A brush belongs to an entity (worldspawn or a brush entity) | A Brush is an instance: a `PhysicsBase` in the tree |

## Sub-projects

| # | Name | Spec | Depends on |
| --- | --- | --- | --- |
| 1 | Brush core | `2026-10-07-brush-core-design.md` | `workspace:Raycast` (`2026-10-06-edit-mode-physics-raycast-design.md`) |
| 2 | Brush editing tools | to be written | 1 |
| 3 | Face texturing | to be written | 1; Material `TextureScale` (terrain textures, or added by 1) |
| 4 | CSG and `.map` import | to be written | 1, 2 |
| 5 | Larger batching | to be written, only if profiling asks for it | 1 |

### 1. Brush core

The `Brush` class (a `PhysicsBase`), its faces, the shape builder, drawing with per-face Materials and anchored brushes baked into cells, its body (one exact hull, or several exact pieces past Box3D's limit), `RaycastResult.Face`, the Lua API (`BrushFace` datatype and Brush methods), saving, undo, and Insert in Studio. Brushes are moved and rotated with the existing Draggers.

### 2. Brush editing tools

A Studio brush mode: grid snapping, drawing a box by dragging, moving faces, edges, and vertices (TrenchBroom's `transformVertices`/`transformEdges`/`transformFaces`, `addVertex`, `removeVertices`, `snapVertices`), the clip tool, and reshaping a brush's faces by a matrix (`transform` with lock), as the scale tool needs. Multi-brush builders: arch, arch spandrels, hollow cylinder.

### 3. Face texturing

Clicking a face (through `RaycastResult.Face`) to apply a Material; a face panel for Offset, Scale, Rotation; tools to reset axes, rotate, shear, flip, fit, and align; texture lock when moving or reshaping; copying a face's texturing to matching faces (`cloneFaceAttributesFrom`).

### 4. CSG and `.map` import

Subtract, intersect, hollow (built on `Expand`), merge; brush contains and intersects tests. Import of TrenchBroom `.map` files: Valve 220 faces read directly, Quake faces converted from Paraxial; texture names matched to project Materials by name.

### 5. Larger batching

Core already batches anchored brushes into 64-unit cells, one draw per Material per cell. If profiling of very large maps asks for it: merging neighbouring cells into larger draws at a distance, and multi-draw indirect where the hardware has it.
