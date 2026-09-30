# Dense Views: Flecs-Backed Stepping, Physics, and Snapshot Membership Design

2026-09-30 · revises the 2026-09-29 design (hand-rolled dense views) to put GameObject's spatial data in flecs. Builds on the two-thread core (`engine_core/README.md`) and the scene services.

## Goal

The per-frame loops that today touch every instance become flecs queries or loops over dense arrays, and GameObject's spatial data moves into flecs components, so render and physics read one copy of it.

- **Heartbeat** walks the whole tree and calls a virtual `step()` on every descendant of the root, though only `TestTriangle` overrides it. It becomes a query over the instances whose class steps.
- **Physics integration** scans all 16384 slots and chases a pointer into each GameObject. It becomes a query over simulated bodies.
- **The render snapshot** admits every live GameObject, wherever it is. It becomes: a row exists iff the instance is a live GameObject under Workspace, kept current incrementally.

Instances stay the model scripts, the editor, saving, and undo see. Flecs is the storage and query layer under them, private to `engine_core`.

## Decisions

| Question | Decision |
| --- | --- |
| Storage | Flecs v4.1.6 owns GameObject's transform, color, size, and velocity (option B). The members are deleted, not mirrored. |
| Why flecs over hand-rolled | Chosen for flexibility as systems are added (queries over any component mix, observers, relationships, the explorer). Hand-rolled arrays are faster still; flecs was accepted for the flexibility. |
| Hot-path API | Flecs' C API with component ids cached per world. The C++ API is for setup only: the world, component registration, query building. Its inline wrappers compile unoptimized in Debug engine code. |
| Measured cost (16385 bodies, MSVC 19.23) | Per body per physics step: today 3.8 ns Release / 5.5 Debug; flecs C-API query 1.7 / 1.6; flecs C++ `each` 1.8 / 29; hand-rolled 1.0 / 16. Per single read: today 1.6 / 2.1; flecs C-API get 2.0–2.4 / 14; flecs C++ `get` 4.6 / 40 (342 with flecs' debug checks); hand-rolled 0.4 / 7.8. |
| Flecs build flags | `flecs.c` builds optimized in every configuration, in its own CMake directory so its Debug C flags can drop `/RTC1`, which MSVC cannot combine with `/O2`. Flecs' debug checks are off (`FLECS_NDEBUG`) unless `ENGINE_FLECS_CHECKS=ON`: the engine never hands out flecs ids, and slot generations already reject stale use. |
| Which instances get an entity | Every live instance, from the moment its slot is issued until it is released. The root has none. |
| Render scope | Only GameObjects under Workspace have snapshot rows. |
| Step scope | Any instance under `game` whose class steps. |
| Step and physics order | Unspecified. Nothing relies on it. |
| Snapshot | Stays incremental: the invalidation queue plus `DenseIdSet` rows; values are read from flecs. |
| Flecs threads, pipelines, REST, explorer | Off. Flecs never starts a thread. |
| Windows timer resolution | Unchanged. flecs raises it to 1 ms per world by default; `EcsProcessSetup` clears that flag once, before the first world, since the engine paces frames around the default tick. |
| Deferred mode | Never used. Loops that run user code iterate a copied id list, not a live query. |

## Architecture

### The dependency (`CMakeLists.txt`)

1. **`FetchContent` by URL** fetches `distr/flecs.c` and `distr/flecs.h` at tag `v4.1.6`, as `httplib.h` is fetched. A new `cmake/flecs/CMakeLists.txt`, added with `add_subdirectory`, builds the static library `flecs` from the fetched `flecs.c`. In that directory's scope `/RTC1` and `/Od` are removed from `CMAKE_C_FLAGS_DEBUG` and Debug adds `/O2`, so flecs is optimized in every configuration without touching any other target's flags. `flecs` links into `engine_core`.
2. **Public definitions:** `FLECS_CPP_NO_ENUM_REFLECTION` — required: without it the world constructor asserts (Debug) or segfaults (Release) on MSVC 19.23, whose `__FUNCSIG__` layout flecs' enum reflection misparses. `FLECS_CUSTOM_BUILD` with `FLECS_CPP`, `FLECS_LOG`, and `FLECS_OS_API_IMPL` only. `FLECS_NDEBUG`, or, when the cache option `ENGINE_FLECS_CHECKS` is ON (default OFF), `FLECS_DEBUG` in Debug and `FLECS_NDEBUG` with `FLECS_KEEP_ASSERT` in the other configurations (flecs rejects `FLECS_DEBUG` beside `NDEBUG`). `flecs_STATIC` is **not** defined by the build: the amalgamated header defines it, and a second definition warns (C4005).
3. **`engine_core/Ecs.hpp`** is the one include of `flecs.h`, wrapped in `#pragma warning(push, 0)` / `#pragma warning(pop)` so engine code stays warning-free at `/W4`. It declares the components and tags, and `EcsIds`, the component and tag ids a world registered, which the hot paths pass to the C API (`ecs_get_id`, `ecs_set_id`, `ecs_has_id`, `ecs_add_id`, `ecs_remove_id`, `ecs_query_iter`, `ecs_query_next`, `ecs_field_w_size`). Nothing outside `engine_core` includes it.

### World and entities (`DataModel`, `State`)

4. **Each root DataModel owns one `flecs::world`** in `State`, built in the root constructor, with its `EcsIds` and cached queries beside it. Separate DataModels have separate worlds.
5. **Threading:** the world is touched only by a thread that holds the DataModel's lock — the write and read modes share one exclusive mutex — or that is the gameplay thread before engine threads start. This is the rule GameObject's fields follow today. The render thread touches it only inside Prepare, never in Perform, Present, or PostRender.
6. **`Slot` gains `std::uint64_t entity`** (a `flecs::entity_t`; 0 when none). `issue_entity(Slot&, InstanceId)` creates the entity with `Instance{id}` and, for a class whose `steps()` is true, the `Steps` tag. It runs in the two paths that issue a slot — `spawn` (after `allocate`) and `adopt_slot` (place restore and undo revive) — before the object is constructed or reused, so construction can write components. `release_to_pool` (reached from `destroy` and `retire_slot`) deletes the entity and zeroes the handle after `on_release`.
7. **`DataModel::entity()`** (private, `GameObject` is a friend) returns the live slot's entity, or 0 when `id_` is dead, through the generation-checked `slot()`. A stale `InstanceId` fails closed before any flecs call. Flecs ids never leave `engine_core`.

### Components and tags (`Ecs.hpp`)

8. | Kind | Name | Replaces |
   | --- | --- | --- |
   | component | `Transform` (existing type) | `GameObject::transform_` |
   | component | `ColorRgb` (existing type) | `GameObject::color_` |
   | component | `Size { float x, y, z; }` | `GameObject::size_[3]` |
   | component | `Velocity { float x, y, z; }` | `GameObject::velocity_[3]` |
   | component | `Instance { InstanceId id; }` | new: entity → instance |
   | tag | `InGame`, `InWorkspace` | the 2026-09-29 design's `Slot` scope bits |
   | tag | `Steps` | the 2026-09-29 design's step list |
   | tag | `Simulated`, `VisualOnly` | `Slot::simulated`, `Slot::visual_only` |

   The replaced members and `Slot` fields are deleted. Every reader and writer — `authorize`, `set_simulated`, `set_visual_only`, `simulated()`, `visual_only()`, place capture and restore, history capture — reads or writes the tag.

### GameObject (`engine_instances/GameObject.*`)

9. **Its API is unchanged.** `transform()`, `color()`, and `copy_size()` read the component through `entity()`; a dead instance still reads the zero matrix, `ColorRgb{}`, and `false`. The setters keep every check — `authorize`, paths B and D, `ForceSimWrite`, the equal-value early out, undo recording, `note()` — and change only the store.
10. **Construction and `on_reuse` call `reset_spatial()`**, which sets the four components to today's defaults: identity transform, `ColorRgb{}`, size (1, 1, 1), zero velocity. `clear_spatial()` is deleted: a released instance has no entity, so its reads already fail closed.
11. **Save, load, place bytes, the Lua `Transform`/`CFrame`/`Color` properties, and `Prefab` are unchanged**, since they go through the accessors. The save and place formats are byte-for-byte the same.

### Scope tags

12. **`refresh_scope(id)`** computes `id`'s scope from its parent: parent 0 → `InGame`; parent `kNoParent` → neither; otherwise the parent's `InGame`, and `InWorkspace` when the parent has it or is the Workspace service (`scene_service("Workspace")`, which walks only game's few children and needs no cache when a place load recreates services). Tags that already match return at once. Otherwise it walks the subtree top-down through the sibling links, pruning any node whose tags come out unchanged, adds or removes tags as they flip, and, when `InWorkspace` flips on a GameObject, calls `note(id, VisualField::Ancestry, current_origin())`.
13. **Call sites**, the completion point of every tree mutation: `set_parent` once after relinking; each orphan in `detach_links` (destroy and place retire); `link_children` after each `link_child` (place restore; also history's sibling reorder, where it is a no-op). Undo relinks through `set_parent` or `link_children`. A new entity starts with no scope tags, and a released one is deleted, so `create` and `destroy` need nothing more.
14. **`in_game(id)` and `in_workspace(id)`** are public tag reads; false for a dead id.

### Stepping

15. **`virtual bool steps() const`** on `DataModel`, false; `TestTriangle` overrides it true. Read only in `issue_entity`.
16. **A cached query** over `Instance` with `Steps` and `InGame`, built with the world.
17. **`step_instances(dt)`** copies the query's `Instance` ids into the reserved `step_ids` scratch, then, outside the query, calls `step(dt)` on each id still alive. `step()` may create, destroy, or reparent with immediate effect. `step_descendants` and its tree walk are deleted; Engine's Heartbeat calls `step_instances`.

### Physics

18. **A cached query** over `Transform` (in-out), `Velocity` (in), and `Instance` (in), with `Simulated` and without `VisualOnly`.
19. **`integrate_simulated(dt)`** runs it: a zero velocity skips; otherwise the translation moves by velocity × dt, then `note(id, Transform)` and `notify_watchers(id)`. It writes component values only, so it runs inside the query. If a watcher callback touches the DataModel, the loop instead collects ids and notifies after the query.

### Snapshot membership (`SnapshotPump`, `types.hpp`)

20. **`VisualField` gains `Ancestry`.** The queue, drain, and overflow need no change.
21. **A row exists iff live, a GameObject, and `InWorkspace`.** `apply_live` checks in order: `Removed` or dead → erase; not `in_workspace` → erase; `Ancestry` → ensure the row and copy transform, color, and size; otherwise patch the named fields. Values come through the accessors. The queue stays an invalidation stream: drain reads current state.
22. **`resync`** fills rows from a query over `Instance`, `Transform`, `ColorRgb`, `Size` with `InWorkspace`, so overflow recovery lands on the incremental membership.
23. **Unchanged:** `DenseIdSet base_ids_` rows, overrides, camera, double-buffering, publish, the 2 ms budget.
24. **A latent bug this fixes:** `Instance.new("GameObject")` never reached the snapshot until a property changed, because `create<GameObject>()` skips `create_game_object`'s `note()`. It now appears when parented into Workspace.

## Tests

- **Entities:** one entity per live instance (a count of entities with `Instance` equals live instances); destroy deletes it; a stale id's accessors read zero; undo revive and Stop recreate entities with their values.
- **Tags:** `set_simulated` and `set_visual_only` flip tags; `simulated()`, `visual_only()`, and `authorize` agree with them (the existing path B/D tests stay green).
- **Scope:** unparented → no scope; into Workspace → `InGame` + `InWorkspace`; Storage → `InGame` only; a Folder subtree moved in and out flips every descendant; destroy orphans leave scope; undo and Stop restore scope.
- **Stepping:** a TestTriangle steps only under game; stops when unparented or destroyed; a stale id in the frame's copy is skipped.
- **Physics:** a simulated body with velocity moves; a visual-only or unsimulated one does not; a moved body is noted.
- **Membership:** unparented and Storage objects have no row; entering Workspace gives a complete row, including fields set outside; a move within Workspace keeps the row; `destroy_tree` clears rows; overflow resync matches incremental membership; `Instance.new` parented into Workspace renders.
- **Unchanged behavior:** save/load, place capture/restore, and history round-trips stay byte-identical — the existing project and history suites are the check.
- **Existing tests:** the 24 `create_game_object` sites in `sandbox/tests.cpp` that read snapshot rows use a `create_part` helper that parents into Workspace.
- **Build:** engine code stays warning-free at `/W4` with flecs included.

## Phases

Each phase builds and passes the full suite on its own.

0. The `[T4]` race fix (done).
1. `DenseIdSet` and the pump refactor onto it (done).
2. Flecs dependency, `Ecs.hpp`, a world per DataModel, an entity per instance.
3. GameObject's fields into components; `Simulated` and `VisualOnly` tags. No behavior change.
4. Scope tags and their call sites.
5. Stepping and physics as queries.
6. `Ancestry`, pump membership, the test migration, the `Instance.new` regression.
