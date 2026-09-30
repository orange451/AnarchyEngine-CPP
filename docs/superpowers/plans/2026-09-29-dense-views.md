# Dense Views Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Heartbeat steps a dense list of stepping instances instead of walking the tree, and the render snapshot holds a row only for live GameObjects under Workspace, kept current by scope bits and a new Ancestry invalidation.

**Architecture:** Three primitives — a `DenseIdSet` (packed ids + slot→position map), `in_game`/`in_workspace` bits on `Slot` maintained at the tree-mutation completion points, and a `VisualField::Ancestry` invalidation — composed by two consumers with inlined membership tests. No view registry.

**Tech Stack:** C++17, MSVC (Debug config: `./build/Debug/sandbox.exe`), CMake, Catch2 v3 (sandbox target).

**Spec:** `docs/superpowers/specs/2026-09-29-dense-views-design.md`

## Global Constraints

- Base branch is `dense-views` off a3b0198; the worktree is `.worktrees/dense-views`. Do not `cd` out of it.
- Both dense structures reserve `DataModel::kMaxInstances` (16384) at startup. No allocation in the per-frame step.
- The step order over the dense list is unspecified; no test may assert an order.
- `State::walk` stays — `emit_ancestry` uses it. Scope walks use their own scratch vector.
- Every task ends with the FULL suite green: `./build/Debug/sandbox.exe` → 0 failures (baseline: 306 cases, 1 skipped).
- Build with: `cmake --build build --target sandbox --parallel`.
- Never bare `git stash` (shared stash stack across worktrees).

## Review Focus

Spec-implied behaviors no existing test exercises; each line's test is pinned to the owning task.

1. A GameObject recolored while in Storage must arrive in the snapshot complete when moved into Workspace — Task 5, "a row arrives complete".
2. `destroy_tree` of a Folder subtree under Workspace must remove every descendant's row and stepper — Task 5, "destroy_tree clears rows".
3. Reparenting to `kNoParent` (leaving the tree) must remove the row and stop stepping — Task 4 "a triangle stops when unparented" and Task 5 "leaving the tree removes the row".
4. Overflow resync must land on the same membership as the incremental path (Storage objects excluded) — Task 5, "resync keeps Workspace membership".
5. A script's `Instance.new("GameObject")` parented into Workspace must render — today it silently never enters the snapshot — Task 6, "Instance.new renders".

---

### Task 1: DenseIdSet

**Files:**
- Create: `src/engine_core/DenseIdSet.hpp` (header-only)
- Create: `sandbox/dense_views_tests.cpp`
- Modify: `CMakeLists.txt:469-474` (add the test file to the `sandbox` executable's source list)

**Interfaces:**
- Consumes: `InstanceId`, `id_slot(id)` from `src/engine_core/types.hpp:19`, `contract_fail` from `src/engine_core/Contract.hpp`.
- Produces: `class engine_core::DenseIdSet` with `void reserve(std::size_t slots)`, `std::size_t size() const`, `const std::vector<InstanceId>& ids() const`, `bool contains(InstanceId) const`, `int position(InstanceId) const` (-1 when absent), `bool insert(InstanceId)` (false when present, `contract_fail` when full), `int erase(InstanceId, InstanceId* moved = nullptr)` (vacated position or -1; `*moved` is the id swapped into that position, 0 when none), `void clear()`.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/dense_views_tests.cpp`:

```cpp
// Dense views: DenseIdSet, scope bits, the step list, and snapshot membership.

#include "support.hpp"

#include "DenseIdSet.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

engine_core::InstanceId test_id(std::uint32_t generation, std::uint32_t slot) {
    return engine_core::make_instance_id(generation, slot);
}

}  // namespace

TEST_CASE("DenseIdSet inserts, finds, and rejects duplicates", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId a = test_id(1, 5);
    const engine_core::InstanceId b = test_id(1, 9);
    REQUIRE(set.size() == 0);
    REQUIRE_FALSE(set.contains(a));
    REQUIRE(set.position(a) == -1);
    REQUIRE(set.insert(a));
    REQUIRE(set.insert(b));
    REQUIRE_FALSE(set.insert(a));
    REQUIRE(set.size() == 2);
    REQUIRE(set.contains(a));
    REQUIRE(set.position(a) == 0);
    REQUIRE(set.position(b) == 1);
    REQUIRE(set.ids()[0] == a);
    REQUIRE(set.ids()[1] == b);
}

TEST_CASE("DenseIdSet erase swaps the last id in and reports it", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId a = test_id(1, 1);
    const engine_core::InstanceId b = test_id(1, 2);
    const engine_core::InstanceId c = test_id(1, 3);
    set.insert(a);
    set.insert(b);
    set.insert(c);
    engine_core::InstanceId moved = 123;
    REQUIRE(set.erase(a, &moved) == 0);
    REQUIRE(moved == c);
    REQUIRE(set.position(c) == 0);
    REQUIRE(set.position(b) == 1);
    REQUIRE(set.size() == 2);
    // Erasing the last element swaps nothing.
    REQUIRE(set.erase(b, &moved) == 1);
    REQUIRE(moved == 0);
    // Erasing an absent id reports -1 and no swap.
    REQUIRE(set.erase(a, &moved) == -1);
    REQUIRE(moved == 0);
    REQUIRE(set.size() == 1);
}

