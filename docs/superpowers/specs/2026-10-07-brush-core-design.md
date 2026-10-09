# Brush Core Design

2026-10-07 · sub-project 1 of the Brush roadmap (`2026-10-07-brush-roadmap-design.md`). Assumes `workspace:Raycast` (`2026-10-06-edit-mode-physics-raycast-design.md`) is on main, including the material slot in `RayHit`.

## Goal

A `Brush` instance: one convex solid with a Material and Valve 220 texture alignment per face, stored as TrenchBroom stores it. It draws with each face's Material, collides exactly, is hit by `workspace:Raycast` with the face reported, saves, undoes, and is made and edited from Lua. Studio can insert one and move it with the existing Draggers. Editing tools come in sub-project 2.

## Decisions

### The instance

| Question | Decision |
| --- | --- |
| Base class | `PhysicsBase`, so a ray's `Instance` is always a PhysicsBase: a PhysicsObject, a PlayerController, a Brush, and later Terrain. |
| Inherited | Transform, Velocity, Anchored, Mass, LinearDamping, GameObject. Anchored defaults to **true** for a Brush. Unanchored, it is a dynamic body like any other, and may move a GameObject through the GameObject link. |
| Own properties | AngularVelocity, AngularDamping, Friction, Bounciness (as PhysicsObject: same defaults and checks); CanCollide (true); Color and Transparency (as GameObject: they tint and fade the whole Brush, stacking on each face's Material). Saved registry properties. |
| Faces | One saved property, **Faces**, hidden from Properties. Read and written through methods; assigning `brush.Faces` raises "use SetFaces". Changes fire `Changed("Faces")`. |
| Parents | Workspace or any PVInstance, as PhysicsObject. |
| New Brush | A 4×4×4-unit box, six faces, no Materials, centred on its Transform, anchored. |

### Faces

| Field | Type | Meaning |
| --- | --- | --- |
| P1, P2, P3 | Vector3, stored as doubles | Three points on the face, counter-clockwise seen from outside, in the Brush's local space. The plane comes from them. |
| Material | Material? | Nil draws the default material (white, roughness 0.4). Held by GUID. |
| UAxis, VAxis | Vector3 | Texture axes in local space. A face made without them gets them from the nearest world axis. |
| Offset | Vector2 | Texture shift, in repeats. (0, 0). |
| Scale | Vector2 | Multiplies the Material's TextureScale. (1, 1). Until Material has TextureScale, one unit per repeat; whichever of this or terrain textures lands first adds `Material.TextureScale` (8 by default, as the terrain textures spec defines it). |
| Rotation | number, degrees | Rotates U and V about the face normal, as Valve 220 stores it. 0. |

There is no limit on the number of faces. Four is the fewest that can close a solid.

### Building the shape

TrenchBroom's method (`Brush::updateGeometryFromFaces`):

1. Start from a box 1,000 times the extent of the faces' points, around their centre. Sized from the faces, not a fixed world size, so there is no world bound on where or how large a brush can be. If any of the start box's own faces survives step 2, the faces do not close a solid.
2. Clip it by each face's plane, in doubles.
3. Merge vertices within 1e-6 of each other.
4. Drop faces whose planes cut nothing.

A face list is refused, with the reason and no change, only when a face's points are collinear ("face 3's points lie on a line"), when the result is empty ("the faces leave nothing"), or when it is not closed ("the faces do not close a solid"). The polygons, vertices, and planes are cached on the Brush and rebuilt only when the faces change. Every edit goes through this build, so a Brush is never invalid.

### Drawing

| Question | Decision |
| --- | --- |
| Mesh | Each face's polygon as a triangle fan. Per vertex: position, flat normal, tangent (along UAxis, for normal maps), UV from the face's axes, Offset, Scale, Rotation. UVs are in local space, so textures move with the Brush. Built on SimulationThread from the cached polygons when faces change. |
| GPU storage | One shared, growing vertex buffer and index buffer for all Brush geometry. No VAO or VBO per Brush. Draws use `glDrawElementsBaseVertex` (core in GL 3.2). |
| Anchored brushes | Baked into **64-unit cells** on a worker thread: each cell's anchored, opaque brushes in world space, grouped by Material. One draw per Material per cell. Each cell has a bounding box and goes through the existing frustum culling and shadow passes. |
| Rebuilds | An edited, added, or removed brush marks only the cells its bounds touch. The old bake draws until the new one is uploaded, so edits never flicker. A brush being dragged in Studio leaves its cell and draws alone until it settles. |
| Moving brushes | Unanchored brushes draw one by one from the shared buffer with their Transform, one draw per Material. |
| Transparency | Faces with a transparent Material, or a Brush with Transparency above 0, are not baked. They draw in the existing transparent pass, sorted by distance. |
| Handoff | `VisualSnapshot::brushes`: id, Transform, a shared immutable mesh (per-Material index ranges) with a revision, Anchored, Color, Transparency, and the Materials' values. The render thread uploads only when a revision changes, as Terrain chunks do. |
| Cost | About 40 bytes per vertex; a box is 24 vertices and 36 indices. A 3,000-brush map is about 3 MB of GPU memory and at most a few hundred draws, of which only cells in view are drawn. |

### Physics and raycast

| Question | Decision |
| --- | --- |
| Body | One Box3D body per Brush in Workspace: static while Anchored, dynamic otherwise. `userData` is the Brush's InstanceId. All PhysicsBase rules hold: edit-mode sync, Stop's restore, moving a GameObject. |
| Shapes | One exact hull from the shape's vertices, never simplified. Past Box3D's limit (128 vertices, faces, or edges), the shape is cut by a plane through its centre across its longest axis, and each half again, until every piece fits. Halves of a convex solid are convex and fill it exactly. The pieces never show in the faces. |
| Shape settings | Each shape takes the Brush's Friction and Bounciness. CanCollide false: no shapes. |
| Scale | Scale in the Transform's axes is applied to the hull points, as `PhysicsWorld::shape_scale` does, with the same near-equality check so a turning body does not remake its shapes. |
| Mass | The Brush's Mass, as PhysicsObject (`updateBodyMass=false`). A split brush shares it among its pieces by volume, so the centre of mass is right. |
| Changes | A face edit marks `kDirtyShape`; the next sync rebuilds. `SetFaces` then `Raycast` on the next line sees the new shape. |
| Raycast | A hit on a Brush: the point and normal are taken into local space, and the face is the one whose plane holds the point (within 1e-4) with the nearest normal. `RaycastResult.Material` is that face's Material. A new field, **`RaycastResult.Face: number?`**, is its index into `GetFaces()`; nil for anything but a Brush. It does not depend on which piece was hit. |

### Saving, Stop, undo

| Question | Decision |
| --- | --- |
| Format | Faces save as a JSON array, one object per face: `{"p":[9 numbers],"m":"<GUID>","u":[3],"v":[3],"o":[2],"s":[2],"r":0}`. Fields at their defaults are left out. Numbers are written in their shortest form that reads back exactly. |
| Missing Material | The face draws the default and keeps the GUID, so undo or a restored asset brings it back. |
| Place bytes | `write_place` / `read_place` overrides write the faces, then call the base. Stop and undo restore through them. |
| History | One step per method call, holding the whole old face list (about 200 bytes for a box). |

### Studio

| Question | Decision |
| --- | --- |
| Insert | "Brush" in the insertable classes: the 4-unit box in front of the camera, anchored. |
| Explorer | An icon and a class-order slot. |
| Selection | Clicking a brush selects it. The existing Draggers move and rotate it through Transform. The selection outline uses its mesh. |
| Properties | All properties except Faces. |

### Out of scope

Editing tools (sub-project 2), face texturing tools and texture lock (3), CSG and `.map` import (4), and batching beyond cells (5).

## Lua API

### BrushFace

A new read-only datatype, a value like Vector3.

| Member | Description |
| --- | --- |
| `BrushFace.new(p1: Vector3, p2: Vector3, p3: Vector3) -> BrushFace` | A face through three points, counter-clockwise from outside. No Material, axes from the nearest world axis, Offset (0, 0), Scale (1, 1), Rotation 0. Collinear points raise. |
| `BrushFace.fromPlane(normal: Vector3, point: Vector3) -> BrushFace` | A face from its outward normal and a point on it. A zero normal raises. |
| `P1`, `P2`, `P3`, `Material`, `UAxis`, `VAxis`, `Offset`, `Scale`, `Rotation` | The fields above. |
| `Normal: Vector3` | The outward unit normal, from the points. |
| `face:With(changes: {[string]: any}) -> BrushFace` | A copy with the named fields changed. Unknown names, wrong types, or collinear points raise. `Normal` cannot be set. |

### Brush methods

Indices start at 1. Each call is one undo step. A refused call raises its reason and changes nothing.

| Method | Description |
| --- | --- |
| `GetFaces() -> {BrushFace}` | All faces, in order. |
| `SetFaces(faces: {BrushFace})` | Replaces all faces. Faces that cut nothing are dropped, so the count can shrink. |
| `GetFace(i) -> BrushFace` / `SetFace(i, face)` | One face. |
| `SetFaceMaterial(i, material: Material?)` | Sets one face's Material. The shape is not rebuilt. |
| `GetFaceVertices(i) -> {Vector3}` | That face's corners in local space, counter-clockwise from outside. |
| `GetVertices() -> {Vector3}` | Every corner in local space. |
| `GetBounds() -> (Vector3, Vector3)` | Local minimum and maximum. |
| `ContainsPoint(point: Vector3) -> boolean` | Whether a local-space point is inside or on the surface. |
| `Clip(face: BrushFace)` | Adds a face, cutting away everything in front of it. Raises if nothing would remain. |
| `MoveFace(i, distance: number)` | Moves a face along its normal; negative moves it inward. Raises if the solid would vanish. Other faces that stop cutting are dropped. |
| `Expand(distance: number)` | Moves every face along its normal; negative shrinks. |
| `MakeBox(size: Vector3)` | Becomes a box of that size centred on the origin, no Materials. |
| `MakeCylinder(size: Vector3, sides: number)` | A cylinder along Y filling `size`, with `sides` sides (at least 3). |
| `MakeCone(size: Vector3, sides: number)` | A cone along Y, base at the bottom, tip at the top. |
| `MakeSphere(size: Vector3, detail: number)` | An icosphere filling `size`; `detail` is subdivision levels (0 is 20 faces, each level ×4). |

`Instance.new("Brush")` makes the 4-unit anchored box.

### RaycastResult

| Member | Description |
| --- | --- |
| `Face: number?` | New. The index into the hit Brush's `GetFaces()`. Nil for any other instance. |

## Architecture

- **`engine_core/brush/BrushGeometry`**: pure geometry, no instances or Lua. Faces in, polygons/vertices/planes or a refusal out; the clip build; `MoveFace`, `Expand`, `Clip`, the shape builders, `ContainsPoint`; splitting into hull-sized convex pieces; building the mesh (positions, normals, tangents, UVs, per-Material index ranges). Doubles throughout, floats in the mesh.
- **`engine_instances/Brush`**: the instance. Holds faces and the cached geometry; saved properties; `save_properties` / `load_property` for Faces; `write_place` / `read_place`; history; marks `kDirtyShape` and a visual note on face changes.
- **`engine_datatypes/BrushFace`**: the datatype, registered as Vector3 is.
- **PhysicsWorld**: a Brush branch in `make_shape` (one hull per piece, from `build_hull` without simplification); the face lookup in `raycast`; `RayHit` gains a face index.
- **Rendering**: `VisualSnapshot::brushes`; a brush buffer pool (shared VBO/EBO with a free-range allocator) on the render thread; the cell baker on a worker thread; brush cells and moving brushes as their own runs in the draw batches, through culling, shadows, and the transparent pass.
- **Registration:** `ANARCHY_LUA_REGISTER` in `Brush.cpp` (`register_lua_class("Brush", "PhysicsBase", ...)`, `register_suited_parents`); `register_lua_creatable` (`ScriptBindings.cpp`); the class registry and insertable list (`Project.cpp`); member docs (`LuaApi.cpp`), including `RaycastResult.Face`; `ide/ClassOrder.hpp`, `ide/IdeIcons.cpp`; `CMakeLists.txt`; README entries.

## Testing

- **Geometry:** a box; clipping; redundant faces dropped; collinear, empty, and unclosed lists refused with their reasons; `MoveFace`, `Expand`, and the shape builders; a brush past Box3D's limits splits into pieces that each fit and whose volumes sum to the brush's.
- **UVs and tangents:** default axes per face direction; Offset, Scale, Rotation.
- **Lua:** every method and constructor; `BrushFace:With`; a refused call leaves faces unchanged; `Changed("Faces")` fires; assigning `Faces` raises.
- **Saving:** exact round trip of numbers; defaults omitted; a missing Material keeps its GUID; undo and Stop restore faces.
- **Physics:** an anchored brush stays put; an unanchored one falls and rests; CanCollide false lets a ray through; `RaycastResult.Face` and `Material` are right, including on a split brush; a scaled Transform scales the hull.
- **Drawing:** only edited cells rebake; a moved-out brush draws alone; transparent faces go to the transparent pass; a 3,000-brush scene stays within a draw-count budget.
