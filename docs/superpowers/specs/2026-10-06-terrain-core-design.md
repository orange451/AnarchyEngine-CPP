# Terrain Core Design

2026-10-06 · sub-project 1 of the terrain roadmap (`2026-10-06-terrain-roadmap-design.md`). Needs sub-project 0 (`2026-10-06-edit-mode-physics-raycast-design.md`): bodies exist while stopped, and `workspace:Raycast` exists.

## Goal

A `Terrain` instance: an island of voxels that scripts sculpt into any shape, in edit mode and at runtime, drawn and collided as a smooth surface. Any number of Terrains may sit in Workspace; they share nothing. The user sees three properties, a set of methods, and a Configure Terrain tab where they choose which Materials the island may use. Everything else (chunks, data file, meshes) is internal and hidden.

This sub-project draws each material as its flat Color. Textured, blended materials are sub-project 3, and need no change to anything built here.

## Decisions

### The instances

| Question | Decision |
| --- | --- |
| Terrain's base class | `PVInstance`. Its Transform places the island; it may be moved and rotated. |
| Terrain's visible properties | `Transform: Matrix4`, `VoxelSize: number` (read-only, always 1), `CanCollide: boolean` (default true). |
| Scale | A Transform with scale or shear is refused: "Terrain cannot be scaled". Cell size is VoxelSize's job. |
| Terrain's hidden saved property | `DataPath: string`, the `.avox` file under resources, made with `lua_hidden`. |
| Where a Terrain may live | Anywhere an instance may. Only a Terrain in Workspace (at any depth) is meshed, drawn, and collided. Methods work everywhere. |
| What chooses a Terrain's materials | Its `TerrainMaterial` children. Each says "voxels with this Id draw and collide as this Material". The user configures them in the Configure Terrain tab or through Terrain's methods. |
| TerrainMaterial's properties | `Id: number`, read-only, 1–255, fixed for the TerrainMaterial's life. `Material: Material?`, settable; changing it re-skins every voxel with that Id at once, with no voxel edit. `Name` is editable and starts as its Material's name, so "Rock" and "Rock (large)" can be told apart. Later sub-projects add Shader, texture scale, and friction here. |
| TerrainMaterial is internal | Never shown in Explorer. Its Parent is fixed to the Terrain that made it: setting Parent raises "TerrainMaterial cannot be reparented". `Instance.new("TerrainMaterial")` raises; Terrain's `AddMaterial` makes them. `Destroy` works. These need two new engine abilities, added by this sub-project: a class flag that Explorer skips, and a class flag that refuses reparenting (Destroy, undo, paste, and Stop's restore still move it). |
| Several entries per Material | Allowed. One Material may back several TerrainMaterials, which will differ by tiling, shader, or friction once those exist. So voxel methods take TerrainMaterials, never Materials: a Material would be ambiguous. |
| Limit | 255 TerrainMaterials. One more raises "Terrain can hold at most 255 Materials". The real budget in practice is texture memory (sub-project 3), which the Configure Terrain tab will show. |
| Which Id a new entry gets | The lowest Id that no TerrainMaterial of this Terrain holds. |
| Deleting a TerrainMaterial | The voxels keep their Id and draw and collide as the default material (Id 0). The Configure Terrain tab lists how many cells use an unassigned Id, with a Replace action. A TerrainMaterial added later takes the lowest free Id, which may be that one, and the leftover voxels become its Material. Undo restores a deleted TerrainMaterial on its old Id. |
| Material asset deleted | The TerrainMaterial's `Material` becomes `nil`; its voxels draw as the default material until it is set again, or the delete is undone. |

### Voxels