TEST_CASE("DenseIdSet misses a stale generation on a reused slot", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId old_id = test_id(1, 7);
    const engine_core::InstanceId new_id = test_id(2, 7);
    set.insert(old_id);
    REQUIRE_FALSE(set.contains(new_id));
    REQUIRE(set.position(new_id) == -1);
    set.erase(old_id);
    set.insert(new_id);
    REQUIRE_FALSE(set.contains(old_id));
    REQUIRE(set.contains(new_id));
}

TEST_CASE("DenseIdSet clear empties and forgets positions", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId a = test_id(1, 4);
    set.insert(a);
    set.clear();
    REQUIRE(set.size() == 0);
    REQUIRE_FALSE(set.contains(a));
    REQUIRE(set.insert(a));
    REQUIRE(set.position(a) == 0);
}
```

In `CMakeLists.txt`, add the file to the sandbox target's sources after `sandbox/scene_services_tests.cpp` (line 474):

```cmake
    sandbox/scene_services_tests.cpp
    sandbox/dense_views_tests.cpp
```

- [ ] **Step 2: Run to verify the tests fail**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -5`
Expected: compile FAILURE — `DenseIdSet.hpp: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `src/engine_core/DenseIdSet.hpp`:

```cpp
#pragma once

#include "Contract.hpp"
#include "types.hpp"

#include <cstddef>
#include <vector>

namespace engine_core {

// Dense set of live instance ids. ids() is packed and unordered; contains,
// position, insert, and erase are O(1) through a per-slot position map keyed
// by id_slot. A stale generation misses because the mapped entry no longer
// holds the queried id. erase is swap-and-pop and reports the swap, so an
// owner keeping a parallel payload array mirrors it. Not thread-safe: the
// owner touches it under whatever already guards its writes.
class DenseIdSet {
public:
    // Sizes the position map and the dense array. Call once, before use;
    // nothing here allocates afterwards.
    void reserve(std::size_t slots) {
        index_.assign(slots, -1);
        dense_.reserve(slots);
    }

    std::size_t size() const { return dense_.size(); }
    const std::vector<InstanceId>& ids() const { return dense_; }
    bool contains(InstanceId id) const { return position(id) >= 0; }

    // Position of id in ids(), or -1 when absent.
    int position(InstanceId id) const {
        const std::uint32_t slot = id_slot(id);
        if (slot >= index_.size()) {
            return -1;
        }
        const int pos = index_[slot];
        if (pos < 0 || dense_[static_cast<std::size_t>(pos)] != id) {
            return -1;
        }
        return pos;
    }

    // False when id is already present. A full set fails the contract.
    bool insert(InstanceId id) {
        if (position(id) >= 0) {
            return false;
        }
        const std::uint32_t slot = id_slot(id);
        if (slot >= index_.size() || dense_.size() == dense_.capacity()) {
            contract_fail("DenseIdSet capacity exhausted");
        }
        index_[slot] = static_cast<int>(dense_.size());
        dense_.push_back(id);
        return true;
    }

    // Removes id. Returns the position it vacated, or -1 when absent. When
    // another id was swapped into that position, *moved names it, else 0.
    int erase(InstanceId id, InstanceId* moved = nullptr) {
        if (moved != nullptr) {
            *moved = 0;
        }
        const int pos = position(id);
        if (pos < 0) {
            return -1;
        }
        const InstanceId last = dense_.back();
        dense_[static_cast<std::size_t>(pos)] = last;
        dense_.pop_back();
        index_[id_slot(id)] = -1;
        if (last != id) {
            index_[id_slot(last)] = pos;
            if (moved != nullptr) {
                *moved = last;
            }
        }
        return pos;
    }

    void clear() {
        for (const InstanceId id : dense_) {
            index_[id_slot(id)] = -1;
        }
        dense_.clear();
    }

private:
    std::vector<InstanceId> dense_;
    // id_slot -> position in dense_, -1 when absent.
    std::vector<int> index_;
};

}  // namespace engine_core
```

Note: `contract_fail` — confirm the exact header (`Contract.hpp`) and namespace by looking at how `src/engine_core/InvalidationQueue.cpp` or `SnapshotPump.cpp` fails; match it.

- [ ] **Step 4: Run to verify the tests pass**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe "[dense]" 2>&1 | tail -3`
Expected: all `[dense]` assertions pass.

- [ ] **Step 5: Run the full suite, then commit**

Run: `./build/Debug/sandbox.exe 2>&1 | tail -3`
Expected: 0 failures.

```bash
git add src/engine_core/DenseIdSet.hpp sandbox/dense_views_tests.cpp CMakeLists.txt
git commit -m "Add DenseIdSet, the dense id primitive for views

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: SnapshotPump on DenseIdSet

Behavior-neutral refactor: replace the pump's hand-rolled `base_index_` / `remember` / `erase_base` with a `DenseIdSet` mirroring `base_.instances`. No test changes; the existing suite is the check.

**Files:**
- Modify: `src/engine_core/SnapshotPump.hpp` (members and private helpers)
- Modify: `src/engine_core/SnapshotPump.cpp` (reserve, erase_base, apply_live, resync, apply_overrides, base_find)

**Interfaces:**
- Consumes: `DenseIdSet` from Task 1.
- Produces: nothing new; the pump's public API is unchanged. Task 5 relies on the private shape: `DenseIdSet base_ids_` mirrored index-for-index by `base_.instances`.

- [ ] **Step 1: Replace the members**

In `SnapshotPump.hpp`: add `#include "DenseIdSet.hpp"`; delete the members `std::vector<int> base_index_;` and the declarations `void remember(InstanceId id, int position);` and `void erase_base(InstanceId id);` — keep `erase_base` (reimplemented) but delete `remember`. Add `DenseIdSet base_ids_;`.

