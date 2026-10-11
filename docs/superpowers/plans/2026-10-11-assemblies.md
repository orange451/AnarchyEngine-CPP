# Assemblies Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `WeldConstraint` and `RigidConstraint` join PVInstances into assemblies that move as one rigid thing: compound Box3D bodies in play, a live RigidConstraint snap in the editor, a deterministic root, and copies that keep their links.

**Architecture:**
- **Classes.** `Constraint` (abstract) and its two creatable subclasses live in `engine_instances/Constraint.{hpp,cpp}`. They are plain instances with two `PVInstance?` references, tagged in flecs so a cached query finds those in Workspace.
- **Assemblies.** A new engine_core unit, `Assemblies`, is owned by DataModel state, keyed by its own revision, and rebuilt lazily. It is the only code that groups nodes, picks roots, and decides `Active`. The work splits across three files:
  - `Assemblies.cpp`: the graph;
  - `AssemblyFollow.cpp`: the editor's RigidConstraint follow;
  - `AssemblyPlay.cpp`: play capture, whole-assembly moves, pending velocities, and membership diffs.
- **Hooks.** DataModel, PhysicsBase, Attachment, and Constraint call small hook entry points on `Assemblies`. Each returns at once for slots that are not linked.
- **Physics.** `PhysicsWorld` keeps its single-body path for lone nodes, untouched. It adds a compound-body path for multi-node assemblies, reading the play state `Assemblies` captures.

**Tech Stack:** C++17 (MSVC 14.23, CMake 3.16), Box3D at `9f998c86`, flecs, Luau, Catch2 (sandbox), the studio test programs, JadeFX.

**Spec:** `docs/superpowers/specs/2026-10-11-assemblies-design.md`. Read it with this plan; section numbers below (§2, §4…) are the spec's.

## Global Constraints

- **Words.** Engine distances are "units", never "studs": in code, comments, tests, and messages.
- **Where to work.** Work only in the worktree `C:/Users/Andrew/Documents/GitHub/AnarchyEngine-CPP/.claude/worktrees/assemblies`, branch `assemblies`. Never edit, stash, or switch the main checkout; another session works there.
- **Compiler.** C++17 only (`CMAKE_CXX_STANDARD 17`): no `std::span`, no designated initializers, no concepts. MSVC `/W4` must stay warning-free on touched code. Use `std::numeric_limits<float>::max()`, not an FLT_MAX literal. Use `engine_core::environment_variable`, not `getenv`.
- **Building.** Build from Git Bash with `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox --parallel`. Run tests with `build/Debug/sandbox.exe "[tag]"`. Build only the targets a step needs. A Windows fast-fail shows as exit 127 in Bash; re-run from PowerShell for the real code.
- **Stale objects.** MSBuild rebuilds by timestamp. After restoring a file from a copy, `touch` it.
- **Allocation.** A warm play step must not allocate (engine_core README). Every per-slot array is reserved for `DataModel::kMaxInstances` (16384) when it is built.
- **References.** Read references with `InstanceRef::resolve` plus an alive and class check, never through a `LuaSlot`. A 16-character GUID copy allocates.
- **Hook writes.** Hooks never fire Changed or record history for the members they move. Only the written instance does, through its own setter. The editor follow is the one exception: it writes movers through `set_pv_transform`, which records history and fires Changed, by design.
- **Lone nodes.** A node in no multi-node assembly behaves exactly as today in every path. Existing physics, edit-physics, raycast, player-controller, brush, attachment, and move-tool tests must stay green unchanged.
- **Test ids.** Use `[assembly]` plus a sub-tag per task. IDs continue from today's last: physics `P40+`, edit physics `E18+`, move tool `MT19+`. New files use their own prefixes, all unused today: `CN` constraint, `AS` assemblies, `AE` assembly editor, `AP` assembly play, `CR` copy remap, `VZ` constraint visualization.
- **Test filters.** The real tags are `[physics]` (includes `[physics][edit]`), `[edit]`, `[raycast]`, `[player]`, `[brush]`, `[attachment]`, `[terrain][physics]`. Move-tool and instance-file tests are tagged per id, so filter them by name: `"MT*"`, `"IF*"`. Catch2 tags take no wildcards.
- **Commits.** One commit per task, in the repo's style: a plain sentence subject, sometimes with an `Area:` prefix (for example "Assemblies: welded bodies fall as one"). It ends with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Comments.** Match the surrounding code: prose comments above classes and non-obvious functions, in the same voice as `PhysicsWorld.hpp` and `Attachment.hpp`.

## Review Focus

- **A constraint end destroyed during play**, a member PhysicsObject in particular. The assembly splits cleanly. No Box3D shape is left on the old body, and nothing crashes on the next step or at Stop. Pinned in Task 8, `AP20`.
- **Undo of a deleted RigidConstraint, or of a deleted sub, in the editor.** The revived constraint is Active again and the sub sits on the parent, with no extra undo step and no spurious write. Pinned in Task 4, `AE16`.
- **A constraint whose two ends are one node:** a GameObject and its own PhysicsObject, or an Attachment and its holder. It is inactive, never snaps, and never loops. Pinned in Task 2, `AS12`.
- **A long rigid chain** (100 links) moved at its head. It follows in one pass with no recursion, and one undo reverts all of it. Pinned in Task 4, `AE17`.
- **Non-uniformly scaled GameObjects** as rigid parent and sub. Pivots ignore scale, and the sub keeps its own scale. Pinned in Task 4, `AE18`.

---

## File Structure

**New:**
- `src/engine_instances/Constraint.hpp`, `Constraint.cpp`: `Constraint`, `WeldConstraint`, `RigidConstraint`, and their Lua registration.
- `src/engine_core/Assemblies.hpp`: the `Assemblies` class, `AssemblySuspend`, and `AssemblyPivotChange`.
- `src/engine_core/Assemblies.cpp`:
  - the revision and the rebuild;
  - claims and node resolution;
  - the union-find, rigid rules, roots, `Active`, and `AssemblyRoot`;
  - per-slot flags;
  - suspend.
- `src/engine_core/AssemblyFollow.cpp`: the hook dispatch and the editor RigidConstraint follow (§4).
- `src/engine_core/AssemblyPlay.cpp`:
  - play capture, play members and offsets, and `pose_members`;
  - whole-assembly moves and pending velocity;
  - membership diffs, rigid activation in play, offset re-snap, and pivot changes (§5).
- `sandbox/constraint_tests.cpp`, `sandbox/assembly_tests.cpp`, `sandbox/assembly_edit_tests.cpp`, `sandbox/assembly_play_tests.cpp`, `sandbox/copy_remap_tests.cpp`, `sandbox/alloc_counter.hpp`, `sandbox/alloc_counter.cpp`.
- `resources/icons/WeldConstraint.png`, `resources/icons/RigidConstraint.png`.

**Modified:**
- **Core:**
  - `src/engine_core/DataModel.hpp`, `DataModel.cpp`, `DataModelState.hpp`: the constraint flag, tag, and query; the `assemblies()` accessor; revision bumps at tree changes; the `apply_transform` hook; `integrate_simulated` skipping members; `note()` dedupe.
  - `src/engine_core/Ecs.hpp`, `Ecs.cpp`: the `Constraint` tag.
  - `src/engine_core/DataModelPlace.cpp`: `capture_play` in `start_simulation`, and suspend in the restore.
  - `src/engine_services/ChangeHistoryService.cpp`: suspend while applying edit waypoints.
  - `src/engine_core/Project.cpp`: `class_registry`, and suspend in `Rebuild` and `apply_changes`.
  - `src/engine_core/Engine.cpp`: the stopped-tick follow pass, and a refresh in `step_physics`.
- **Physics:** `src/engine_core/PhysicsWorld.hpp`, `PhysicsWorld.cpp`, for compound bodies, write-back, `ray_hit`, terrain interest, test accessors, and PlayerController.
- **Instances:**
  - `src/engine_instances/PhysicsBase.cpp`: hooks and bumps.
  - `PhysicsObject.cpp`: the AngularVelocity hook.
  - `Attachment.cpp`: hooks.
  - `Brush.cpp`: `AssemblyPivotChange` in `apply`.
  - `GameObject.cpp`: the `AssemblyRoot` property.
- **Copying:**
  - `src/engine_core/InstanceFile.hpp`, `InstanceFile.cpp`: the GUID in `CopiedNode` and in files, the shared copy map, and the remap.
  - `src/ide/IdeLayoutEditing.cpp`: Duplicate, Paste, and held-cut use one action map.
  - `src/ide/IdeLayoutProject.cpp`: instance-file drops.
