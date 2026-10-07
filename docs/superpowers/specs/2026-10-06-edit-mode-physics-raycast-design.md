# Edit-Mode Physics and Raycast Design

2026-10-06 · sub-project 0 of the terrain roadmap (`2026-10-06-terrain-roadmap-design.md`). Builds on the Box3D physics design (`2026-09-30-box3d-physics-design.md`) and the custom shape decomposition design (`2026-10-06-custom-shape-decomposition-design.md`).

## Goal

The Box3D world exists while the place is stopped, not only while it plays. Stopped, every body is kept in step with the tree but never simulated: physics is paused, as if everything were anchored. Scripts get `workspace:Raycast`, which casts against that world in edit and play mode alike, so Studio tools, plugins, and Terrain can all ask "what is under this ray?" the same way.

## Decisions

| Question | Decision |
| --- | --- |
| How the pipeline splits | `PhysicsWorld::step` becomes two public calls. `sync(game)` is steps 1–3 of today's pipeline (drop bodies on a new world_generation; give bodies to PhysicsBases in Workspace and take them from the rest; push dirty properties). `simulate(game, dt)` is steps 4–6 (PlayerController ground probes, `b3World_Step`, write-back). `step` stays as `sync` then `simulate`. |
| Who calls `sync` while stopped | The Engine's stopped tick, beside `decomposer_.update`, on SimulationThread under the write lock. Once per tick. |
| Who calls `step` while playing | `Engine::step_physics`, as today. |
| Pressing Play | No rebuild: the bodies already exist and simulation just starts. Bodies made in edit mode without convex pieces are the one exception (below). |
| Pressing Stop | Unchanged: restoring the place bumps world_generation, every body is dropped, and the next `sync` builds bodies for the restored tree. |
| Body types while stopped | Real types: `b3_staticBody` for Anchored, `b3_dynamicBody` otherwise. Nothing moves because nothing steps. |
| A body's Transform while stopped | Follows the GameObject it moves (position and rotation, its own scale, a PlayerController upright), stored as a physics move is: no Changed, no history. Main does this since 2026-10-06 through `PhysicsWorld::follow_game_objects` from the Engine's stopped tick; `sync` takes it over. The body is then already where play starts it, and a dragger over the GameObject's children finds it there. |
| A driven body's own Transform written while stopped | Moves neither the body nor its GameObject (that would be a GameObject move with no Changed and no history); the GameObject decides, and the next `sync` puts the Transform back where the body is. The same holds on the first `sync` of play, for a write made after the last stopped `sync`. So a stopped `Raycast` sees a driven body where its GameObject is, not where its own Transform was just set. (Added at the merge into `terrain`, 2026-10-07.) |
| Velocity while stopped | Never applied. Velocity properties reach the body through `take_dirty` as today, and take effect when play starts. |
| Unanchored Custom while stopped | Never decomposes synchronously. It uses its Mesh's stored pieces if the file has them, else the single hull. The body record notes that it was made without pieces. When the decomposer stores pieces, the Mesh file's stamp changes and the next `sync` remakes the body. On Play, any body still marked "made without pieces" is remade through today's play path (stored pieces, memory cache, or synchronous decompose). |
| PlayerController while stopped | Has its body; no ground probe, since that is part of `simulate`. |
| Warnings while stopped | Body-building warnings (a Hull that falls back to a Box, and so on) fire while stopped too, once per instance as today. |
| `workspace:Raycast` signature | `workspace:Raycast(origin: Vector3, direction: Vector3, params: RaycastParams?) -> RaycastResult?`. The direction's length is the ray's length. `nil` on a miss, or when direction is zero. |
| `RaycastResult` | A new read-only datatype: `Instance` (the PhysicsBase hit; Terrain once sub-project 1 lands), `Position`, `Normal`, `Distance`, and `Material` (a Material asset or `nil`). In this sub-project `Material` is always `nil`; Terrain fills it from the per-triangle material Box3D reports. |
| `RaycastParams` | A new datatype made with `RaycastParams.new()`: `FilterType: Enum.RaycastFilterType` (Exclude, the default, or Include) and `FilterDescendantsInstances: {Instance}`. A hit counts when its Instance is, or is a descendant of, a listed Instance (Include), or is not (Exclude). |
| How hits map to instances | Each Box3D shape's `userData` holds its owner's InstanceId. The ray uses `b3World_CastRay` with a callback that skips filtered hits and clips to the closest kept hit. |
| Freshness | `Raycast` runs `sync` first, so it sees the tree as it is at the call, including a Transform the script set on the line before. `sync` only pushes what is dirty, so this is cheap. |
| Threads | `Raycast` runs on SimulationThread, as every Lua call does. Physics steps run on that thread too, between script phases, so a ray never overlaps a Box3D step and needs no lock of its own. |
| Out of scope | Studio click-to-select, shape casts (`workspace:Shapecast`), `Material` for non-Terrain hits, collision groups. |

## Architecture

### 1. `PhysicsWorld` (`engine_core/PhysicsWorld.{hpp,cpp}`)

- `sync(DataModel&)` and `simulate(DataModel&, double dt)`, public, with the header's pipeline comment split to match. `step` calls both, and still does nothing at all to simulate while stopped.
- `raycast(DataModel&, Vec3 origin, Vec3 direction, const RayFilter&) -> std::optional<RayHit>`, where `RayHit` is `{InstanceId instance; Vec3 position; Vec3 normal; float distance; std::uint8_t material_slot; bool has_material;}`. It calls `sync`, then casts. It knows nothing of Lua.
- The body record gains `bool made_without_pieces`. Making a body takes a flag saying whether synchronous decomposition is allowed (false while stopped).
- A hook for the start of play (or a check in `step` on the first tick of a world generation) remakes every body marked `made_without_pieces`.

### 2. Engine (`engine_core/Engine.cpp`)

The stopped tick calls `physics_.sync(game_)` under the write lock, next to `decomposer_.update(game_)`.

### 3. Lua

Workspace is a `SceneService` (`engine_services/SceneService.cpp`), so `Raycast` is registered there; the datatypes go in `engine_datatypes/` beside `Vector3` and `Matrix4`.

- `RaycastParams` and `RaycastResult` datatypes, registered as the other datatypes are, with property docs.
- `Enum.RaycastFilterType { Exclude = 0, Include = 1 }`.
- `Workspace:Raycast`, which converts arguments, builds the filter set from the listed instances, and calls `PhysicsWorld::raycast`.

## Testing

- **Edit mode:** stopped, a PhysicsObject in Workspace has a body (`has_body`); moving its GameObject moves the body on the next `sync`; nothing falls over many stopped ticks.
- **Play and Stop:** Play does not remake existing bodies (body identity unchanged); Stop drops and remakes them from the restored tree.
- **Custom without pieces:** stopped, it gets the single hull and no synchronous decompose runs; on Play, it is remade with pieces.
- **Raycast:** hits a box at the right position, normal, and distance; misses return `nil`; Include and Exclude filters work on descendants; a Transform set just before the call is seen; works stopped and playing.
- **Existing physics tests** still pass unchanged.