- [ ] **Step 2: Rewrite the implementation**

In `SnapshotPump.cpp`:

`reserve` (sizes `base_index_` today): replace the `base_index_` sizing with `base_ids_.reserve(DataModel::kMaxInstances);` (keep the existing `instances` reserves).

`base_find`:

```cpp
VisualInstance* SnapshotPump::base_find(InstanceId id) {
    const int pos = base_ids_.position(id);
    return pos < 0 ? nullptr : &base_.instances[static_cast<std::size_t>(pos)];
}
```

`erase_base` (delete `remember` entirely):

```cpp
void SnapshotPump::erase_base(InstanceId id) {
    const int pos = base_ids_.erase(id);
    if (pos < 0) {
        return;
    }
    // DenseIdSet swapped its last id into pos; mirror that on the rows.
    base_.instances[static_cast<std::size_t>(pos)] = base_.instances.back();
    base_.instances.pop_back();
}
```

`apply_live` insertion path (currently `remember(...)` + `push_back`):

```cpp
    VisualInstance* inst = base_find(change.id);
    if (inst == nullptr) {
        if (base_.instances.size() == base_.instances.capacity()) {
            contract_fail("snapshot instance capacity exhausted");
        }
        base_ids_.insert(change.id);
        base_.instances.push_back(VisualInstance{});
        inst = &base_.instances.back();
        inst->id = change.id;
    }
```

`resync`: replace `base_.instances.clear(); std::fill(base_index_...)` with `base_.instances.clear(); base_ids_.clear();` and replace `remember(object.id(), ...)` before each `push_back` with `base_ids_.insert(object.id());`.

`apply_overrides`: replace the `id_slot`/`base_index_` lookup with:

```cpp
    for (const SnapshotOverride& override : overrides_) {
        const int position = base_ids_.position(override.id);
        if (position < 0 || static_cast<std::size_t>(position) >= dst.instances.size()) {
            continue;
        }
        VisualInstance& inst = dst.instances[static_cast<std::size_t>(position)];
        // ... rest unchanged (the inst.id != override.id guard can stay; it is
        // now redundant but harmless) ...
```

If `std::fill`/`<algorithm>` becomes unused, drop the include.

- [ ] **Step 3: Build and run the full suite**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe 2>&1 | tail -3`
Expected: 0 failures — this refactor must not change behavior. Pay attention to `[T4]`, `[T9]`, and any test using `pump().find`.

- [ ] **Step 4: Commit**

```bash
git add src/engine_core/SnapshotPump.hpp src/engine_core/SnapshotPump.cpp
git commit -m "Refactor SnapshotPump onto DenseIdSet

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Scope bits

`in_game` and `in_workspace` on `Slot`, maintained at every tree-mutation completion point, readable through two new accessors. Nothing consumes them yet.

**Files:**
- Modify: `src/engine_core/DataModel.hpp` (Slot at :421, accessor decls near `alive` at :~318, private decls)
- Modify: `src/engine_core/DataModelState.hpp` (scope scratch vector)
- Modify: `src/engine_core/DataModel.cpp` (refresh_scope + apply_scope; hooks in set_parent :1014, detach_links :981, destroy :463; reserve in the root constructor :40)
- Modify: `src/engine_core/DataModelPlace.cpp` (hook in link_children :215)
- Test: `sandbox/dense_views_tests.cpp`

**Interfaces:**
- Consumes: `scene_service("Workspace")` (`DataModel.hpp:141`), `slot()`, sibling links, `current_origin()`.
- Produces: `bool DataModel::in_game(InstanceId) const`, `bool DataModel::in_workspace(InstanceId) const` (false for dead ids); `void refresh_scope(InstanceId)` (private, idempotent); `void apply_scope(InstanceId, bool in_game, bool in_workspace)` (private; Tasks 4 and 5 extend its flip handling). `Slot::in_game`, `Slot::in_workspace`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/dense_views_tests.cpp`. These use `SimRole` + `engine_core::Game` + `workspace_of` from `support.hpp` (see `ScriptRig` in `support.hpp:44` for the pattern; `Game.hpp` is already included there).

```cpp
TEST_CASE("scope bits follow the tree", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    const engine_core::InstanceId storage = game.scene_service("Storage");
    REQUIRE(game.in_game(ws));
    REQUIRE_FALSE(game.in_workspace(ws));  // Workspace is not inside itself

    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    REQUIRE_FALSE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));

    game.set_parent(id, ws);
    REQUIRE(game.in_game(id));
    REQUIRE(game.in_workspace(id));

    game.set_parent(id, storage);
    REQUIRE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));

    game.set_parent(id, engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));

    REQUIRE_FALSE(game.in_game(engine_core::make_instance_id(9, 999)));  // dead id
}

TEST_CASE("scope bits flip a whole subtree", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& part = game.create_game_object();
    engine_core::GameObject& nested = game.create_game_object();
    game.set_parent(part.id(), folder.id());
    game.set_parent(nested.id(), part.id());
    REQUIRE_FALSE(game.in_workspace(nested.id()));

    game.set_parent(folder.id(), ws);
    REQUIRE(game.in_workspace(folder.id()));
    REQUIRE(game.in_workspace(part.id()));
    REQUIRE(game.in_workspace(nested.id()));

    game.set_parent(folder.id(), engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(part.id()));
    REQUIRE_FALSE(game.in_workspace(nested.id()));
}