- **Scripting:** `src/engine_core/ScriptBindings.cpp` (factories), `LuaApi.cpp` (docs).
- **Studio:**
  - `src/ide/ClassOrder.hpp`, `InsertPopup.cpp`.
  - `PropertiesPanel.cpp`: re-read on the assembly revision.
  - `src/runner/GameView.hpp`, `GameView.cpp`: the visualization.
- **Plugins:** `resources/plugins/MoveTool.luau`.
- **Docs:** `resources/icons/ATTRIBUTION.md`, `src/engine_instances/README.md`.
- **Build:** `CMakeLists.txt` (sources and tests).
- **Tests:** `tests/LuauCompleteTest.cpp`, `tests/PropertiesTest.cpp`, `sandbox/move_tool_tests.cpp`.

---

### Task 1: Constraint classes

**Files:**
- Create: `src/engine_instances/Constraint.hpp`, `src/engine_instances/Constraint.cpp`, `sandbox/constraint_tests.cpp`
- Modify:
  - `src/engine_core/DataModel.hpp`: next to `wireframe()`, add `virtual bool constraint() const { return false; }`, and declare `void constraints(std::vector<InstanceId>& out) const;` next to `wireframes`.
  - `src/engine_core/Ecs.hpp` and `Ecs.cpp`: `struct ConstraintTag {};` and `EcsIds::constraint`, as `WireframeTag` is done.
  - `src/engine_core/DataModel.cpp`: `tag_entity` adds the tag when `object.constraint()`. Add `constraints(out)` over a new cached `constraint_query`: Instance, with ConstraintTag and InWorkspace.
  - `src/engine_core/DataModelState.hpp`: `flecs::query<> constraint_query;`, built where `body_query` is.
  - `src/engine_core/ScriptBindings.cpp`: `create_weld_constraint`/`create_rigid_constraint` factories, and `register_lua_creatable("WeldConstraint", ...)`/`("RigidConstraint", ...)`.
  - `src/engine_core/Project.cpp`: `class_registry` entries for both.
  - `CMakeLists.txt`: `Constraint.cpp` in `engine_instances`, `constraint_tests.cpp` in `sandbox`.

**Interfaces:**
- Produces:
```cpp
// Constraint.hpp
namespace engine_core {
// Joins two PVInstances into one assembly (Assemblies). Abstract: the Lua class
// Constraint cannot be made. PVInstance1 / PVInstance2 PVInstance?, saved by GUID,
// nil. Enabled boolean, saved, true. Active boolean, read-only, not saved (Task 2).
class Constraint : public DataModel {
public:
    Constraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    bool constraint() const override { return true; }
    virtual bool rigid() const = 0;                 // RigidConstraint: true
    LuaSlot pv_instance1() const;                   // instance_reference_slot(ref1_, "PVInstance")
    LuaSlot pv_instance2() const;
    // The live PVInstance an end names, or 0: ref.resolve + alive + lua_class_inherits.
    // Copies no string, so it allocates nothing.
    InstanceId pv_instance1_id() const;
    InstanceId pv_instance2_id() const;
    const InstanceRef& pv_instance1_ref() const { return ref1_; }
    const InstanceRef& pv_instance2_ref() const { return ref2_; }
    bool enabled() const { return enabled_; }
    // SimulationThread. Each returns why it refused, changing nothing.
    std::optional<std::string> set_pv_instance1(const LuaSlot& value);  // set_instance_reference("PVInstance1", "PVInstance", ...)
    std::optional<std::string> set_pv_instance2(const LuaSlot& value);
    std::optional<std::string> set_enabled(bool enabled);              // note_property_change("Enabled", ...)
protected:
    void on_reuse() override;   // both GUIDs cleared, Enabled true
private:
    InstanceRef ref1_;
    InstanceRef ref2_;
    bool enabled_ = true;
};
class WeldConstraint final : public Constraint { /* class_name "WeldConstraint", rigid() false */ };
class RigidConstraint final : public Constraint { /* class_name "RigidConstraint", rigid() true */ };
}
// DataModel
virtual bool constraint() const;                       // false
void constraints(std::vector<InstanceId>& out) const;  // live Constraints in Workspace; out is cleared first
```
- Lua registration (one `ANARCHY_LUA_REGISTER` block in `Constraint.cpp`):
  - `lua_group("Constraint")`;
  - `lua_saved_property("PVInstance1", "PVInstance?", ..., "null")`;
  - `lua_saved_property("PVInstance2", "PVInstance?", ..., "null")`;
  - `lua_saved_property("Enabled", "boolean", ..., "true")`;
  - `register_lua_class("Constraint", "Instance", fields, n)`;
  - `register_lua_class("WeldConstraint", "Constraint", nullptr, 0)` and the same for `RigidConstraint`;
  - `register_suited_parents` for both: `{"Workspace", "PVInstance"}`.

- [ ] **Step 1: Write the failing tests** in `sandbox/constraint_tests.cpp`, tag `[assembly][constraint]`. Model it on `sandbox/wireframe_tests.cpp`, using `engine_core::Game` and `workspace_of`.
  - `CN1`: a new WeldConstraint has PVInstance1 and PVInstance2 nil and Enabled true. `IsA("Constraint")` and `IsA("Instance")` are true, and `rigid()` is false. For RigidConstraint, `rigid()` is true.
  - `CN2`: PVInstance1 takes a GameObject, an Attachment, and a PhysicsObject (`pv_instance1_id()` returns each). It refuses a Folder with `"PVInstance1 must be a PVInstance"` and keeps the previous value. Nil clears it.
  - `CN3`: setting PVInstance2 to the same instance as PVInstance1 is allowed.
  - `CN4`: from Lua through `ScriptRig`, `local w = Instance.new("WeldConstraint", workspace); w.PVInstance1 = a; w.Enabled = false` reads back. `Instance.new("Constraint")` errors.
  - `CN5`: `constraints(out)` lists only constraints in Workspace. One under Storage, and one destroyed, are not listed. One moved into Workspace appears.
  - `CN6`: an undo of a PVInstance2 write puts back the old reference (as `history_tests.cpp` drives undo).
  - `CN7`: a project save and load round-trips both classes, both ends by GUID, and Enabled. Use the `project_tests.cpp` save/load helpers.
  - `CN8`: a destroyed target makes the end read nil and keeps its GUID. An undo of the destroy heals it.
- [ ] **Step 2: Run** `build/Debug/sandbox.exe "[constraint]"`. Expected: it fails to compile (`Constraint.hpp` is missing).
- [ ] **Step 3: Implement** the files above. Model the setters, slots, and `on_reuse` on `WireframeAdornment.cpp:49-65`.
  - `pv_instance1_id()`: `const InstanceId t = ref1_.resolve(*this); const DataModel* o = t ? instance(t) : nullptr; return o && lua_class_inherits(o->class_name(), "PVInstance") ? t : 0;`
- [ ] **Step 4: Run** `build/Debug/sandbox.exe "[constraint]"`, then `"[SS13]"`, which checks that a fresh instance of every class saves nothing. Expected: pass.
- [ ] **Step 5: Commit** "Constraints: WeldConstraint and RigidConstraint, two PVInstance ends and Enabled".

---

### Task 2: The assembly graph

