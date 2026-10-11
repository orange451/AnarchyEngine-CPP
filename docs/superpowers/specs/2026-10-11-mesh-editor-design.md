# MeshEditor: custom meshes, bones, and skin weights from Lua

Scripts build and keep editing arbitrary meshes vertex by vertex, give them a
skeleton, and weight their vertices to it. The result skins and poses exactly
as an imported rig does (`GameObject:GetBone`, `Bone.Offset`).

## Where this starts

- The Lua Mesh API today is whole primitives only: `Mesh:AddBox`,
  `AddSphere`, `AddCylinder`, `AddCone`, `AddPlane`, `AddTeapot`, `Clear`.
  Each one reads the whole AMESH file (or, playing, the session copy), edits
  it through `Mesh::edit_geometry`, and writes it back.
- The skinning foundation (2026-10-10-skinned-meshes-design.md) is done:
  AMESH carries a bone table and four bone/weight pairs per vertex,
  `Mesh::skeleton()` reads them from whatever the Mesh draws, and the `Pose`,
  GPU skinning, and `GetBone` follow. Only the importer makes bones today.

So nothing below the scripting layer needs new capability. What is missing is
a vertex-level API, ids that survive removals, and a way to make many small
edits without rewriting the file for each.

Other engines, for reference: Roblox `EditableMesh` (bones in the mesh, posed
by `Bone` instances matched by name: the closest to ours), Unity
`Mesh.SetBoneWeights` (bulk arrays), Godot `SurfaceTool.set_bones` (stateful),
Unreal Geometry Script `SetVertexBoneWeights` (plus automatic weighting).

## Decisions

- A separate **`MeshEditor`** handle, from `mesh:Edit()`, not methods on Mesh
  and not bulk arrays. Scripts make ongoing edits, so ids must stay valid
  across removals; the handle gives those ids a clear lifetime and frees the
  working copy when nobody holds it.
- The old `Mesh:Add*` and `Mesh:Clear` move to the editor and are **removed**
  from Mesh. No deprecation shims.
- Attributes are **per vertex** (as AMESH and Unity store them). A hard edge
  or UV seam is two vertices.
- Bone rest transforms are **in mesh space**, the space the vertices and
  AMESH use, not relative to the parent.
- A bone name already in use is an **error**, never renamed: `GetBone` and
  `FindBone` match by name.

## 1. Lua API

### Getting one

`Mesh:Edit() -> MeshEditor`. MeshEditor is a script handle, not an Instance.
There is at most one per Mesh: while one is alive, every script and plugin
that calls `Edit()` gets the same one.

`Edit()` errors on a Mesh whose geometry has LODs (as `edit_geometry` refuses
them today), so an imported mesh never silently loses its LODs.

### Ids

