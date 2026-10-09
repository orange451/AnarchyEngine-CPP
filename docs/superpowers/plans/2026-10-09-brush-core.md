# Brush Core Implementation Plan

**Goal:** the `Brush` instance of `2026-10-07-brush-core-design.md`, then a Scene View brush editor (the start of sub-project 2) that just works.

**Spec:** `docs/superpowers/specs/2026-10-07-brush-core-design.md`; UX research in `docs/superpowers/specs/2026-10-09-brush-ux-research.md`.

## What the code already says (changes to the spec)

| Spec says | Code says | Plan |
| --- | --- | --- |
| One undo step per method call | A step exists only while a recording is open (IDE command, `TryBeginRecording`, a tool stroke). | Brush face edits note one `Custom` mutation per open recording, holding the old face list, as `Terrain::note_voxel_history` does. Scripts get steps the way they do for every other property. |
| Assigning `Faces` raises "use SetFaces" | `writable = false` gives the fixed message `cannot set Faces`; load, Stop, undo and paste still write through the field. | `lua_hidden` + `writable = false`. The message is `cannot set Faces`. |
| `Faces` saves as a JSON array | A saved property holds one of the slot kinds; there is no JSON kind. | `Faces` is a hidden saved `string` holding the compact JSON text. |
| CanCollide on a Brush | No PhysicsObject has one; only Terrain. | Brush gets its own `CanCollide` (default true). |
| Shared VBO with `glDrawElementsBaseVertex` | Not in the GL loader; culling takes bounds only from a whole `GpuMesh`. | Add it to `gl.hpp/.cpp`. Brush draws carry their own bounds (a new `MeshDraw` bounds override), and shadows draw ranges. |
| Selection outline uses the mesh | The outline pass draws world line segments only. | A selected Brush outlines its edges (exact, cheap, and what a brush editor wants). |
| Insert in front of the camera | Insert parents into Workspace at the default Transform. | The Brush tool's draw-a-box is the main way in; Insert keeps the default 4-unit box at the origin. |
| Plugin for editing | Plugins cannot draw lines, cast rays from the mouse, or capture the mouse. | The editor is C++ in `src/runner/` beside `TerrainBrush`, as the terrain prototype is. |

## Tasks

Each task builds, adds tests to `sandbox` (`[brush]`), and keeps every existing test passing.

1. **Geometry** — `src/engine_core/brush/BrushGeometry.{hpp,cpp}`: faces, plane build with weld and exact refinement, edits, shape builders, hull pieces, render mesh, face lookup. Tests BG*.
2. **BrushFace datatype** — `engine_datatypes`-style userdata `"AE.BrushFace"` (Matrix4 is the template): `new`, `fromPlane`, fields, `Normal`, `With`, `__eq`, `__tostring`; docs and analysis registration. Material held by GUID, pushed as an Instance through the runtime. Tests BF*.
3. **Brush instance** — `src/engine_instances/Brush.{hpp,cpp}`: `PhysicsBase` subclass; AngularVelocity, AngularDamping, Friction, Bounciness, CanCollide, Color, Transparency; hidden saved `Faces`; cached `Built`; `Custom` history; `write_place`/`read_place` through the base; registration (`register_lua_creatable`, `class_registry`, `ClassOrder`, icon, docs, README). Tests BI*.
4. **Lua methods** — `src/engine_core/BrushBindings.cpp` (TerrainBindings is the template): GetFaces, SetFaces, GetFace, SetFace, SetFaceMaterial, GetFaceVertices, GetVertices, GetBounds, ContainsPoint, Clip, MoveFace, Expand, MakeBox/Cylinder/Cone/Sphere. Tests BL*.
5. **Physics and raycast** — Brush branch in `PhysicsWorld::make_shape` (hull per piece from `hull_pieces` at Box3D's 128 limits, never simplified; scale applied to points; mass by volume); `RayHits` keeps the shape; `RayHit.face`; `RaycastResult.Face` and `.Material` for Brushes. Tests BP*.
6. **Drawing** — `VisualSnapshot::brushes` (id, world, shared immutable mesh + revision, anchored, color, transparency, per-range material values); `SceneFeed` copy; a brush cache on the render side; anchored brushes baked into 64-unit cells on a worker, one draw per Material per cell; moving and transparent brushes drawn alone; Material `TextureScale` as a uniform; culling and shadows through per-draw bounds. Check in `scene-render-check`.
7. **Selection outline and click-select** — selected Brushes outline their edges; clicking a Brush in the Scene View selects it (physics ray), Shift/Ctrl add.
8. **Brush editor** — `src/runner/BrushTool.{hpp,cpp}`, hooked into `GameView` like `TerrainBrush`: draw a box on the grid or on the face under the pointer, height by dragging, grid sizes, face drag with live dimensions, vertex/edge handles, clip, duplicate, nudge, rotate 90 / flip, primitive palette, material apply. Driven by the UX research.

## Review focus

1. A brush edit must never leave an invalid brush: every edit goes through `build`, and a refusal changes nothing.
2. Grid-aligned input must give exact grid output (no 1.9999999 vertices); everything the editor snaps relies on it.
3. Thousands of anchored brushes must stay a few hundred draws, and an edit must rebake only its cells.
4. Undo of a tool drag is one step; Stop restores faces exactly.