| Question | Decision |
| --- | --- |
| What a cell stores | 2 bytes: a signed distance (int8) and a material Id (uint8). |
| Distance | The distance from the cell's sample point to the surface, in studs; negative inside. Stored as `round(d / (4 × VoxelSize) × 127)`, clamped to ±127, so the band is ±4 cells at a step of about 0.03 cells. Never stored in cell units, so a future resample needs no format change. |
| Sample points | Lattice points: cell `(i, j, k)` samples Terrain-local position `(i, j, k) × VoxelSize`. |
| Material Id | 0 is the default material (engine default gray). 1–255 name the TerrainMaterial with that Id, if there is one. Air cells store 0. |
| Chunk | 32³ cells, 32 × VoxelSize studs on a side, keyed by integer chunk coordinates. Missing chunks are air. |
| Chunk forms | **Uniform:** one distance and one Id (all air, or all solid of one material); tens of bytes. **Dense:** 32,768 distances and 32,768 Ids, 64 KB. A dense chunk that becomes uniform after an edit collapses back. |
| Id usage mask | Each chunk keeps a 256-bit mask of the Ids its solid cells use, rebuilt when the chunk is edited. ORing them answers "is this Id in use?" for the Configure Terrain tab without scanning cells. |
| Copy-on-write | A chunk's data is a `std::shared_ptr<const ChunkData>`, never changed once shared. An edit clones only the chunks it touches and swaps the pointer. Each swap bumps the chunk's revision. |
| Per-call limit | One edit call may touch at most 16,777,216 cells (256³). More raises "Terrain edit too large: split it into smaller calls". |

### Meshing, drawing, colliding

| Question | Decision |
| --- | --- |
| Meshing | Surface Nets: one vertex per surface cell, placed at the average of its edge crossings, normal from the distance field's gradient. Each surface-crossing lattice edge makes one quad, owned by the chunk that holds the edge's lower endpoint, so neighbors never double up. |
| Chunk borders | The mesher reads the chunk plus a 2-sample apron from its 26 neighbors (for border cells and gradients). Neighbors share border vertices exactly, so there are no cracks. An edit dirties every chunk whose apron it touches. |
| Vertex format | The existing `GpuVertex` (`amesh.hpp`). Color is white. The vertex's material Ids go in the bone-index channel and their weights in the bone-weight channel: this sub-project writes one Id (that of its most-solid neighboring sample) at weight 1. Sub-project 3 fills the other three. |
| Collision mesh | The same positions, welded, with each triangle's material Id in `b3MeshDef::materialIndices`. |
| Where meshing runs | One `TerrainMesher` worker thread, as `ConvexDecomposer` has. A job holds the chunk and apron pointers (copied cheaply, under the copy-on-write rule) and the revision. It returns the render mesh (`std::shared_ptr<const amesh::Data>`) and the Box3D mesh (`b3CreateMesh`, built on the worker). |
| Job order | Nearest chunk to the camera first. One pending job per chunk; a newer edit replaces a queued job. |
| Stale results | A result whose revision is older than the chunk's is dropped. The newer job brings the right geometry. |
| Applying results | On SimulationThread: the render mesh goes onto the chunk for the next snapshot; the Box3D mesh is handed to `PhysicsWorld`, which swaps the chunk's shape during its next `sync`, under the step lock while playing. |
| Physics | One `b3_staticBody` per Terrain at its Transform, and one mesh shape per chunk with triangles. `userData` is the Terrain's InstanceId. Each shape has 256 surface materials, `userMaterialId` = Id. CanCollide false means no shapes. A Transform change calls `b3Body_SetTransform`. The Terrain record owns every `b3MeshData`. |
| Raycast | `workspace:Raycast` hits chunk shapes like any other. `Instance` is the Terrain; `Material` is the Material of the TerrainMaterial with the reported Id, or `nil` for Id 0 or an unassigned Id. |
| Edit latency | An edit changes voxels before its call returns; `ReadVoxels` sees it at once. Meshes and colliders follow about one to two frames later. A ray cast straight after an edit may hit the old surface. Documented on every edit method. |
| Rendering | `SnapshotPump` emits a `VisualTerrainChunk` per meshed chunk: Terrain id, chunk coordinates, revision, world matrix, and the mesh pointer; and once per Terrain, a 256-entry color table (each TerrainMaterial's Material's Color, the default color for Id 0 and unassigned Ids). The renderer's `TerrainChunkCache` uploads a chunk when its revision changes and frees chunks no snapshot names, as `MeshCache::getSession` does. Chunks draw in the deferred geometry and shadow passes, culled by chunk bounds, with a terrain variant of `geometry.vert` / `surface.glsl` that colors by Id from the table. Changing a TerrainMaterial's Material changes only the table, never a mesh. |

### Play, saving, place bytes

