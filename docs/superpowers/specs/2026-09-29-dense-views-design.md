# Dense Views: Heartbeat Stepping and Snapshot Membership Design

2026-09-29 · builds on the two-thread core (`engine_core/README.md`) and the scene services (commit 954bcd2).

## Goal

The two per-frame loops that today touch every instance become loops over dense arrays that change notices keep current, the way an ECS keeps component tables.

- **Heartbeat** walks the whole tree and calls a virtual `step()` on every descendant of the root, though only `TestTriangle` overrides it. It becomes a loop over a dense list of the instances whose class steps.
- **The render snapshot** admits every live GameObject, wherever it is — including Storage, and unparented ones. It becomes: a row exists iff the instance is a live GameObject under Workspace.

Not a framework. Three reusable primitives — scope bits on `Slot`, an `Ancestry` invalidation, and a `DenseIdSet` — composed by two consumers, with plain inlined membership tests. A later system (physics, culling, tweens) composes the same primitives; none of them is designed for it in advance.

## Decisions

| Question | Decision |
| --- | --- |
| Render scope | Only descendants of Workspace render, as in Roblox. Storage, the Assets tree, and unparented objects are out. |
| Step scope | Any instance under `game` whose class steps, wherever it is. Unparented instances do not step (they never did: the old walk started at the root). |
| Step order | Unspecified (dense-array order). It was breadth-first tree order; nothing relied on it, and Roblox promises none. |
| Flexibility | Primitives, not a view registry. Each consumer's membership test is inlined code, never a stored callable on the event path. |
| Membership updates | At the completion point of each tree mutation, via one idempotent helper. A move that does not change scope costs two bit compares. |
| Snapshot rule on entry | An object entering Workspace gets a complete row (transform, color, size), since fields may have changed while it had no row. |
| Path-C overrides | Unchanged. An override on a non-rendered object is a no-op, as an override on a dead one is today. |
| Capacity | Both dense structures reserve `kMaxInstances` at startup, like every pool. The step does not allocate. |

## Architecture

### DenseIdSet (`engine_core/DenseIdSet.hpp`)

1. **A dense set of live instance ids.** Iteration is over a packed `std::vector<InstanceId>`; `contains`, `position`, `insert`, and `erase` are O(1) through a `std::vector<int>` keyed by `id_slot(id)`, -1 when absent. `erase` is swap-and-pop. A stale generation misses because the dense entry at the mapped position no longer equals the queried id.
2. **Parallel payloads stay with the owner.** The set holds no payload. `erase` reports the swap — which id moved, from and to which position — so an owner keeping a parallel array mirrors it. The pump mirrors `VisualInstance` rows; the step list mirrors `DataModel*` pointers.
3. **`reserve(capacity)` once at startup**; after it, no member allocates. `insert` past capacity returns false, and each caller treats that as its existing capacity contract failure.
4. **Not thread-safe.** The owner touches it under whatever already guards its writes: the DataModel write lock for the step list, RenderThread's Prepare window for the pump.
5. **`SnapshotPump` refactors onto it.** `base_index_`, `remember`, and `erase_base` are deleted; a `DenseIdSet` plus the mirrored `instances` vector replace them. Behavior-neutral, covered by the existing `[T*]` tests.

### Scope bits (`DataModel`, `Slot`)

6. **`Slot` gains `in_game` and `in_workspace`.** `in_game` means reachable from the root; `in_workspace` means under the Workspace service. `State` caches `workspace_id`, set when `Game()` builds the fixed tree and whenever a place load recreates it.
7. **A node's bits come from its parent's in O(1):** parent 0 → `{true, false}`; parent `kNoParent` → `{false, false}`; otherwise the parent's bits, with `in_workspace` also true when the parent is `workspace_id`.
8. **`refresh_scope(id)`** recomputes `id`'s bits from its current parent. Unchanged bits return immediately. Changed bits walk the subtree top-down through the existing sibling links — children inherit deterministically — and fire membership events per node as bits flip:
   - `in_game` flipped and the instance steps → step-list insert or erase.
   - `in_workspace` flipped and the slot has a `body` → `Invalidation{id, VisualField::Ancestry, origin}`.