Vertex, triangle, and bone ids are integers. An id stays valid, meaning the
same thing, for the editor's whole life, whatever else is removed; ids are
never reused within one editor. Opening an existing mesh makes its vertex,
triangle, and bone indices the first ids (vertex 0 is the file's vertex 0).
Ids mean nothing across editors.

A call given an id that does not exist errors: `no vertex 12`,
`no triangle 3`, `no bone 7`.

### Geometry

| Method | Returns | Meaning |
|---|---|---|
| `AddVertex(position: Vector3)` | `number` | A new vertex: no normal set (computed), UV (0, 0), white, opaque, no bones |
| `RemoveVertex(id)` | `nil` | Removes it and every triangle that uses it |
| `GetVertices()` | `{number}` | Every live vertex id, ascending |
| `GetPosition(id)` / `SetPosition(id, Vector3)` | | |
| `GetNormal(id)` / `SetNormal(id, Vector3?)` | `Vector3` | Get gives the computed one when none is set; Set nil goes back to computed. A set normal is normalized; zero length errors |
| `GetUV(id)` / `SetUV(id, Vector2)` | | |
| `GetColor(id)` / `SetColor(id, Color3, alpha: number?)` | `Color3, number` | alpha 0 to 1, default 1 |
| `AddTriangle(a, b, c)` | `number` | Counter-clockwise front face. Two equal ids error |
| `RemoveTriangle(id)` | `nil` | Its vertices stay |
| `GetTriangles()` | `{number}` | Every live triangle id, ascending |
| `GetTriangleVertices(id)` | `number, number, number` | |

### Primitives

`AddBox`, `AddSphere`, `AddCylinder`, `AddCone`, `AddPlane`, `AddTeapot` take
exactly the arguments the Mesh methods take today and return `{number}`: the
ids of the vertices they added, so a script can move or weight them. Their
normals count as set (the shape builders make them, hard edges included).

`Clear()` removes every vertex, triangle, and bone. Ids made before it stay
dead; new ones continue from where they were.

### Bones

| Method | Returns | Meaning |
|---|---|---|
| `AddBone(name: string, parent: number?, rest: Matrix4?)` | `number` | rest is the bone's rest transform in mesh space, default identity. A name in use, or an empty one, errors |
| `RemoveBone(id)` | `nil` | Its children move to its parent (keeping their mesh-space rest); weights on it are dropped and the rest of each vertex's weights renormalized |
| `GetBones()` | `{number}` | Every live bone id, ascending |
| `FindBone(name)` | `number?` | |
| `GetBoneName(id)` / `SetBoneName(id, string)` | | Same rules as AddBone |
| `GetBoneParent(id)` / `SetBoneParent(id, number?)` | | A parent that would make a cycle errors |
| `GetBoneTransform(id)` / `SetBoneTransform(id, Matrix4)` | | The rest transform, mesh space. May carry scale; one with no inverse errors |

### Weights

- `SetVertexBones(vertex, bones: {number}, weights: {number}?)`. Weights
  default to equal shares. The two lists must be the same length; a negative
  or non-finite weight errors; a zero total errors. More than four keeps the
  four largest. The kept weights are renormalized to sum to 1. An empty list
  clears the vertex's bones.
- `GetVertexBones(vertex) -> {number}, {number}`: bone ids and weights, as
  renormalized.
- In a mesh with bones, a vertex with none is bound at commit to the first
  root bone (lowest id with no parent) at weight 1, as the importer binds one.
  `GetVertexBones` still reports it empty: the binding is a commit default,
  not stored.

### Posing

Unchanged. A committed bone is posed by `GameObject:GetBone(name)` and its
`Offset`, and `GetBoneNames` lists the committed skeleton.

## 2. Lifetime, commit, and data flow

### Ownership

`MeshEditor` (C++, `src/engine_instances/MeshEditor.{hpp,cpp}`) holds the
working model: vertex slots (position, normal, normal-set flag, UV, RGBA,
up to four bone ids and weights, live flag), triangle slots (three vertex
ids, live flag), bone slots (name, parent id, rest Matrix4, live flag), a
name-to-bone map, and a dirty flag. It knows nothing of Lua.

The Lua userdata holds a `std::shared_ptr<MeshEditor>`; the Mesh holds a
`std::weak_ptr`. `Mesh::edit()` returns the live editor or opens a new one
from what the Mesh draws now (this session's copy while playing, else the
file). Bindings live in `src/engine_core/MeshEditorBindings.cpp`, following
`RaycastBindings.cpp`.

### Commit

An edit sets the dirty flag and does nothing else. Once per step, after
scripts and tools have run (the play step and the stopped tick alike), each
dirty editor commits:

1. Compact live slots into an `amesh::Data`: vertices and triangles in id
   order; triangle indices, skins, and bone parents remapped to the compacted
   indices.
2. Order bones parents-first (AMESH requires it; `SetBoneParent` can break
   it), stable by id otherwise, and remap skins to that order.
3. Bind unweighted vertices to the first root bone, when there are bones.
   Quantize weights the way AMESH stores them.
4. Compute normals for vertices whose normal is not set (`compute_normals`,
   then put back the set ones), then tangents, each bone's `cull_radius`,
   and the box.
5. Hand the immutable `Data` to the Mesh as its live copy (below). No LODs
   and no pieces, as `edit_geometry` leaves them today.

The renderer, `Mesh::skeleton()`, the `Pose`, bounds, and physics read the
live copy exactly as they read session geometry now.

### Drawing now, writing the file later

The Mesh's session copy becomes a **live copy**, used in both modes:

- **Playing**: as today. The file and Path are never touched; Stop drops the
  live copy and the Mesh draws its file again.
- **Stopped**: the live copy draws on the next frame. The file is written
  from it (with `write_file`, atomic as now) when the editor has had no edit
  for 1 second, when the editor is freed, before a project save, and before
  Play. After the write the live copy is dropped and the Mesh draws its file.
  A Mesh with no Path gets `meshes/<Name>.<guid>.amesh`, and an empty file
  there, when `Edit()` opens its editor, so the Path change lands in the
  caller's undo step; that Path change is the only undo step.

The idle delay reads an injectable clock, so tests never sleep.

### Same-frame reads

Script calls that read a Mesh's committed geometry commit that Mesh's
pending edits first, on the gameplay thread, under the step's write lock:
`GameObject:GetBone`, `GameObject:GetBoneNames`, `Prefab:GetBoundingBox`, and
`OriginOffset` on Mesh and Prefab. So a bone added this frame can be fetched and posed this
frame. Other threads only ever see the last commit.

### When an editor ends

Play, Stop, a change to the Mesh's Path, the Mesh being destroyed, or its
file changing on disk from something else (a reimport) ends the editor. It
tells its own writes apart by the file stamp it last wrote. An editor ended
by Play writes its pending edits first; ended by anything else, they are
dropped. A project save writes pending edits but does not end the editor. Every later call on it errors: `this MeshEditor has ended;
call mesh:Edit() again`. The next `Edit()` opens a fresh one.

### Limits

The AMESH limits (`kMaxVertices`, `kMaxTriangles`, `kMaxBones`) are checked
when a vertex, triangle, or bone is added, counting live slots, so the error
names the call that broke them.

### Memory

An editor is freed when Lua collects its last handle; Lua does not see the
C++ memory behind it, so a large one may linger after scripts drop it. Its
edits are on disk (or in the live copy) by then; only memory is held. No
`Close()`.

## 3. Docs, Studio, and testing

### Docs and completion

- `LuaApi.cpp`: remove the `Mesh` `Add*`/`Clear` entries; add `Mesh:Edit`
  and a `MeshEditor` type with every method above, so completion knows what
  `Edit()` returns.
- `README.md` and `src/engine_instances/README.md`: mesh examples move to
  `mesh:Edit()`.
- `amesh.hpp`'s header comment: the datamodel Mesh is authored through
  `MeshEditor`.

### Studio

No UI changes.

### Tests

**Editor model** (sandbox, no GL):

- Ids survive removals; compaction remaps triangles, skins, and parents.
- `RemoveVertex` removes its triangles; `RemoveTriangle` keeps its vertices.
- Bones come out parents-first after `SetBoneParent` reorders them.
- `RemoveBone` reparents its children and renormalizes weights.
- More than four influences keeps the four largest; a zero total errors.
- An unweighted vertex binds to the first root bone.
- Auto normals touch only vertices with no normal set.
- `cull_radius` covers every weighted vertex.
- Opening a mesh and committing with no edits gives the same `Data` back.
- A duplicate bone name and a parent cycle error.

**Lua** (tests/):

- `Edit()` twice gives the same editor.
- Primitives return their vertex ids.
- A bone added and fetched with `GetBone` in one frame works, and its
  `Offset` moves the skinning matrix.
- Play and Stop end an editor, with the documented error.
- A mesh with LODs is refused.
- Stopped, the file is written after the idle delay (injected clock), when
  the editor is freed, and before save and Play; playing, never.
- The old `Mesh:AddBox` is gone.

**Live**: in a running Release Studio over MCP, a script builds a two-bone
tube with weights blended along its length; setting the tip's `Bone.Offset`
bends the tube and its shadow, and after Stop the file is on disk.

## Out of scope

Automatic weighting (Unreal's `ComputeSmoothBoneWeights`; a natural next
step), per-face-corner attributes, blend shapes, LODs for edited meshes,
undo of geometry, editing meshes that have LODs, and a weight-painting tool.