**Files:**
- Create: `src/engine_core/Assemblies.hpp`, `src/engine_core/Assemblies.cpp`, `sandbox/assembly_tests.cpp`
- Modify:
  - `src/engine_core/DataModelState.hpp`: an `Assemblies assemblies;` member in `DataModel::State`.
  - `src/engine_core/DataModel.hpp`: `Assemblies& assemblies() const;` (state-owned and mutable, as other caches are), and `std::string_view guid_view(InstanceId id) const;` (a view of the slot's GUID, copying nothing) for the root comparator.
  - `src/engine_instances/PhysicsBase.cpp`: `game_object_id()` resolves through `game_object_ref_.resolve(*this)` plus an alive and GameObject check, with no `LuaSlot`, so `driven_game_object()` allocates nothing.
  - `src/engine_core/PhysicsWorld.cpp`: `first_in_tree_order` moves into `Assemblies.cpp` as the shared `claim_winner`. Reconcile calls it; its behavior does not change.
  - `src/engine_instances/Constraint.cpp`: register `Active` as `lua_property("Active", "boolean", false, read_active, nullptr)`, which calls `assemblies().refresh(*this)` and then `active(id())`.
  - `src/engine_instances/GameObject.cpp`: PVInstance's fields gain `lua_property("AssemblyRoot", "PVInstance?", false, read_assembly_root, nullptr)`.
  - `CMakeLists.txt`: `Assemblies.cpp` in `engine_core`, `assembly_tests.cpp` in `sandbox`.

**Interfaces:**
- Consumes: `Constraint` (Task 1), `DataModel::constraints`, `DataModel::physics_bodies`, `PhysicsBase::driven_game_object/anchored/mass`, and `Attachment::offset/offset_space`.
- Produces:
```cpp
namespace engine_core {
class DataModel;

// The PhysicsBase that drives game_object among candidates (each of which drives it): the
// first in depth-first Workspace order. Shared by Assemblies and PhysicsWorld::reconcile.
InstanceId claim_winner(const DataModel& game, const InstanceId* candidates, std::size_t count);

class Assemblies {
public:
    static constexpr std::uint8_t kLinked = 1u << 0;  // §3 "linked"
    static constexpr std::uint8_t kMember = 1u << 1;  // in an assembly of more than one node
    static constexpr std::uint8_t kRigid  = 1u << 2;  // a node or Attachment on either side of an active RigidConstraint

    Assemblies();                                     // reserves every per-slot array for kMaxInstances

    std::uint64_t revision() const;                   // atomic, any thread
    void bump();                                      // moves the revision
    // Rebuilds when revision(), game.world_generation(), or game.simulation_running() differs from the
    // last build (rigid rule 2 is play-only). Read-only otherwise: it never writes an instance. Under the lock.
    void refresh(const DataModel& game);

    // Valid after refresh(). 0 / false / -1 for an id the last build did not see.
    std::uint8_t flags(InstanceId id) const;
    InstanceId node_of(InstanceId id) const;          // a physics node is named by its PhysicsBase
    bool active(InstanceId constraint) const;
    InstanceId root_of(const DataModel& game, InstanceId id) const;  // AssemblyRoot (§2); computes an unlinked slot live
    InstanceId claimant_of(InstanceId game_object) const;

    // §2 "A node's frame" (unscaled), "mover", and an end's pivot. end is 1 or 2.
    Matrix4 node_frame(const DataModel& game, InstanceId node) const;
    InstanceId mover_of(const DataModel& game, InstanceId node) const;
    bool end_pivot(const DataModel& game, InstanceId constraint, int end, Matrix4& pivot) const;

    struct Group { InstanceId root = 0; bool anchored = false; bool has_physics = false;
                   std::uint32_t first = 0; std::uint32_t count = 0; };  // into group_nodes()
    const std::vector<Group>& groups() const;         // assemblies of more than one node, in root GUID order
    const std::vector<InstanceId>& group_nodes() const;
    int group_of(InstanceId node) const;              // index into groups(), or -1

    struct RigidEdge { InstanceId constraint = 0; InstanceId parent_node = 0; InstanceId sub_node = 0; };
    const std::vector<RigidEdge>& rigid_edges() const;  // active ones, in follow order (§2 rigid rule 1)
    // Task 3 adds: suspend/resume, hooks, and follow's declaration. Task 4 fills follow. Tasks 6-8 add: play.
};
}
```
- The rebuild (§2, §3):
  - It walks `constraints()` and `physics_bodies()` into reserved scratch, computes claims with `claim_winner` where more than one body drives a GameObject, and resolves ends (Attachment walk, Folder/Workspace/Terrain holder means unresolved).
  - It runs a union-find over slot indices. The rigid forest comes first (GUID order). In play, welds are joined first, then rigid edges in follow order (§2 rigid rule 2).
  - It picks each root with the comparator: anchored, then PlayerController, then physics, then higher Mass, then lower GUID, comparing `guid_view` bytes (the PhysicsBase's for a physics node).
  - It writes the per-slot arrays: node, group, flags, active, and claimant. It sets `kLinked` on:
    - ends;
    - Attachments on end walks;
    - holders;
    - members of multi-node groups;
    - contested GameObjects.
  - Use `std::vector`s sized once in the constructor; the rebuild only overwrites them. An unresolved GUID end goes in a reserved list, re-resolved only when Task 3's events ask.

- [ ] **Step 1: Write the failing tests** in `sandbox/assembly_tests.cpp`, tag `[assembly][graph]`. Use a `PhysicsRig`-like local helper: parts are a GameObject with a child PhysicsObject (`P15` style), plus `weld(a, b)` and `rigid(a, b)` helpers that create, set, and parent a constraint. Call `game.assemblies().refresh(game)` before each read.
  - `AS1`: two welded parts are one group. Each part's GameObject and PhysicsObject give the same `node_of`, and `root_of` on any of the four is one PhysicsObject.
  - `AS2`: anchored beats a heavier unanchored part.
  - `AS3`: a PlayerController beats a heavier PhysicsObject.
  - `AS4`: physics beats a plain GameObject.
  - `AS5`: higher Mass wins.
  - AS2–AS5 each run four ways (creation order and constraint direction swapped), with the same root every time.
  - `AS6`: equal Mass, and the lower PhysicsBase GUID wins. Set GUIDs with `set_guid` to `"a…"`/`"b…"`, then repeat with the creation order and weld direction swapped. The root is the same all four ways.
  - `AS7`: an Attachment under part B, welded to part A, joins B's node. `root_of(attachment)` is the group's root, never the Attachment.
  - `AS8`: an Attachment under a Folder, and one directly in Workspace, as an end: Active is false, and the Attachment's `AssemblyRoot` reads nil.
  - `AS9`: Active is false with an end missing, Enabled off, the constraint in Storage, or an end in Storage.
  - `AS10`: a Terrain end is inactive.
  - `AS11`: rigid rules.
    - A second RigidConstraint onto the same sub is inactive (the higher GUID loses).
    - A rigid loop A→B, B→A deactivates the later GUID.
    - `rigid_edges()` lists parent edges before sub edges.
  - `AS12` (Review Focus): a constraint between a GameObject and its own PhysicsObject, and one between an Attachment and its holder, are inactive and make no group.
  - `AS13`: a losing PhysicsBase (a second body naming the same GameObject) belongs to the winner's node.
  - `AS14`: `AssemblyRoot` from Lua (`part.AssemblyRoot`) on a lone part reads its PhysicsObject. A plain GameObject reads itself, and anything outside Workspace reads nil. Then `play()`: every read, for a lone part and for a welded pair, gives the same value as before Play.
  - `AS15`: in play (`game.start_simulation()` after `capture_place`), a rigid edge whose ends a weld already joins is inactive. In edit mode it is active.
- [ ] **Step 2: Run** `build/Debug/sandbox.exe "[graph]"`. Expected: it fails to compile.
- [ ] **Step 3: Implement** `Assemblies.hpp/.cpp`, `claim_winner`, the `Active`/`AssemblyRoot` properties, and the `PhysicsWorld` switch to `claim_winner`. In this task, `refresh` rebuilds whenever it is called (`revision()` is wired in Task 3).
- [ ] **Step 4: Run** `"[graph]"`, `"[constraint]"`, `"[physics]"` (includes `[physics][edit]`), and `"[raycast]"`. Expected: all pass.
- [ ] **Step 5: Commit** "Assemblies: constraints group PVInstances, with a root chosen by fixed rules".

---

### Task 3: Revision, slot flags, suspend, and the hook skeleton

**Files:**
- Modify:
  - `src/engine_core/Assemblies.hpp/.cpp`: the revision is real, and suspend and hooks are added.
  - `src/engine_core/DataModel.cpp`: bump sites in `link_child`, `unlink_parent`, `destroy`, and `apply_scope`; the `apply_transform` hook; `set_guid` re-resolve.
  - `src/engine_core/DataModelHistory.cpp`: `apply_record_fields` (undo/redo revival writes `guid_` directly) calls `guids_may_have_returned()`.
  - `src/engine_instances/Constraint.cpp`: setters call `constraint_changed`.
  - `src/engine_instances/PhysicsBase.cpp`: `set_transform` calls `transform_written`. The `set_anchored`, `set_mass`, and `set_game_object` setters bump when linked.
  - `src/engine_instances/Attachment.cpp`: `set_offset`, `set_offset_space`, and `set_transform` call `attachment_changed`.
  - `src/engine_services/ChangeHistoryService.cpp`: `apply_waypoint` holds an `AssemblySuspend` while the game is not simulating.
  - `src/engine_core/Project.cpp`: `Rebuild` holds one, and so does `apply_changes`.
  - `src/engine_core/DataModelPlace.cpp`: `restore_place_unlocked` holds one, and calls `guids_may_have_returned()`.
  - `src/ide/PropertiesPanel.cpp`: re-read when `assemblies().revision()` moves.
  - `sandbox/assembly_tests.cpp`: new tests.

**Interfaces:**
- Consumes: Task 2's `Assemblies`.
- Produces:
```cpp
class Assemblies {
public:
    // §3 suspend. While suspended() every hook returns at once. resume() on the outermost
    // close runs one refresh + follow(game, 0) when follow_on_resume was asked (paste), else nothing.
    void suspend();
    void resume(DataModel& game);
    bool suspended() const;
    void follow_on_resume();                      // the scope's owner asks for a pass at close

    // Hooks. Each returns at once while suspended or re-entered (an RAII guard), or when the slot
    // is not kLinked after refresh. Stopped: Task 4 fills them. Playing: Tasks 7-8 fill them.
    void transform_written(DataModel& game, InstanceId id);    // after a checked Transform write
    void attachment_changed(DataModel& game, InstanceId id);   // Offset / OffsetSpace / Transform
    void constraint_changed(DataModel& game, InstanceId id);   // bump + (stopped) refresh + follow pass
    void tree_changed(DataModel& game, InstanceId id);         // a linked slot or a Constraint linked/unlinked/destroyed
    void guids_may_have_returned();                            // undo/redo, Stop restore, paste, set_guid; also bumps

    // Stopped only; Task 4 fills it (returns 0 until then). start == 0: every edge.
    int follow(DataModel& game, InstanceId start);
};
class AssemblySuspend {   // RAII
public:
    explicit AssemblySuspend(DataModel& game);
    ~AssemblySuspend();
};
```
- Bump rules, exactly §3 "What moves the revision". In `link_child`/`unlink_parent`/`destroy`:
  - bump when the slot is `kLinked`, or is a Constraint;
  - bump when it is a PhysicsBase whose driven GameObject (before or after) is `kLinked`.

  `apply_scope` bumps when a Constraint or a `kLinked` slot enters or leaves Workspace. Renames never bump. While `suspended()`, bumps still move the revision; only hooks are silenced. `refresh` also rebuilds when `simulation_running()` differs from the last build, so Play and Stop apply rigid rule 2 without an edit.

- [ ] **Step 1: Write the failing tests**, tag `[assembly][revision]`:
  - `AS16`: renaming a welded part, and creating, parenting, and moving an unrelated PhysicsObject under a fresh GameObject in Workspace, leave `revision()` unchanged.
  - `AS17`: each of these moves it:
    - a constraint's PVInstance1;
    - its Enabled;
    - reparenting the constraint;
    - destroying a welded part;
    - setting Anchored or Mass on a welded part's PhysicsObject;
    - parenting a new PhysicsObject under a welded GameObject.
  - `AS18`: inside an `AssemblySuspend`, a constraint edit still moves the revision, and `active()` is right after a `refresh`.
  - `AS19`: `set_guid` of an instance that an unresolved end names makes the end resolve after the next refresh, as project load does. The rebuild does not call `find_guid` per end (a test hook, `Assemblies::find_guid_calls()`, counts the slot scans: 200 dangling ends, then 1000 iterations of `bump(); refresh(game);`, make 0 scans).
  - `AS20`: weld A–B plus rigid A→B. `active(rigid)` is true; after `play()`, with no other edit, it is false; after Stop it is true again.
  - `AS21`: delete sub B of an active RigidConstraint, then undo the delete: the constraint is Active again after the next refresh (revival re-resolves the GUID).
- [ ] **Step 2: Run** `"[revision]"`. Expected: it fails.
- [ ] **Step 3: Implement.** The `apply_transform` call goes after `emit_change`:
  `state_->assemblies.transform_written(*this, id);`.
  The hook bodies only refresh and early-return in this task. Their stopped branch calls `follow(game, id)` (declared public above, returning 0 until Task 4). Their playing branch calls private `play_transform_written(game, id)` and `play_attachment_changed(game, id)`, declared now and doing nothing until Tasks 7 and 8, each with a comment naming that task.
- [ ] **Step 4: Run** `"[assembly]"`, `"[physics]"`, `"[attachment]"`, `"[history]"`, `"[project]"`, and `"MT*"`. Expected: pass.
- [ ] **Step 5: Commit** "Assemblies: own revision, slot flags, and suspend scopes for load, undo, and Stop".

---

### Task 4: The editor RigidConstraint follow

**Files:**
- Create: `src/engine_core/AssemblyFollow.cpp`, `sandbox/assembly_edit_tests.cpp`
- Modify:
  - `src/engine_core/Assemblies.hpp`: fill in `follow` (declared in Task 3), and add private helpers.
  - `src/engine_core/Engine.cpp`: the stopped tick calls `game_.assemblies().follow(game_, 0)` right after `physics_.sync(game_)`, under the same lock.
  - `CMakeLists.txt`.

**Interfaces:**
- Consumes: `node_frame`, `mover_of`, `end_pivot`, `rigid_edges`, the hooks (Task 3), and `PVInstance::set_pv_transform`.
- Produces:
```cpp
// Stopped only. start == 0: every active rigid edge in follow order (the stopped-tick pass and the
// resume pass). Else: start's own incoming edge (when it is a sub) then every edge downstream of it.
// Writes made here start no pass (re-entry guard). Returns how many movers it wrote.
int Assemblies::follow(DataModel& game, InstanceId start);
```
- The math, per §4:
  - Compute `L = node_frame(sub)⁻¹ × F_sub` and `target = F_parent × L⁻¹`. For a World-space Attachment sub, match translation only.
  - Make a PlayerController's target upright: yaw only, as `PlayerController` keeps it.
  - Compare the mover's current frame (from `node_frame`) to the target. Write only past 1e-5 units, or 1e-5 in any rotation-matrix element.
  - The write is `mover.set_pv_transform(target with the mover's own column scales)`.
  - Then move the mover's world-Transform descendants by the same delta: GameObjects, Cameras, and Lights; not Attachments, Bones, or driven PhysicsBases. Use DataModel's reserved walk scratch.
  - The pass keeps an explicit work list (no recursion). A carried descendant that is a `kRigid` sub goes on it, so its own incoming edge, then its downstream edges, are handled in the same pass.
  - Skip `workspace.CurrentCamera` as a mover and as a parent end.
  - While stopped, a `transform_written` for a driven PhysicsBase does nothing (E12).

- [ ] **Step 1: Write the failing tests** in `sandbox/assembly_edit_tests.cpp`, tag `[assembly][follow]`. Use `PhysicsRig` (stopped, with `rig.sync_steps(1)` where a body must follow) and `ChangeHistoryService` recordings, as `history_tests.cpp` uses them.
  - `AE1`: connecting a RigidConstraint (PVInstance1 = A's GameObject, PVInstance2 = B's GameObject) inside a recording snaps B onto A. One undo puts B back, and the constraint loses PVInstance2.
  - `AE2`: inside one recording, moving A moves B. One undo reverts both. Repeat with the ends picked as A's and B's PhysicsObjects.
  - `AE3`: writing B's Transform alone snaps it straight back in the same call, so B.Transform reads the snapped pose right after the write.
  - `AE4`: an Attachment end. A RigidConstraint from A's Attachment `At` (Offset translate (0,2,0)) to B puts B's frame at A × At.Offset. Writing At's Offset moves B in the same call.
  - `AE5`: a World-space Attachment sub, with A turned 90° about Y. B's position matches, and B keeps its rotation.
  - `AE6`: a PlayerController sub under a parent pivot tilted 30° about X settles in one write. A second `follow(game, 0)` writes 0.
  - `AE7`: the chain A→B→C, where C is held by B's Attachment. Moving A moves B and C in one call.
  - `AE8`: a sub's child GameObject (not welded) moves with it by the same delta. Its child Attachment is not written.
  - `AE9`: a WeldConstraint moves nothing while stopped.
  - `AE10`: a CurrentCamera that is a RigidConstraint sub can be written freely, and nothing else moves.
  - `AE11`: a write to a driven PhysicsObject's own Transform starts no follow, and after `sync_steps(1)` nothing has moved (E12 holds).
  - `AE12`: in a consistent place, the stopped pass (`follow(game, 0)`) writes 0 and records nothing.
  - `AE13`: a suspended scope (`AssemblySuspend`) around a parent move snaps nothing. The pass at the next `follow(game, 0)` snaps the sub.
  - `AE14`: Project round-trip. Save a snapped pair, load it, run `follow(game, 0)`: it writes 0.
  - `AE15`: undo and redo of a parent move write the sub back and forth with no extra history steps: after `reset_waypoints`, `begin_step`/`end_step` (support.hpp) wrap the parent move; a local `undo_all` copy (as in history_tests.cpp) undoes exactly 1 step, which reverts A and B; redo then undo leaves `can_undo()` and `can_redo()` as before.
  - `AE16` (Review Focus): delete the RigidConstraint, move A, and undo the delete. The undo itself makes no follow. The next stopped pass snaps B, and `Active` is true. Then delete the sub B and undo: B comes back on A, `active(rigid)` is true, and a later move of A moves B in the same call.
  - `AE17` (Review Focus): a 100-link chain moved at its head follows in one call, undoes in one step, and makes no recursion; the follow is iterative.
  - `AE18` (Review Focus): parent A scaled (2, 1, 3) and sub B scaled (0.5, 0.5, 0.5). After the snap, B's column lengths are unchanged and its unscaled pose equals A's.
  - `AE20`: B's child GameObject D is the sub of a RigidConstraint from C. Moving B carries D, and D snaps back onto C in the same call.
  - `AE19`: a Brush that is a rigid sub, edited with `MoveFace` (its recentre moves its Transform), is snapped again in the same call. Its pivot stays on the parent's, and its geometry shifts by the recentre, as §4 Exceptions say.
- [ ] **Step 2: Run** `"[follow]"`. Expected: it fails.
- [ ] **Step 3: Implement** `AssemblyFollow.cpp` and the Engine stopped-tick call.
- [ ] **Step 4: Run** `"[assembly]"`, `"[edit]"`, `"[attachment]"`, `"[history]"`, and `"MT*"`. Expected: pass.
- [ ] **Step 5: Commit** "RigidConstraint: the sub follows its parent's pivot live in the editor, in the same undo step".

---

### Task 5: MoveTool carries Attachments and re-places its handles

**Files:**
- Modify: `resources/plugins/MoveTool.luau`, `sandbox/move_tool_tests.cpp`

**Interfaces:**
- Consumes: Task 4's follow (a sub snapped back).
- Produces:
  - In `Dragged`: skip an item that `IsA("Attachment")` (a Bone is one) whose `Parent` is in the moved list. Build a set from `starts` at `DragBegan`.
  - In `DragEnded`: call `place()` after `starts = {}`. `place` is a local defined after `make_dragger`, so forward-declare `local place` above `make_dragger` and define it later as `place = function() ... end`; otherwise the handler calls a nil global.
  - Update the comment above `Dragged`: it now says that Attachments of a moved parent are never written, rather than being skipped by FuzzyEq.

- [ ] **Step 1: Write the failing tests** in `sandbox/move_tool_tests.cpp`, using the existing MT harness:
  - `MT19`: a RigidConstraint sub part B, held by its own Attachment `Ab`, with its parent pivot on A. Drag B by (3, 0, 0). After DragEnded, B's Transform and Ab's Offset are unchanged.
  - `MT20`: after that drag, the Dragger handles sit back at B's position (`dragger.Transform.Position` equals B's).
  - `MT21`: a normal drag of a part with an Attachment child still moves the part, and the Attachment's Offset is unchanged (no regression to MT17).
- [ ] **Step 2: Run** `"MT*"`. Expected: MT19 and MT20 fail.
- [ ] **Step 3: Implement** the two Luau changes.
- [ ] **Step 4: Run** `"MT*"` and `"[follow]"`. Expected: pass.
- [ ] **Step 5: Commit** "Move: a moved part's Attachments are never written, and the handles come back after a snap".

---

### Task 6: Compound bodies in play

**Files:**
- Create: `src/engine_core/AssemblyPlay.cpp`, `sandbox/assembly_play_tests.cpp`
- Modify:
  - `src/engine_core/Assemblies.hpp`: the play state.
  - `src/engine_core/DataModelPlace.cpp`: `start_simulation` calls `state_->assemblies.capture_play(*this)` after `simulation_running` is set and before `on_start`.
  - `src/engine_core/PhysicsWorld.cpp/.hpp`: compound records, shape placement, write-back, `ray_hit`, terrain interest, and accessors.
  - `src/engine_core/DataModel.cpp`: `integrate_simulated` skips `kMember` slots.
  - `src/engine_core/Engine.cpp`: `step_physics` calls `game_.assemblies().sync_play(game_)` before `integrate_simulated`.
  - `src/runner/GameView.cpp`: while playing, a selected Custom's outline picks mesh or pieces from its play assembly's `anchored` (`play_of`/`play()`), not `body->anchored()`.
  - `sandbox/physics_rig.hpp`: an `engine_steps(int n)` helper that mirrors `Engine::step_physics`: `game.assemblies().sync_play(game); game.integrate_simulated(kStep); physics.step(game, kStep);`.
  - `CMakeLists.txt`.

**Interfaces:**
- Consumes: `groups()`, `group_nodes()`, `node_frame`, `flags`, and `claimant_of`.
- Produces:
```cpp
class Assemblies {
public:
    struct PlayMember { InstanceId node = 0; Matrix4 offset{}; };  // offset = frame⁻¹ × node frame
    struct PlayAssembly {
        InstanceId root = 0;
        bool anchored = false;
        bool has_physics = false;
        Matrix4 frame{};                 // body frame when offsets were captured; the teleport target
        std::uint32_t first = 0, count = 0;   // into play_members()
        std::uint32_t stamp = 0;         // moves when members or offsets change: PhysicsWorld re-places shapes
        bool teleport = false;           // Task 7: a member was written; PhysicsWorld moves the body to frame
        bool velocity_pending = false;   // Task 7
        Vec3 velocity{};
        bool angular_pending = false;
        Vec3 angular{};
    };
    void capture_play(DataModel& game);                       // start_simulation, before scripts; refreshes first
    // Non-const: refresh, then (from Task 8) diff play() when the build moved. SimulationThread only.
    // PhysicsWorld::sync calls it first, so steps, raycasts, and test rigs all see current play state.
    // Until Task 8 it is refresh plus nothing. Readers (Active, AssemblyRoot, GameView) call refresh only.
    void sync_play(DataModel& game);
    const std::vector<PlayAssembly>& play() const;
    std::vector<PlayAssembly>& play();
    const std::vector<PlayMember>& play_members() const;
    int play_of(InstanceId node) const;                       // index into play(), or -1
    // Writes every member of play()[a] from body_pose × offset with the write-back stores
    // (§5 Write-back): no Changed, no history. skip: a member already written by its own setter.
    void pose_members(DataModel& game, std::size_t a, const Matrix4& body_pose,
                      Vec3 velocity, Vec3 angular, InstanceId skip = 0);
};
// PhysicsWorld test accessors take any member's PhysicsBase id (§5 Test accessors).
```
- PhysicsWorld:
  - **Records.** A second map, `compounds`, holds one `Compound` record per play assembly with physics. It has the `b3BodyId`, the stamp it was built at, and per member: the shape span, a cached member-space hull (`b3HullData` from its first build), and volume. A reserved per-slot array `body_of[slot]` gives the compound index, or -1.
  - **Sync.** `PhysicsWorld::sync` begins with `game.assemblies().sync_play(game)`.
  - **Reconcile.** It skips PhysicsBases that are compound members (their singles are destroyed at the first play sync). It then makes or keeps compounds whose `stamp` moved.
  - **Shapes.** Placement goes through `b3CreateTransformedHullShape`, `b3MakeTransformedBoxHull`, and transformed sphere and capsule centers. An anchored Custom gets transformed mesh points.
  - **Mass and type.** Mass is per-member density, then one `b3Body_ApplyMassFromShapes`. Type is static when `anchored`. Velocity uses `b3Body_SetLinearVelocity` and `b3Body_SetAngularVelocity` after the mass is applied.
  - **Write-back.** `pull()` sends a compound's move event to `pose_members`.
  - **Raycasts.** `ray_hit` uses `b3Shape_GetUserData`.
  - **Terrain.** Interest, for dynamic compounds, is asked around each physics member.

- [ ] **Step 1: Write the failing tests** in `sandbox/assembly_play_tests.cpp`, tag `[assembly][play]`, with `PhysicsRig`:
  - `AP1`: two boxes welded at y=5 and y=8 (sizes 1, mass 50 each) over the floor. After `play()` and `steps(1)`:
    - `body_count()` is floor + 1;
    - `body_mass(either)` is 100;
    - `shape_count(either)` is 1;
    - `body_key` is the same for both.

    After `seconds(3)`, the low box rests at y≈0.5 (±0.05) and the gap stays 3 (±1e-3).
  - `AP2`: a plain GameObject, a Camera, and a PointLight, each welded to a falling box, keep their offsets after `seconds(1)` (±1e-3). Their Transforms changed, and no Changed fired. Build this one on `ScriptRig` plus a standalone `engine_core::PhysicsWorld`, stepped as P10 does, and count Changed with a Lua `Changed:Connect` counter (a watch counter would count the write-backs).
  - `AP3`: a box welded to an anchored box at y=7 stays at exactly y=7 after `seconds(1)`. `body_mass` is 0, since static.
  - `AP4`: a welded box and sphere where the sphere's GameObject is scaled 2 and turned 45°. The sphere's shape sits at its offset (a ray down through the sphere hits at its top). Its written-back Transform keeps scale 2.
  - `AP5`: a ray that hits the non-root member reports that member's PhysicsObject in `RayHit.instance`.
  - `AP6`: an unanchored Custom welded to an anchored box gets mesh shapes. The same pair unanchored gets pieces. Use `MeshShapes`, as P11 or the decomposition tests do. `PhysicsWorld::collision_outline` called with the assembly's anchoring gives mesh lines for the unanchored Custom welded to the anchored box.
  - `AP7`: Stop restores every member's authored Transform, and a second play simulates again from them (P9 style).
  - `AP8`: lone bodies are untouched. The existing P1–P39 pass unchanged; run them in Step 4.
  - `AP9`: a welded Brush (unanchored) and box fall as one, and the Brush's AngularVelocity is written back.
  - `AP9b`: the compound takes the root's LinearDamping/AngularDamping; a damping write on the non-root member leaves the body's damping as it was (read back through a test accessor `body_damping(id)` added here).
  - `AP9c`: a rider GameObject with `Simulated` true and a Velocity does not drift from its offset over `engine_steps(240)`, because `integrate_simulated` skips members. A lone Simulated GameObject with the same Velocity is the control, and does drift.
  - `AP9d`: two boxes welded 40 units apart over a Terrain (terrain test helpers, as the terrain physics tests build one): after `seconds(2)` neither has fallen through (interest is asked around each member, not only the body's origin).
- [ ] **Step 2: Run** `"[play]"`. Expected: it fails.
- [ ] **Step 3: Implement.** Keep the compound code in its own block of `PhysicsWorld.cpp` (a `// Compound assemblies` section) so the single-body code reads as before.
- [ ] **Step 4: Run** `"[assembly]"`, `"[physics]"`, `"[raycast]"`, `"[player]"`, `"[brush]"`, and `"[terrain][physics]"`. Expected: pass.
- [ ] **Step 5: Commit** "Assemblies: welded bodies are one Box3D body in play, and riders go along".

---

### Task 7: Moving an assembly, velocity, and PlayerController in play

**Files:**
- Modify:
  - `src/engine_core/AssemblyPlay.cpp`: `play_transform_written` and `play_velocity_written`.
  - `src/engine_core/Assemblies.hpp`: `velocity_written`.
  - `src/engine_instances/PhysicsBase.cpp`: `set_velocity` calls the hook.
  - `src/engine_instances/PhysicsObject.cpp`: the AngularVelocity setter calls it too.
  - `src/engine_instances/Brush.cpp`: `set_angular_velocity` calls it too.
  - `src/engine_core/PhysicsWorld.cpp`:
    - apply `teleport` and pending velocities in sync;
    - `follow_driven` skips compound members;
    - PlayerController roots get controller setup;
    - `control()` filters contacts to the controller's own shape and uses `b3Body_GetMass`;
    - a non-root controller stores `OnGround`/`IsSliding` false;
    - applying a pending velocity to a compound whose root is a PlayerController sets the controller's `launched` when the pending y exceeds the body's by `kRisingSpeed`, whichever member was written (as `push` does for a lone controller);
    - a test accessor `body_center(id)` (`b3Body_GetWorldCenter`).
  - `sandbox/assembly_play_tests.cpp`.

**Interfaces:**
- Consumes: `play()`, `pose_members`, and the `transform_written` hook (Task 3).
- Produces:
```cpp
void Assemblies::velocity_written(DataModel& game, InstanceId id, Vec3 linear, bool has_angular, Vec3 angular);
// private, AssemblyPlay.cpp:
void Assemblies::play_transform_written(DataModel& game, InstanceId id);
//   frame = W × offset⁻¹, where W is the written instance's own new Transform, unscaled (for a driven or
//   claim-losing PhysicsBase too, not its GameObject's frame), made upright if the root is a
//   PlayerController; pose_members(skip = id unless the root is a PlayerController);
//   play()[a].frame = frame; teleport = true. An assembly with no physics only poses its members.
```

- [ ] **Step 1: Write the failing tests**, tag `[assembly][play-move]`:
  - `AP10`: writing the non-root box's GameObject Transform from Lua mid-fall moves both at once. In the same script, reading the root's Transform shows the moved pose. Only the written box fires Changed. Built on `ScriptRig` plus a standalone PhysicsWorld, with a Lua Changed counter, as P10 is.
  - `AP10b`: writing the non-root member's PhysicsObject.Transform (not its GameObject) mid-fall, and again before the first step, moves both members and both GameObjects.
  - `AP10c`: two plain GameObjects welded (no PhysicsBase), in play: writing one's Transform moves the other at once, silently, and `body_count()` is unchanged.
  - `AP11`: the same write made before the first step (right after `play()`, no `steps`) moves both. The compound is made at the new pose.
  - `AP12`: writing Velocity (0, 10, 0) on the non-root member makes the body rise. Reading the root's Velocity right after gives (0, 10, 0). With two writes in one frame on different members, the last wins. Writing AngularVelocity on a welded Brush member spins the body.
  - `AP13`: center-of-mass velocity. A pair with a 2-unit center offset is given AngularVelocity (0, 5, 0) and Velocity 0 at play. After `steps(1)`, `body_center`'s horizontal displacement is ~0 and the written root Velocity is ~0 horizontally (`body_position`, the origin, does move: that is expected).
  - `AP14`: a PlayerController welded to a mass-500 crate is the root, hovers at its gap over the floor after `seconds(1)`, and is upright. Writing the crate's Transform tilted 30° leaves the controller upright, and the crate is re-placed from the upright frame.
  - `AP15`: the crate's contact with a low ceiling box does not zero the controller's walk velocity (the `stop_at_walls` filter).
  - `AP16`: two PlayerControllers welded together: the non-root one reads `OnGround` false.
  - `AP16c`: a PlayerController welded to a crate on the floor: writing Velocity (0, 20, 0) on the crate lifts the player off its hover gap within `seconds(0.2)` (it is launched, not held down).
- [ ] **Step 2: Run** `"[play-move]"`. Expected: it fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `"[assembly]"`, `"[player]"`, and `"[physics]"`. Expected: pass.
- [ ] **Step 5: Commit** "Assemblies: a member moved or pushed in play moves the whole assembly, and players carry what is welded to them".

---

### Task 8: Membership changes in play, rigid activation, re-snap, recentre, and Stop

**Files:**
- Modify:
  - `src/engine_core/AssemblyPlay.cpp`:
    - `play_rebuilt`, called from `sync_play` (Task 6) after a refresh that rebuilt while playing. `refresh` stays read-only, so a UI or render-thread read of Active never runs the diff;
    - `play_attachment_changed`;
    - `AssemblyPivotChange`.
  - `src/engine_core/Assemblies.hpp`.
  - `src/engine_instances/Brush.cpp`: `apply` wraps its `set_transform` in an `AssemblyPivotChange`.
  - `src/engine_core/PhysicsWorld.cpp`:
    - incremental split and merge;
    - anchoring change remakes Custom members;
    - re-placing one side's shapes.
  - `sandbox/assembly_play_tests.cpp`.

**Interfaces:**
- Produces:
```cpp
class AssemblyPivotChange {   // RAII: a Transform write to id inside it is a pivot change, not a move
public:
    AssemblyPivotChange(DataModel& game, InstanceId id);
    ~AssemblyPivotChange();
};
// private: play_rebuilt(DataModel&): diff old play() against the new groups by member body (§5).
//   A group whose node set is unchanged keeps frame, offsets, stamp. A changed group: the piece holding
//   the old root keeps its frame and its members' offsets; new members get offsets from current frames;
//   a RigidConstraint newly active in play first moves the sub's old assembly rigidly onto the parent
//   pivot (pose_members), then joins. stamp moves; PhysicsWorld re-places only what changed.
```
- PhysicsWorld, on a moved stamp, matches the new assembly to the old compound that holds its root (or most of its members).
  - **Kept members.** They keep their shapes.
  - **Departing members.** Their shapes are destroyed.
  - **Arriving members.** Their shapes are created at their offsets from the cached member-space hulls.
  - **Mass.** It is applied once.
  - **Velocity.** A new body for a departing piece gets `v + ω × (c_new − c_old)` and ω. A merge keeps momentum, and ω is the mass-weighted mean.
  - **Anchoring flip.** It remakes Custom members before `b3Body_SetType`.

- [ ] **Step 1: Write the failing tests**, tag `[assembly][play-change]`:
  - `AP17`: a falling welded pair whose weld a script disables mid-fall becomes two bodies.
    - With no spin, both keep the same velocity.
    - Spinning (ω = (0, 0, 4)): each new body's velocity equals the old body's point velocity at its center (±1e-3).
  - `AP18`: a script enables a weld between two falling boxes: one body, with momentum kept (±1e-3).
  - `AP19`: un-anchoring the anchored member of an assembly with a Custom member makes the Custom collide as pieces: it lands on the floor instead of falling through.
  - `AP20` (Review Focus): destroying a member PhysicsObject mid-fall leaves one body with one shape for the survivor. Twenty more steps and then Stop complete with no contract failure.
  - `AP21`: a script connects a RigidConstraint in play. The sub's whole old assembly (the sub plus a box welded to it) moves rigidly so the pivots meet, then all three move as one.
  - `AP22`: a script slides the parent-end Attachment's Offset along X every step. Only the sub side's shape key changes (`shape_key(sub)` moves, `shape_key(parent)` does not), and a box resting on the parent stays resting, with velocity ~0.
  - `AP23`: `Brush:MoveFace` (or Clip) in play on a welded Brush does not move the other member.
  - `AP24`: Stop restores every member after a play that enabled a RigidConstraint and slid an end Offset.
  - `AP25`: a member leaving Workspace mid-play leaves the assembly. It and the rest each keep moving.
  - `AP16b`: a PlayerController welded to an anchored seat is not root (the seat is); un-anchoring the seat in play makes the controller root, the body is rebuilt at the controller's feet, and after `seconds(1)` it hovers at its gap.
  - `AP28`: a script destroys the welded sub of a RigidConstraint mid-play; after Stop, the constraint is Active again and the sub is back on its parent.
- [ ] **Step 2: Run** `"[play-change]"`. Expected: it fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `"[assembly]"`, `"[physics]"`, `"[brush]"`. Expected: pass.
- [ ] **Step 5: Commit** "Assemblies: welds made or broken in play split and merge bodies in place, keeping their motion".

---

### Task 9: One invalidation per slot, and a no-allocation step

**Files:**
- Create: `sandbox/alloc_counter.hpp`, `sandbox/alloc_counter.cpp`. These hold a global replacement of every `operator new`/`operator delete` form (plain, array, aligned, and nothrow; DataModel uses the aligned form) that counts while armed. Arming also installs `b3SetAllocator` with counting wrappers, since Box3D allocates through `b3Alloc`, not `new`; disarming restores Box3D's default. The functions are `alloc_counter::arm()`/`disarm()`/`count()`. Box3D's allocator hook needs a small `PhysicsWorld` test entry point, because only PhysicsWorld.cpp includes Box3D.
- Modify:
  - `src/engine_core/InvalidationQueue.hpp/.cpp`: a per-slot `queued` array (sized in `reserve()` to `kMaxInstances`, holding the queued InstanceId, so a reused slot is not suppressed). `push()` skips a Transform-only item whose slot already holds that id. `drain()`, `clear()`, and `take_overflow()` all reset it, so a Stop or a resync never leaves a slot suppressed.
  - `sandbox/dense_views_tests.cpp`: the one intended test change. "overflow resync keeps Workspace membership" overflows through a field that is not deduped: it alternates a Transform write with a Scale or Color write each iteration.
  - `src/engine_core/PhysicsWorld.cpp`: reconcile's per-sync `claims` map becomes a reserved per-slot array.
  - `sandbox/assembly_play_tests.cpp`, `CMakeLists.txt`.

**Interfaces:**
- Produces:
```cpp
namespace alloc_counter { void arm(); void disarm(); std::size_t count(); }
```

- [ ] **Step 1: Write the failing tests**, tag `[assembly][alloc]`:
  - `AP26`: a place with no Terrain holds 20 welded pairs plus 20 riders.
    - After `play()` and `steps(10)`, arm the counter, run `steps(240)`, and disarm: the count is 0.
    - Writing a member's Transform and Velocity from C++ between steps also stays 0.
  - `AP27`: clear the queue after setup, then 2100 riders on one body for 32 substeps with no drain (67200 notes without the dedupe): `!game.invalidations().overflow()`.
- [ ] **Step 2: Build Release `sandbox` and run** `build/Release/sandbox.exe "[alloc]"`. Expected: it fails. In Debug, `AP26` SKIPs when `_ITERATOR_DEBUG_LEVEL != 0`, because MSVC Debug containers allocate proxies.
- [ ] **Step 3: Implement.** Find each allocation the counter reports with a breakpoint on the counting `operator new`. Fix each one with reserved scratch. Do not exclude any assembly or physics code from the count.
- [ ] **Step 4: Run** `"[assembly]"` and `"[dense]"` in Release, then the whole Debug sandbox. Expected: pass.
- [ ] **Step 5: Commit** "Assemblies: a warm step allocates nothing, and a moved slot queues one redraw".

---

### Task 10: Copies keep their links

**Files:**
- Create: `sandbox/copy_remap_tests.cpp`
- Modify:
  - `src/engine_core/InstanceFile.hpp/.cpp`: `CopiedNode::guid`; `copy_tree` fills it; `node_json` writes and `read_node` reads an optional `"guid"`.
  - Also in `InstanceFile`: `paste_copies` takes an optional `CopyMap*`, and a new `remap_references` function.
  - `src/ide/IdeLayoutEditing.cpp`: `duplicate` uses one `CopyMap` for all its groups, then one `remap_references`. Paste and held-cut Paste use a fresh `CopyMap` per target, with `remap_references` after that target's `paste_copies`. One `AssemblySuspend` covers the whole action.
  - `src/ide/IdeLayoutProject.cpp`: an instance-file drop remaps.
  - `CMakeLists.txt`.

**Interfaces:**
- Consumes: `Constraint::rigid()`, `set_pv_instance2`, `AssemblySuspend`, and `Assemblies::follow_on_resume`.
- Produces:
```cpp
struct CopiedNode { /* ... */ std::string guid; /* the source's GUID; empty from an old file */ };
// Source GUID -> the copy's id, for every node one action built.
using CopyMap = std::unordered_map<std::string, InstanceId>;
bool paste_copies(DataModel& world, const std::vector<CopiedNode>& roots, InstanceId parent,
                  std::vector<InstanceId>* made = nullptr, std::string* refused = nullptr,
                  CopyMap* map = nullptr);
// Rewrites every saved X? reference property on the map's copies whose GUID is a key, through the
// property's own write (recorded). Clears PVInstance2 of a copied RigidConstraint whose PVInstance1
// was remapped and whose PVInstance2 was not. Runs on SimulationThread inside the action's recording.
void remap_references(DataModel& world, const CopyMap& map);
```

- [ ] **Step 1: Write the failing tests** in `sandbox/copy_remap_tests.cpp`, tag `[assembly][remap]`. Drive the real `copy_tree`/`paste_copies`/`remap_references` path. For Duplicate, also drive the studio path, either through `tests/` with the IdeLayout harness the Explorer tests use, or by mirroring `duplicate`'s per-group calls with one shared map:
  - `CR1`: two welded parts and their WeldConstraint, as siblings, duplicated as three groups with one map. The copied weld names the copies. After Undo then Redo, it still does.
  - `CR2`: a constraint copied alone still names the originals.
  - `CR3`: a part with a RigidConstraint child whose PVInstance1 is the part and whose PVInstance2 is outside. In the copy, PVInstance1 is the copied part and PVInstance2 is nil.
  - `CR4`: a PhysicsObject whose `GameObject` names a sibling GameObject, copied together: the copy names the copied GameObject.
  - `CR5`: an instance file round trip. `save_instance_file` of a welded pair, then `load_instance_file` and `paste_copies` with a map, then `remap_references`: the inserted weld names the inserted parts. A version-1 file with no `"guid"` keys still loads.
  - `CR7`: a welded pair and its weld, copied and pasted onto two targets: each target's weld names its own target's copies.
  - `CR6`: pasting a welded RigidConstraint pair holds an `AssemblySuspend`. No follow runs mid-paste, and one pass runs at the end inside the paste recording (undo removes the whole paste in one step).
- [ ] **Step 2: Run** `"[remap]"`. Expected: it fails.
- [ ] **Step 3: Implement.** In `IdeLayoutEditing.cpp`, inside each action's existing `ScopedRecording`:
  - **Duplicate:** `AssemblySuspend suspend(world); CopyMap map;`, its loop of `paste_copies(..., &map)` over the groups, then `remap_references(world, map); world.assemblies().follow_on_resume();`.
  - **Paste and held-cut Paste:** `AssemblySuspend suspend(world);`, then for each target `CopyMap map; paste_copies(..., &map); remap_references(world, map);`, then `world.assemblies().follow_on_resume();`.
- [ ] **Step 4: Run** `"[remap]"`, `"IF*"`, `"[assembly]"`, and the studio `explorer-tests`. Expected: pass.
- [ ] **Step 5: Commit** "Copy, Paste, Duplicate, and instance files remap references inside what they copy".

---

### Task 11: Studio wiring

**Files:**
- Create: `resources/icons/WeldConstraint.png`, `resources/icons/RigidConstraint.png`. Both are 32×32 RGBA in the Fugue style of `Terrain.png`/`Rotate.png`: a chain link for Weld, and a link with a pin for Rigid. Draw them with a stdlib-only `py` script in the scratchpad (a zlib + struct PNG writer; Pillow is not installed), and add entries to `resources/icons/ATTRIBUTION.md`.
- Modify:
  - `src/ide/ClassOrder.hpp`: both in the Attachment cluster, at the current rank (10).
  - `src/ide/InsertPopup.cpp`: `describe_class` gets the spec's two descriptions.
  - `src/engine_core/LuaApi.cpp`: docs rows for:
    - Constraint's `PVInstance1`, `PVInstance2`, `Enabled`, `Active`;
    - PVInstance's `AssemblyRoot`.
  - `src/engine_instances/README.md`: the classes, the assembly, and a fixed PVInstance list.
  - `tests/LuauCompleteTest.cpp`: `testInsertFilter`'s expected list rewritten to the current ClassOrder, with both classes.
  - `tests/PropertiesTest.cpp`: the new tests below.

**Interfaces:**
- Consumes: Tasks 1–3.

- [ ] **Step 1: Write the failing tests:**
  - **`tests/LuauCompleteTest.cpp`:** the new expected list. Run `engine-tests` first to see today's real order, then add `RigidConstraint` and `WeldConstraint` where ClassOrder ranks them.
  - **`tests/PropertiesTest.cpp`**, with the existing `clickExplorer` harness:
    - Selecting a WeldConstraint shows a `Constraint` group with PVInstance1, PVInstance2, Enabled, and a read-only Active.
    - Picking PVInstance1 by clicking a GameObject in the Explorer sets it.
    - Enabling a second RigidConstraint onto the same sub refreshes the selected one's Active row to false, with no selection change. First `set_guid` the selected constraint to `"b..."` and the second to `"a..."`, so the selected one loses rigid rule 1.
    - A selected part shows a read-only AssemblyRoot.
- [ ] **Step 2: Run** `build/Debug/engine-tests.exe` and `build/Debug/properties-tests.exe`, after building those targets. Expected: the new checks fail.
- [ ] **Step 3: Implement.** The Active refresh comes from Task 3's PropertiesPanel revision check; fix it here if the test shows it missing.
- [ ] **Step 4: Run** both programs again, and `explorer-tests`. Expected: pass.
- [ ] **Step 5: Commit** "Studio: constraint icons, Insert order and descriptions, Properties rows, and docs". Commit the icons separately first, as "Icons: WeldConstraint.png and RigidConstraint.png, a chain link and a pinned link".

---

### Task 12: Constraint visualization

**Files:**
- Modify:
  - `src/runner/GameView.hpp/.cpp`:
    - `readSelectedBodies`, under the read lock, also stores for each selected Constraint a `ConstraintPivots` (from `Assemblies::refresh`, `active`, and `end_pivot`), and marks both ends' physics nodes for outlining;
    - `collectOutlines` (no lock, every frame) calls `constraint_lines` with a `DraggerView` built as `collectHandles` builds one.
  - Create `src/engine_core/ConstraintLines.hpp`, `ConstraintLines.cpp`, `sandbox/constraint_lines_tests.cpp`, and add them to `CMakeLists.txt`.

**Interfaces:**
- Consumes: `Assemblies::end_pivot`, `active`, and the existing outline builders for a PhysicsBase.
- Produces:
```cpp
// What GameView reads under the lock for one selected constraint.
struct ConstraintPivots { bool rigid = false; bool active = false;
                          bool has1 = false; Matrix4 pivot1{}; bool has2 = false; Matrix4 pivot2{}; };
// The Scene View's lines for it: a segment between the two pivots (when both resolve), and a small
// three-axis cross at each resolved pivot, sized by handle_scale(view, pivot), colored Weld
// (0.35, 0.75, 1), Rigid (1, 0.7, 0.2), or gray (0.55) when inactive. 7 floats per point (xyz rgba),
// as Renderer::setToolLines takes them. Pure: no DataModel, no lock.
void constraint_lines(const ConstraintPivots& c, const DraggerView& view, std::vector<float>& out);
```

- [ ] **Step 1: Write the failing tests** in `sandbox/constraint_lines_tests.cpp`, tag `[assembly][viz]`, each building a `DraggerView` by hand:
  - `VZ1`: an active weld gives one segment from pivot 1 to pivot 2 plus two crosses, in the Weld color.
  - `VZ2`: an inactive constraint (Enabled false) gives the same lines in gray.
  - `VZ3`: a constraint with an end missing gives a cross at the one resolved pivot only.
- [ ] **Step 2: Run** `"[viz]"`. Expected: it fails.
- [ ] **Step 3: Implement.** Call it from GameView. The ends' collision outlines reuse the selected-body outline path, as if each end's physics node were selected too.
- [ ] **Step 4: Run** `"[viz]"`, then build `AnarchyStudio` Debug. Expected: pass and a clean build. The look is checked live in Task 13.
- [ ] **Step 5: Commit** "Scene View: a selected constraint draws a line between its pivots and outlines both ends".

---

### Task 13: Full verification and a live check

**Files:** none new.

- [ ] **Step 1:** Build Debug `sandbox` and run the whole sandbox. Expected: exit 0.
- [ ] **Step 2:** Build `ALL_BUILD` in Release, then `cd build && ctest -C Release --output-on-failure` (sandbox, engine-, mcp-, properties-, studio-, explorer-, prefab-editor-, assets-, console-, shell-, terrain-editor-, terminal-tests), then run `build/Release/scene-render-check.exe` by hand from the repository root (it is not in ctest). All are green on main, so any failure is new. Re-run a Debug-only timing failure (analysis "did not settle", decomposition "idle") on a quiet machine before treating it as new.
- [ ] **Step 3:** Build Release `AnarchyStudio` (`--target AnarchyStudio`).
- [ ] **Step 4:** Do a live check over MCP, launching `build/Release/AnarchyStudio.exe` with `ANARCHY_MCP_PORT`, `ANARCHY_MCP_TOKEN`, and a scratch `APPDATA` on a copy of the Bounce sample project.
  1. Create two parts welded, a third RigidConstraint'ed to an Attachment.
  2. `set_selection` on the RigidConstraint, and `screenshot` (lines and outlines show).
  3. `playtest start`, wait 2 s, `screenshot` (the welded pair has fallen as one, and the rigid part rides along).
  4. Stop.

  Look at both screenshots before reporting.
- [ ] **Step 5:** Run `/code-review` (or `superpowers:requesting-code-review`) over the branch. Fix what it confirms, then repeat Steps 1–3 for what the fixes touched.