9. **Call sites**, the completion point of every mutation that changes tree shape:

   | Mutation | Refresh |
   | --- | --- |
   | `set_parent` | once, after `link_child`, before signals fire |
   | `destroy` | each orphan in `detach_links` gets bits cleared and events; the destroyed id itself needs none (`Removed` covers the pump; the step list erases it directly) |
   | place load | after its direct `link_child` calls |
   | undo and redo | after history relinks a revived subtree |
   | `create` | none — a new instance starts unparented with bits false |

   `set_parent` refreshes once after relinking rather than hooking `unlink_parent` and `link_child` separately, so a reparent is one walk, not an out-walk plus an in-walk.
10. **`DataModel::in_workspace(id)`** is a new const accessor; false for a dead id. The pump reads it during `take_changes`, under the write lock, so it is current.

### The step list (`DataModel`, `State`, `Engine`)

11. **`virtual bool steps() const { return false; }`** on `DataModel`; `TestTriangle` overrides it true. A class property, read when membership changes, never per frame.
12. **`State` holds `DenseIdSet step_set` and a parallel `std::vector<DataModel*> steppers`**, both reserved to `kMaxInstances`. Membership is `in_game && steps()`, maintained by item 8's events; `destroy` erases directly before releasing the slot.
13. **Heartbeat** copies `step_set`'s ids into the existing `step_ids` scratch — stepping may create, destroy, or reparent, so iteration is over a stable copy, as today — then calls `step(dt)` through each cached pointer whose id is still alive. `step_descendants` is deleted, and `State::walk` with it if nothing else uses it.

### Snapshot membership (`SnapshotPump`, `types.hpp`)

14. **`VisualField` gains `Ancestry`.** The queue, drain, and overflow paths need no change.
15. **A row exists iff `alive && GameObject && in_workspace`.** `apply_live` checks in order:
    - `Removed` or not alive → erase the row (unchanged).
    - not `in_workspace(id)` → erase the row. This covers an Ancestry flip out and any note for an out-of-scope object.
    - `Ancestry`, in scope → ensure the row exists and refresh transform, color, and size with the change's origin. An object recolored while in Storage had no row to patch; it must arrive complete.
    - `Transform`, `Color`, `Size` → per-field patch, unchanged.

    The queue stays an invalidation stream, not an event log: drain reads current DataModel state, so Workspace → Storage → Workspace within one frame settles correctly in queue order.
16. **`resync` filters the same way**, skipping objects not in Workspace, so overflow recovery lands on the same membership as the incremental path.
17. **Untouched:** overrides, camera, double-buffering, publish, the 2 ms budget, the blit. Snapshot size and blit cost now scale with rendered objects, not all GameObjects.
18. **A latent bug this fixes:** `Instance.new("GameObject")` never reaches the snapshot today until a property changes, because `create<GameObject>()` skips `create_game_object`'s `note()`. Under this design the object appears when parented into Workspace — the correct trigger — and that `note()` becomes harmless, since the pump ignores out-of-scope ids.

## Tests

New tests follow the sandbox's Catch2 style.

- **DenseIdSet**: insert, erase, contains; swap-and-pop reported so a parallel array can mirror it; stale-generation lookups miss; capacity respected.
- **Scope**: an unparented object has no row; parenting into Workspace produces a complete row, including fields set while outside; Workspace to Storage removes the row; a Folder subtree of GameObjects moved in and out flips every descendant; a move within Workspace fires no membership events.
- **Step list**: a TestTriangle steps once parented under game, stops when unparented or destroyed; a `step()` that destroys or reparents another stepper mid-iteration is safe.
- **History and place**: destroy plus undo restores the row; undo of a reparent restores membership; place load and Stop rebuild bits; overflow resync matches incremental membership.
- **Lua**: `Instance.new("GameObject")` parented into Workspace renders — the regression test for item 18.

**Existing tests:** the ~24 sites in `sandbox/tests.cpp` that create unparented GameObjects and expect snapshot rows use a new `support.hpp` helper, `create_part(DataModel&)` — create, then parent under Workspace. Assertions stay untouched. `[T1]` through `[T11]` stay green throughout, including under ThreadSanitizer.

## Phases

Each phase compiles and passes the full suite on its own.

1. `DenseIdSet` and the pump-internals refactor. Behavior-neutral.
2. Scope bits, `refresh_scope`, `workspace_id`, accessors. Maintained, not yet consumed.
3. The step list and the Heartbeat switch; delete `step_descendants`.
4. `VisualField::Ancestry`, pump membership, test migration.