| Question | Decision |
| --- | --- |
| Play | Pressing Play stores each Terrain's chunk map (pointers only). Edits during play clone chunks as usual. |
| Stop | Restoring the place gives each Terrain back the chunk map it had at Play, and its TerrainMaterials (they are children, restored with the tree). Only chunks whose pointers differ from the live ones are re-meshed. Play-time edits are never saved. |
| Place bytes | `write_place` writes a token, not voxels. The token names an entry in a `TerrainStash` that holds a chunk map snapshot (pointers). `read_place` takes the map from the stash. This covers the Play capture, Stop restore, copy and paste, and undo of Destroy, without copying voxel data. Copy-on-write means a stash entry shares every unchanged chunk with the live Terrain. Entries are released when the project closes and when edit history is cleared. A pasted copy gets its own file when next saved. TerrainMaterials travel as ordinary children. |
| Saving | TerrainMaterials save as ordinary child instances in the project's JSON, with `Id` as a hidden saved property. When the project saves, each Terrain with unsaved voxel changes writes its `.avox` (to a temporary file, then renamed over the old one). A Terrain without a DataPath gets `terrain/<Name>.<guid>.avox`. Edits never write files by themselves. The plan adds a per-instance save hook if `Project` has none. |
| Loading | Reading a Terrain reads its `.avox` and queues every chunk with triangles for meshing, nearest the camera first. A missing or corrupt file loads an empty Terrain and warns once. |
| Dirty flag | Every voxel edit while stopped marks the place dirty, as a property change does. No undo step for voxel edits yet (sub-project 2). TerrainMaterial changes are ordinary undo steps. |

### Out of scope

Sculpt tools and voxel-edit undo (sub-project 2); textures, blending, and per-material shaders (3); LOD, water, generators, resampling, `Flush` (4); a Studio debug hotkey.

## The `.avox` file

Little-endian. Voxels only; the materials are the TerrainMaterial children.

```
char  magic[4]       "AVOX"
u16   version_major  1
u16   version_minor  0
f32   voxel_size
u32   chunk_size     32
u32   chunk_count
then  chunk_count × chunk:
        i32 x, y, z
        u8  form             0 uniform, 1 dense
        uniform: i8 distance, u8 id
        dense:   u32 byte_length, then run-length triples (u16 run, i8 distance, u8 id)
                 covering 32,768 cells in x-fastest order
u32   crc32 of everything before it
```

Only chunks that are not all air are written.

## Lua API

### Terrain: materials

| Method | Effect |
| --- | --- |
| `AddMaterial(material: Material?) -> TerrainMaterial` | Adds a TerrainMaterial on the lowest free Id, named after the Material. Raises past 255. |
| `GetMaterials() -> {TerrainMaterial}` | Every TerrainMaterial, ordered by Id. |
| `GetMaterialById(id: number) -> TerrainMaterial?` | The TerrainMaterial with that Id, or `nil`. |
| `GetMaterialsFor(material: Material) -> {TerrainMaterial}` | Every TerrainMaterial backed by that Material, ordered by Id. |
| `RemoveMaterial(entry: TerrainMaterial)` | The same as `entry:Destroy()`. Raises if it belongs to another Terrain. |

### Terrain: voxels

`space` is `Enum.TransformSpace` (World, the default, or Local). World positions and frames are converted into the Terrain's space; Local ones are taken as they are, so island-relative code keeps working when the island moves.

`material` is a `TerrainMaterial` of this Terrain, or `nil` for the default material. A Material raises "Pass a TerrainMaterial (see Terrain:GetMaterials)"; another Terrain's TerrainMaterial raises "TerrainMaterial belongs to another Terrain".