TEST_CASE("destroy clears scope; orphans leave scope; undo restores it", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::GameObject& parent = game.create_game_object();
    engine_core::GameObject& child = game.create_game_object();
    game.set_parent(parent.id(), ws);
    game.set_parent(child.id(), parent.id());
    const engine_core::InstanceId parent_id = parent.id();
    const engine_core::InstanceId child_id = child.id();

    // destroy (not destroy_tree) orphans the child out of the tree.
    game.destroy(parent_id);
    REQUIRE_FALSE(game.in_workspace(parent_id));  // dead reads false
    REQUIRE(game.alive(child_id));
    REQUIRE_FALSE(game.in_game(child_id));
    REQUIRE_FALSE(game.in_workspace(child_id));

    // Undo revives the parent under Workspace and reparents the child back.
    game.history().undo();
    REQUIRE(game.alive(parent_id));
    REQUIRE(game.in_workspace(parent_id));
    REQUIRE(game.in_workspace(child_id));
}
```

Note on the undo test: verify with `sandbox/history_tests.cpp:39` (`H1`) how a destroy is undone in one step; if destroy+reparent takes two `undo()` calls in this history model, call `undo()` until `can_undo().first` is false and assert the final state.

```cpp
TEST_CASE("Stop restores scope with the place", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::GameObject& authored = game.create_game_object();
    game.set_parent(authored.id(), ws);
    game.capture_place();
    game.start_simulation();
    engine_core::GameObject& session = game.create_game_object();
    game.set_parent(session.id(), ws);
    const engine_core::InstanceId session_id = session.id();
    game.set_parent(authored.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(game.in_workspace(authored.id()));
    game.stop_simulation();
    REQUIRE(game.in_workspace(authored.id()));  // back under Workspace with the place
    REQUIRE_FALSE(game.alive(session_id));      // session object destroyed, bits cleared
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -5`
Expected: compile FAILURE — `in_game` is not a member of `DataModel`.

- [ ] **Step 3: Implement**

`DataModel.hpp` — in `struct Slot` (:421), after `bool visual_only = false;`:

```cpp
        // Scope: reachable from the root, and under the Workspace service.
        // refresh_scope maintains both at every tree mutation.
        bool in_game = false;
        bool in_workspace = false;
```

Public accessors, next to `alive(InstanceId)`:

```cpp
    // Scope. Both are false for a dead id. in_workspace(workspace) is false:
    // the service is not inside itself.
    bool in_game(InstanceId id) const;
    bool in_workspace(InstanceId id) const;
```

Private declarations, near `link_child`:

```cpp
    // Recomputes id's scope bits from its parent. Unchanged bits return at
    // once; changed bits walk the subtree and fire membership events.
    void refresh_scope(InstanceId id);
    void apply_scope(InstanceId id, bool in_game, bool in_workspace);
```

`DataModelState.hpp` — next to `step_ids`:

```cpp
    // Ids gathered by apply_scope's subtree walk. walk belongs to emit_ancestry.
    std::vector<InstanceId> scope_walk;
```

`DataModel.cpp` — reserve in the root constructor (:40 block, after `step_ids.reserve`):

```cpp
    world.scope_walk.reserve(kMaxInstances);
```

Accessors and the scope engine:

```cpp
bool DataModel::in_game(InstanceId id) const {
    const Slot* part = slot(id);
    return part != nullptr && part->in_game;
}

bool DataModel::in_workspace(InstanceId id) const {
    const Slot* part = slot(id);
    return part != nullptr && part->in_workspace;
}

void DataModel::refresh_scope(InstanceId id) {
    Slot* part = slot(id);
    if (part == nullptr) {
        return;
    }
    bool game = false;
    bool workspace = false;
    if (part->parent == 0) {
        game = true;
    } else if (part->parent != kNoParent) {
        if (const Slot* holder = slot(part->parent)) {
            game = holder->in_game;
            workspace = holder->in_workspace || part->parent == scene_service("Workspace");
        }
    }
    if (part->in_game == game && part->in_workspace == workspace) {
        return;
    }
    apply_scope(id, game, workspace);
}

void DataModel::apply_scope(InstanceId id, bool in_game, bool in_workspace) {
    // Top-down over the subtree. A node whose bits come out unchanged prunes
    // its children: their stored bits were derived from its stored bits.
    const InstanceId workspace_id = scene_service("Workspace");
    std::vector<InstanceId>& queue = state_->scope_walk;
    queue.clear();
    queue.push_back(id);
    bool game = in_game;
    bool workspace = in_workspace;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const InstanceId cur = queue[i];
        Slot* part = slot(cur);
        if (part == nullptr) {
            continue;
        }
        if (i > 0) {
            const Slot* holder = slot(part->parent);
            if (holder == nullptr) {
                continue;
            }
            game = holder->in_game;
            workspace = holder->in_workspace || part->parent == workspace_id;
        }
        if (part->in_game == game && part->in_workspace == workspace) {
            continue;
        }
        part->in_game = game;
        part->in_workspace = workspace;
        // Membership events land here: the step list (Task 4) on an in_game
        // flip, the Ancestry invalidation (Task 5) on an in_workspace flip.
        for (InstanceId child = part->first_child; child != 0;) {
            if (queue.size() == queue.capacity()) {
                contract_fail("scope walk capacity exhausted");
            }
            queue.push_back(child);
            const Slot* child_slot = slot(child);
            if (child_slot == nullptr) {
                break;
            }
            child = child_slot->next_sibling;
        }
    }
}
```

Hooks:

1. `set_parent` (:1014): directly after the `unlink_parent` / `link_child` block (lines 1039-1042, before `record_parent`), add `refresh_scope(id);`.
2. `detach_links` (:981): inside the child loop, right after the three lines that clear the child's parent and siblings, add `refresh_scope(child);` — the child just left the tree, so its subtree's bits clear. (This also runs for a destroyed instance's orphans via `destroy`, and for `DataModelPlace.cpp:127`.)
3. `destroy` (:463): where the slot is released (next to `++part->generation`), add `part->in_game = false; part->in_workspace = false;` so a reused slot starts unscoped.
4. `link_children` (`DataModelPlace.cpp:215`): inside the loop, after `link_child(parent, child);`, add `refresh_scope(child);`. This covers place restore (`:276`, `:278`) and history's sibling reorder (`DataModelHistory.cpp:356`, where it is a cheap no-op).
5. Check for any other `link_child(`/`unlink_parent(` callers: `grep -n "link_child(\|unlink_parent(" src/engine_core/*.cpp`. Every caller must be followed by a `refresh_scope` on the moved id (or be inside `set_parent`/`link_children`/`detach_links`, which now handle it). If history's revive path (`DataModelHistory.cpp` around `revive_record`) links through `set_parent` or `link_children` it is covered; if it links some other way, add the same one-line hook there.
6. `clear_hierarchy` (`DataModelPlace.cpp`, called by the place restore) leaves every slot's bits stale on purpose: restore then either relinks each live instance through `link_children` (which refreshes it, per hook 4) or destroys it (which clears its bits, per hook 3). Add that sentence as a comment on `clear_hierarchy`. The "Stop restores scope" test proves it.

- [ ] **Step 4: Run to verify the scope tests pass**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe "[scope]" 2>&1 | tail -3`
Expected: PASS.

- [ ] **Step 5: Run the full suite, then commit**

Run: `./build/Debug/sandbox.exe 2>&1 | tail -3`
Expected: 0 failures (bits are not consumed yet, so nothing else may move).

```bash
git add src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelState.hpp src/engine_core/DataModelPlace.cpp sandbox/dense_views_tests.cpp
git commit -m "Track in_game and in_workspace scope bits on every slot

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: The step list

Heartbeat iterates a dense list of stepping instances; `step_descendants` and its tree walk die.

**Files:**
- Modify: `src/engine_core/DataModel.hpp` (`steps()` next to `step()` at :162; replace `void step_descendants(double dt);` at :334 with `void step_instances(double dt);`)
- Modify: `src/engine_core/DataModelState.hpp` (step list storage)
- Modify: `src/engine_core/DataModel.cpp` (list maintenance in `apply_scope` and `destroy`; `step_instances`; delete `step_descendants` at :657; reserves at :40)
- Modify: `src/engine_instances/TestTriangle.hpp` (`steps()` override)
- Modify: `src/engine_core/Engine.cpp:299` (call site)
- Test: `sandbox/dense_views_tests.cpp`

**Interfaces:**
- Consumes: `apply_scope`'s flip point (Task 3), `DenseIdSet` (Task 1).
- Produces: `virtual bool DataModel::steps() const` (false; TestTriangle true), `void DataModel::step_instances(double dt)` (SimulationThread, called by Engine's Heartbeat), `State::step_set` (`DenseIdSet`) + `State::steppers` (`std::vector<DataModel*>`, parallel), private `void step_list_insert(InstanceId, DataModel*)` / `void step_list_erase(InstanceId)`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/dense_views_tests.cpp` (`TestTriangle.hpp` include is needed: add `#include "TestTriangle.hpp"` at the top):

```cpp
TEST_CASE("a triangle steps only while it is under game", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
    const engine_core::InstanceId id = triangle.id();

    game.step_instances(0.25);  // unparented: nothing steps
    REQUIRE(triangle.angle_degrees() == 0.0);

    game.set_parent(id, ws);
    game.step_instances(0.25);  // 90 deg/s
    REQUIRE(triangle.angle_degrees() == 22.5);

    game.set_parent(id, engine_core::DataModel::kNoParent);
    game.step_instances(0.25);
    REQUIRE(triangle.angle_degrees() == 22.5);

    game.set_parent(id, ws);
    game.destroy(id);
    game.step_instances(0.25);  // erased on destroy; must not touch freed state
    // TestTriangle.hpp: a dead id reads as 0. If the released storage reads
    // recycled state instead, assert !game.alive(id) and drop this line.
    REQUIRE(triangle.angle_degrees() == 0.0);
}

TEST_CASE("a step may destroy another stepper mid-frame", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::TestTriangle& a = game.create<engine_core::TestTriangle>();
    engine_core::TestTriangle& b = game.create<engine_core::TestTriangle>();
    game.set_parent(a.id(), ws);
    game.set_parent(b.id(), ws);
    // Destroy b from outside, then step: the stale id in the frame's copy
    // must be skipped, not dereferenced.
    game.destroy(b.id());
    game.step_instances(0.25);
    REQUIRE(a.angle_degrees() == 22.5);
}

TEST_CASE("plain instances and services never enter the step list", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    game.set_parent(folder.id(), workspace_of(game));
    game.step_instances(0.25);  // nothing to step; must not crash or walk the tree
    REQUIRE(game.stepper_count() == 0);
    engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
    game.set_parent(triangle.id(), workspace_of(game));
    REQUIRE(game.stepper_count() == 1);
}
```

One small test hook this needs on `DataModel` (public, next to the other test-facing readers): `std::size_t stepper_count() const` — implemented in Step 3.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -5`
Expected: compile FAILURE — `step_instances` is not a member.

- [ ] **Step 3: Implement**

`DataModel.hpp`:

- Next to `step()` (:162): `// True for a class Heartbeat steps. Read when scope changes, never per frame.` / `virtual bool steps() const { return false; }`
- Replace `void step_descendants(double dt);` (:334) with:

```cpp
    // Heartbeat. Steps the dense list of stepping instances under game, in
    // unspecified order, over a stable copy so a step may create, destroy,
    // or reparent instances.
    void step_instances(double dt);
    std::size_t stepper_count() const;
```

- Private: `void step_list_insert(InstanceId id, DataModel* instance);` / `void step_list_erase(InstanceId id);`

`TestTriangle.hpp`, next to `class_name`: `bool steps() const override { return true; }`

`DataModelState.hpp`, next to `step_ids`:

```cpp
    // The instances Heartbeat steps: in_game and steps(). steppers mirrors
    // step_set position for position, so the loop needs no slot lookups.
    DenseIdSet step_set;
    std::vector<DataModel*> steppers;
```

(`DataModelState.hpp` needs `#include "DenseIdSet.hpp"`.)

`DataModel.cpp`:

- Constructor (:40 block): `world.step_set.reserve(kMaxInstances); world.steppers.reserve(kMaxInstances);`
- List maintenance:

```cpp
void DataModel::step_list_insert(InstanceId id, DataModel* instance) {
    if (state_->step_set.insert(id)) {
        state_->steppers.push_back(instance);
    }
}

void DataModel::step_list_erase(InstanceId id) {
    const int pos = state_->step_set.erase(id);
    if (pos < 0) {
        return;
    }
    state_->steppers[static_cast<std::size_t>(pos)] = state_->steppers.back();
    state_->steppers.pop_back();
}

std::size_t DataModel::stepper_count() const { return state_->step_set.size(); }
```

- In `apply_scope`, at the membership-events comment from Task 3, add (the flip test compares the OLD stored bit, so place this before the two `part->in_...` assignments and restructure like this):

```cpp
        const bool game_flip = part->in_game != game;
        part->in_game = game;
        part->in_workspace = workspace;
        if (game_flip && part->instance != nullptr && part->instance->steps()) {
            if (game) {
                step_list_insert(cur, part->instance);
            } else {
                step_list_erase(cur);
            }
        }
```

- In `destroy` (:463), next to the bit clearing added in Task 3: `step_list_erase(id);`
- Replace `step_descendants` (:657) wholesale:

```cpp
void DataModel::step_instances(double dt) {
    // A stable copy: a step may create, destroy, or reparent, which edits the
    // dense list. Ids gone stale by the time their turn comes are skipped.
    std::vector<InstanceId>& ids = state_->step_ids;
    ids.clear();
    const std::vector<InstanceId>& live = state_->step_set.ids();
    ids.insert(ids.end(), live.begin(), live.end());
    for (const InstanceId id : ids) {
        const int pos = state_->step_set.position(id);
        if (pos < 0) {
            continue;
        }
        state_->steppers[static_cast<std::size_t>(pos)]->step(dt);
    }
}
```

`Engine.cpp:299`: `game_.step_descendants(render_dt_);` → `game_.step_instances(render_dt_);` (keep the comment, reworded: `// The dense step list runs in this phase. Bound Heartbeat jobs stay for callers that are not instances.`)

- [ ] **Step 4: Run to verify the step tests pass**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe "[step]" 2>&1 | tail -3`
Expected: PASS.

- [ ] **Step 5: Run the full suite, then commit**

Run: `./build/Debug/sandbox.exe 2>&1 | tail -3`
Expected: 0 failures. The triangle tests around `sandbox/tests.cpp:1018-1061` and `:1306`, `:1390` already parent triangles into Workspace, so they keep stepping; if any triangle test creates one unparented and expects it to step, parent it with `game.set_parent(triangle.id(), workspace_of(game))` — that is the new, correct contract.

```bash
git add src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelState.hpp src/engine_instances/TestTriangle.hpp src/engine_core/Engine.cpp sandbox/dense_views_tests.cpp
git commit -m "Step Heartbeat over a dense list instead of walking the tree

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Snapshot membership

A row exists iff `alive && GameObject && in_workspace`, kept current by `VisualField::Ancestry`.

**Files:**
- Modify: `src/engine_core/types.hpp:42` (Ancestry bit)
- Modify: `src/engine_core/DataModel.cpp` (`apply_scope` emits Ancestry)
- Modify: `src/engine_core/SnapshotPump.cpp` (`apply_live`, `resync`)
- Test: `sandbox/dense_views_tests.cpp`

**Interfaces:**
- Consumes: `apply_scope`'s flip point (Task 3/4 shape), `in_workspace(id)` (Task 3), `note(id, fields, origin)` (`DataModel.cpp:258`), pump internals on `base_ids_` (Task 2).
- Produces: `VisualField::Ancestry = 1u << 4`. Membership semantics every later renderer relies on.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/dense_views_tests.cpp`. Drive the pump the way `sandbox/tests.cpp:1700` does (see that block for the exact call pattern):

```cpp
namespace {

// One Prepare, without engine threads: the caller is both roles here.
void pump_frame(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
}

}  // namespace

TEST_CASE("only Workspace GameObjects have snapshot rows", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId ws = workspace_of(game);
    const engine_core::InstanceId storage = game.scene_service("Storage");

    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);  // unparented: no row

    game.set_parent(id, ws);
    pump_frame(pump, game);
    REQUIRE(pump.find(id) != nullptr);

    game.set_parent(id, storage);
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);

    game.set_parent(id, ws);
    game.set_parent(id, engine_core::DataModel::kNoParent);  // leaving the tree removes the row
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);
}

TEST_CASE("a row arrives complete after edits made outside Workspace", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId storage = game.scene_service("Storage");

    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), storage);
    pump_frame(pump, game);
    part.set_color(rgb(0.25f, 0.5f, 0.75f));  // recolored while it has no row
    part.set_size(2.f, 3.f, 4.f);
    pump_frame(pump, game);
    REQUIRE(pump.find(part.id()) == nullptr);

    game.set_parent(part.id(), workspace_of(game));
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->color.r == 0.25f);
    REQUIRE(row->size[1] == 3.f);
}

TEST_CASE("destroy_tree clears rows for a whole subtree", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& a = game.create_game_object();
    engine_core::GameObject& b = game.create_game_object();
    game.set_parent(folder.id(), ws);
    game.set_parent(a.id(), folder.id());
    game.set_parent(b.id(), folder.id());
    const engine_core::InstanceId a_id = a.id();
    const engine_core::InstanceId b_id = b.id();
    pump_frame(pump, game);
    REQUIRE(pump.find(a_id) != nullptr);
    REQUIRE(pump.find(b_id) != nullptr);
    game.destroy_tree(folder.id());
    pump_frame(pump, game);
    REQUIRE(pump.find(a_id) == nullptr);
    REQUIRE(pump.find(b_id) == nullptr);
    REQUIRE(game.stepper_count() == 0);
}

TEST_CASE("a move within Workspace keeps the row", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::DataModel& folder = game.create();
    game.set_parent(folder.id(), ws);
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), ws);
    part.set_color(rgb(0.1f, 0.2f, 0.3f));
    pump_frame(pump, game);
    REQUIRE(pump.find(part.id()) != nullptr);

    game.set_parent(part.id(), folder.id());  // still under Workspace: no flicker
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->color.g == 0.2f);
}

TEST_CASE("overflow resync keeps Workspace membership", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& shown = game.create_game_object();
    engine_core::GameObject& stored = game.create_game_object();
    game.set_parent(shown.id(), workspace_of(game));
    game.set_parent(stored.id(), game.scene_service("Storage"));
    // Overflow the ring so the next take_changes resyncs from scratch.
    for (std::size_t i = 0; i <= engine_core::DataModel::kMaxInvalidations; ++i) {
        shown.set_transform(engine_core::transform_identity());
    }
    REQUIRE(game.invalidations().overflow());
    pump_frame(pump, game);
    REQUIRE(pump.find(shown.id()) != nullptr);
    REQUIRE(pump.find(stored.id()) == nullptr);
}
```

Check `rgb` and `transform_identity` helpers exist as used (`support.hpp:85`, `types.hpp`); check whether `set_transform` on an equal transform still pushes an invalidation (see `GameObject.cpp` / `DataModel.cpp:545`) — if equal values skip the note, vary the transform per loop iteration to force the overflow.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe "[member]" 2>&1 | tail -5`
Expected: build OK; the first `[member]` test FAILS (unparented object currently gets a row).

- [ ] **Step 3: Implement**

`types.hpp:42`: add `Ancestry = 1u << 4` after `Removed`, with the comment `// Scope changed: membership must be re-evaluated, and a joining row read whole.`

`DataModel.cpp`, in `apply_scope`'s flip block (extending Task 4's shape):

```cpp
        const bool game_flip = part->in_game != game;
        const bool workspace_flip = part->in_workspace != workspace;
        part->in_game = game;
        part->in_workspace = workspace;
        if (game_flip && part->instance != nullptr && part->instance->steps()) {
            if (game) {
                step_list_insert(cur, part->instance);
            } else {
                step_list_erase(cur);
            }
        }
        if (workspace_flip && part->body != nullptr) {
            note(cur, VisualField::Ancestry, current_origin());
        }
```

`SnapshotPump.cpp` `apply_live` — insert the scope gate and the Ancestry refresh (the erase branch and the field patches already exist):

```cpp
void SnapshotPump::apply_live(DataModel& game, const Invalidation& change) {
    if (any(change.fields, VisualField::Removed) || !game.alive(change.id)) {
        erase_base(change.id);
        return;
    }
    if (!game.in_workspace(change.id)) {
        // Out of scope: an Ancestry flip out of Workspace, or a stray note
        // for an object that has no row. Either way the row goes.
        erase_base(change.id);
        return;
    }
    const GameObject* object = game.game_object(change.id);
    if (object == nullptr) {
        return;
    }
    VisualInstance* inst = base_find(change.id);
    if (inst == nullptr) {
        if (base_.instances.size() == base_.instances.capacity()) {
            contract_fail("snapshot instance capacity exhausted");
        }
        base_ids_.insert(change.id);
        base_.instances.push_back(VisualInstance{});
        inst = &base_.instances.back();
        inst->id = change.id;
    }
    const bool joined = any(change.fields, VisualField::Ancestry);
    if (joined || any(change.fields, VisualField::Transform)) {
        inst->world = object->transform();
        inst->transform_origin = change.origin;
    }
    if (joined || any(change.fields, VisualField::Color)) {
        inst->color = object->color();
        inst->color_origin = change.origin;
    }
    if (joined || any(change.fields, VisualField::Size)) {
        if (object->copy_size(inst->size)) {
            inst->size_origin = change.origin;
        }
    }
    inst->alive = true;
}
```

`resync` gains one line at the top of the lambda:

```cpp
    game.for_each_game_object([&](const GameObject& object) {
        if (!game.in_workspace(object.id())) {
            return;
        }
        ...
```

(`resync`/`apply_live` take `DataModel& game`; `for_each_game_object` is const — if the lambda's `game.in_workspace` fails on constness, the accessor is const, so it compiles as is.)

- [ ] **Step 4: Run to verify the member tests pass**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe "[member]" 2>&1 | tail -3`
Expected: PASS.

- [ ] **Step 5: Run the full suite — EXPECT failures, do not fix them here**

Run: `./build/Debug/sandbox.exe 2>&1 | tail -3`
Expected: the `[dense]` tags pass; a batch of older tests in `sandbox/tests.cpp` now fail because they create unparented GameObjects and expect rows. That migration is Task 6 — commit this task only if the ONLY failures are of that shape (list them; each failing test must be reading `pump().find`/snapshot state for an unparented object). Any other failure is a bug in this task: stop and fix it first.

```bash
git add src/engine_core/types.hpp src/engine_core/DataModel.cpp src/engine_core/SnapshotPump.cpp sandbox/dense_views_tests.cpp
git commit -m "Limit the snapshot to Workspace GameObjects via Ancestry invalidations

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Test migration and the Lua regression

**Files:**
- Modify: `sandbox/support.hpp` (create_part helper)
- Modify: `sandbox/tests.cpp` (the create_game_object call sites)
- Modify: `sandbox/dense_views_tests.cpp` (Lua regression test)

**Interfaces:**
- Consumes: `workspace_of` (`support.hpp:80`), everything above.
- Produces: `engine_core::GameObject& create_part(engine_core::DataModel& game)` — create + parent under Workspace; the helper every later snapshot-reading test uses.

- [ ] **Step 1: Add the helper**

In `sandbox/support.hpp`, after `workspace_of`:

```cpp
// A GameObject parented under Workspace, so it has a snapshot row.
inline engine_core::GameObject& create_part(engine_core::DataModel& game) {
    engine_core::GameObject& object = game.create_game_object();
    game.set_parent(object.id(), workspace_of(game));
    return object;
}
```

- [ ] **Step 2: Migrate the call sites**

`grep -n "create_game_object" sandbox/tests.cpp` — 24 sites (133, 146, 198, 250, 324, 389, 466, 520, 563, 627, 685, 720, 765, 795, 858, 900, 947, 1001, 1074, 1241, and the rest the grep prints). For each, decide:

- The test reads the snapshot (`pump().find`, `front().instances`, screen state) or steps physics on it → `create_part(...)`.
- The test only exercises DataModel state (names, hierarchy, properties, contract failures) → leave it as is; rows are irrelevant to it.
- `[T11]` (`tests.cpp:511` block) creates 10000 simulated parts and clears the queue; its rejection assertions don't need rows — leave unparented ONLY if it still passes; if its budget arithmetic depended on copied rows, use `create_part` there too and re-check the timing assertion.

One caution: some tests run their creates inside `engine.on_simulation(...)` or with engine threads live; `create_part` calls `set_parent`, which is a SimulationThread call — inside those blocks that is already the right thread. Tests that create from the test thread with threads running would enqueue; none of the 24 sites should be in that state, but if one is, parent it inside the paused-edit block the test already uses.

- [ ] **Step 3: Add the Lua regression test**

Append to `sandbox/dense_views_tests.cpp` (pattern: `ScriptRig` + `add_script` as in `sandbox/tests.cpp:2437`):

```cpp
TEST_CASE("Instance.new GameObject renders once parented into Workspace", "[dense][member][lua]") {
    ScriptRig rig;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    add_script(rig.game, "Spawner", R"(
        local part = Instance.new("GameObject")
        part.Name = "Spawned"
        part.Parent = workspace
    )");
    rig.frames(2);
    const engine_core::InstanceId id = rig.game.find_first_child(workspace_of(rig.game), "Spawned");
    REQUIRE(id != 0);
    pump.prepare_copy(rig.game);
    pump.publish();
    REQUIRE(pump.find(id) != nullptr);
}
```

`find_first_child(parent, name)` is `DataModel.hpp:193`. Check `add_script`'s signature in `support.hpp`/`tests.cpp` and match it.

- [ ] **Step 4: Full suite, both configs**

Run: `cmake --build build --target sandbox --parallel 2>&1 | tail -3 && ./build/Debug/sandbox.exe 2>&1 | tail -3`
Expected: 0 failures, ~310+ cases.

Then ThreadSanitizer per `src/engine_core/README.md` — on this Windows/MSVC setup TSAN is unavailable; if `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENGINE_CORE_TSAN=ON` fails to configure, note that in the commit message and move on rather than fighting the toolchain.

- [ ] **Step 5: Commit**

```bash
git add sandbox/support.hpp sandbox/tests.cpp sandbox/dense_views_tests.cpp
git commit -m "Parent test parts into Workspace and cover script-made parts

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