| Method | Effect |
| --- | --- |
| `FillBall(center: Vector3, radius: number, material, space?)` | Union with a ball |
| `FillBlock(transform: Matrix4, size: Vector3, material, space?)` | Union with a box |
| `FillCylinder(transform: Matrix4, height: number, radius: number, material, space?)` | Union with a cylinder along the frame's Y |
| `FillWedge(transform: Matrix4, size: Vector3, material, space?)` | Union with a wedge (Roblox's wedge shape) |
| `SubtractBall(center, radius, space?)`, `SubtractBlock(transform, size, space?)`, `SubtractCylinder(transform, height, radius, space?)`, `SubtractWedge(transform, size, space?)` | Carve the shape out |
| `PaintBall(center, radius, material, space?)`, `PaintBlock(transform, size, material, space?)` | Set the material of solid cells inside, shape unchanged |
| `ReplaceMaterial(min: Vector3, max: Vector3, from, to, space?)` | Swap one material for another inside a box |
| `ReadVoxels(min: Vector3, max: Vector3) -> {Distances: {{{number}}}, Materials: {{{number}}}}` | Raw cells in integer cell coordinates, inclusive. Distances in studs; Materials as Ids, 0 for air and the default. Plain numbers keep large reads fast; `GetMaterialById` turns an Id into its TerrainMaterial, and a TerrainMaterial's `Id` goes the other way. |
| `WriteVoxels(min: Vector3, distances: {{{number}}}, materials: {{{number}}})` | Writes the arrays back; sizes must match, and Ids must be whole numbers 0–255. An Id with no TerrainMaterial is allowed and draws as the default, so a read then write keeps every cell as it was. |
| `WorldToCell(position: Vector3) -> Vector3`, `CellToWorld(cell: Vector3) -> Vector3` | Convert between world positions and cell coordinates |
| `Clear()` | Remove every voxel. TerrainMaterials stay. |

The shape operations, per cell, with `s` the shape's signed distance at the sample:

- **Fill:** `d = min(d, s)`; the Id becomes `material`'s where `s < d_old` and `s < VoxelSize`, so the new surface is painted.
- **Subtract:** `d = max(d, -s)`.
- **Paint:** the Id becomes `material`'s where `s ≤ 0` and `d ≤ VoxelSize`.

Distances for ball, box, and cylinder are exact. The wedge's is the box's intersected with its slope plane.

## Configure Terrain tab

`ide::IdeTerrainEditor`, a document tab built the way `IdePrefabEditor` is.

- **Opening:** `Terrain::context_actions` adds `InstanceAction::Edit`, so Edit appears in the Explorer context menu, and double-clicking a Terrain in Explorer opens it. `IdeLayout::edit` reuses an open tab for the same Terrain.
- **Contents:** a card per TerrainMaterial, in Id order, showing its Id, its editable Name, its Material (picked with `AssetPicker` over `asset_choices(world, "Material")`), and an "In use" badge from the Id usage masks. An "Add Material" tile, and a counter "n / 255". A row for cells that use unassigned Ids, if any, with a Replace action.
- **Editing:** through a `TerrainEditorHost` of callbacks, as `PrefabEditorHost` does. Each runs on SimulationThread inside `ScopedRecording`, one undo step each: "Add Terrain Material", "Set Terrain Material", "Remove Terrain Material".
- **Remove while in use:** asks "Replace with…", offering the other TerrainMaterials and the default. The replacement runs `ReplaceMaterial` over the whole Terrain and then destroys the card. Until voxel undo exists (sub-project 2), the dialog says the replacement cannot be undone. Choosing "Keep cells" instead leaves them on the freed Id, drawing as the default.
- **Selecting a card** shows its TerrainMaterial in the Properties panel.

## Architecture

New code lives in `src/engine_core/terrain/` (no engine dependencies, unit-testable), `src/engine_instances/Terrain.{hpp,cpp}` and `TerrainMaterial.{hpp,cpp}`, and `src/ide/IdeTerrainEditor.{hpp,cpp}`.

| Unit | What it does | Depends on |
| --- | --- | --- |
| `ChunkData` / `VoxelChunk` | One chunk: uniform or dense, its revision, its Id usage mask; clone and collapse | nothing |
| `VoxelVolume` | Map from chunk coordinates to chunks; the shape operations; read and write cells; the dirty-chunk set; the per-call limit; Ids in use | `VoxelChunk` |
| `ShapeDistance` | Signed distance functions for ball, box, cylinder, wedge | math types |
| `SurfaceNets` | `mesh(chunk, apron) -> {render vertices and indices, collision positions, triangles, Ids}`. Pure | `VoxelChunk` |
| `AvoxFile` | Read and write `.avox` | `VoxelVolume` |
| `TerrainMesher` | The worker thread, the job queue ordered by camera distance, stale-result dropping | `SurfaceNets`, Box3D through a callback that `PhysicsWorld.cpp` supplies (Box3D stays included only there) |
| `TerrainStash` | Token to chunk-map snapshot, for place bytes | `VoxelVolume` |
| `TerrainMaterial` | The instance: Id, Name, Material | `DataModel` |
| `Terrain` | The instance: properties, Lua methods, material resolution, saving, loading | everything above |
| `IdeTerrainEditor` | The Configure Terrain tab | `AssetPicker`, `ScopedRecording` |

Changes elsewhere:

- **Registration:** `ANARCHY_LUA_REGISTER` in `Terrain.cpp` and `TerrainMaterial.cpp`; `register_lua_creatable` for Terrain only (`ScriptBindings.cpp`); the insertable class list (`Project.cpp`), Terrain only; property and method docs (`LuaApi.cpp`); allowed parents (`Containment.cpp`): TerrainMaterial only under Terrain; README entries in `src/engine_instances/`.
- **Engine abilities:** a class flag Explorer skips, and a class flag that refuses scripted and Studio reparenting.
- **`PhysicsWorld`:** a record per Terrain in Workspace; the chunk shape swap in `sync`; the Box3D mesh build callback for `TerrainMesher`; the Terrain material in `RayHit`.
- **`SnapshotPump` / renderer:** `VisualTerrainChunk`, the per-Terrain color table, `TerrainChunkCache`, the terrain shader variant.
- **`Engine`:** collects `TerrainMesher` results each tick, playing or stopped, and passes it the camera position.
- **IDE:** `IdeLayout::edit` opens `IdeTerrainEditor` for a Terrain.

Thread rules: SimulationThread does every voxel write and every Lua call, and applies mesher results. The worker reads only the immutable chunk data handed to it in a job. The render thread sees only immutable meshes and color tables in the snapshot.

## Testing

- **`VoxelVolume`:** fill and subtract give the expected distances; a filled then subtracted region collapses back to uniform air; copy-on-write leaves earlier pointers intact and clones only touched chunks; Id usage masks match the cells; the per-call limit errors.
- **`SurfaceNets`:** a meshed ball's vertices lie within 0.1 × VoxelSize of its radius; two neighboring chunks share their border vertices exactly; uniform chunks make no triangles; a wall a cell thick still meshes.
- **`.avox`:** write then read gives identical chunks; a bad CRC or a missing file loads empty with one warning; VoxelSize is in the header.
- **TerrainMaterial:** `AddMaterial` takes the lowest free Id; two TerrainMaterials may share a Material and `GetMaterialsFor` returns both; the 256th raises; reparenting and `Instance.new` raise; it is absent from Explorer; deleting one leaves its cells on the Id, drawing the default color; adding another reuses the Id and the cells take its Material; undoing a delete restores the Id; changing `Material` changes only the color table.
- **Lua voxels:** every method in World and Local space on a rotated, moved Terrain; passing a Material, or another Terrain's TerrainMaterial, raises; `ReadVoxels` then `WriteVoxels` round-trips exactly, unassigned Ids included; a non-whole or out-of-range Id raises; error messages for a missing argument and the call limit; a scaled Transform is refused.
- **Physics:** a chunk shape appears once its job finishes; a result older than a newer edit is dropped; CanCollide false makes no shapes; `workspace:Raycast` hits a Terrain and returns its Material, stopped and playing.
- **Play and Stop:** edits during play are gone after Stop; TerrainMaterials added during play are gone after Stop; only the chunks changed during play are re-meshed; the `.avox` on disk is untouched by play.
- **Place bytes:** copy and paste of a Terrain gives an equal, independent island with its own TerrainMaterials; undoing its Destroy brings its voxels back.
- **Configure Terrain tab** (sandbox UI test): add, set, and remove make one undo step each; removing an in-use material offers Replace and Keep cells.
- **Performance** (a benchmark test, Release build): `FillBall` of radius 8 under 0.5 ms on SimulationThread; meshing one dense surface chunk plus its Box3D mesh under 2 ms on the worker; a 500-chunk island fully meshed within about 1 s of loading, while the simulation thread keeps ticking.
