# Configure Terrain Tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A "Configure Terrain" document tab, built the way the Prefab editor is, where a person adds, renames, re-points, and removes a Terrain's TerrainMaterials, sees which are in use, and decides what happens to voxels when a material in use is removed.

**Prerequisites:** the `terrain-data` branch (plan 1a: `Terrain`, `TerrainMaterial`, `VoxelVolume`, `Terrain::edit_volume`, `Terrain::add_material`) is merged into `main`. Branch from that `main`. Where this plan names plan 1a's types, read the merged code and use the real names if they differ.

**Architecture:** Three layers, as the Prefab editor has: voxel support in `VoxelVolume` (an island-wide replace and a cheap "Ids in use" answer); data helpers in `src/ide/TerrainMaterials.{hpp,cpp}` (read, add, set, remove — what `PrefabModels` is to the Prefab editor); the pane `src/ide/IdeTerrainEditor.{hpp,cpp}` (cards, picker, remove flow) with a `TerrainEditorHost` of callbacks that `IdeLayout` wires to the simulation thread, one undo step each.

**Tech Stack:** C++20 (MSVC 14.23), JadeFX UI, Catch2 (`sandbox`) for the voxel and data layers, an `Expect`-style test executable for the pane (as `tests/PrefabEditorTest.cpp`).

**Spec:** `docs/superpowers/specs/2026-10-06-terrain-core-design.md`, "Configure Terrain tab" section, and its TerrainMaterial rules ("The instances").

## Global Constraints

- Opening: `Terrain::context_actions` offers `InstanceAction::Edit` as primary, so the Explorer's double-click and its context menu both open the tab. `IdeLayout::edit` reuses an open tab for the same Terrain.
- A card per TerrainMaterial, in Id order: its Id, its editable Name, its Material (picked with `AssetPicker` over `asset_choices(world, "Material")`), and an "In use" badge when some voxel uses its Id. An "Add Material" tile; a counter "n / 255".
- A row for Ids that voxels use but no TerrainMaterial holds, with a Replace action.
- Each edit is one undo step: "Add Terrain Material", "Set Terrain Material", "Rename", "Remove Terrain Material".
- Removing a TerrainMaterial whose Id is in use asks first (a `jadefx::Alert`): **Replace…** (pick another TerrainMaterial of this Terrain, or Default), **Keep Cells** (its voxels keep the Id and draw as the default material), or Cancel. The Alert says a replacement cannot be undone yet. Removing one not in use asks nothing.
- Replace changes every voxel of that Id across the whole Terrain, with no per-call size limit, and is not an undo step (voxel undo is sub-project 2); it marks the place unsaved.
- A deleted Terrain shows "This Terrain no longer exists" in its tab, as a deleted Prefab does.
- Comments in the codebase's style; warning-free at /W4; commit messages a plain imperative sentence ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Removing a material that is in use, then choosing Cancel.** Nothing changes. Pinned in Task 3 (TE5).
2. **Replace on a large island.** It must not hit the 256³ edit limit or touch chunks that do not use the Id. Pinned in Task 1 (VR1, VR2).
3. **A script adds or removes TerrainMaterials while the tab is open.** The cards follow. Pinned in Task 3 (TE6).
4. **Undo after Add, Set, Rename, Remove.** Each undoes alone. Pinned in Task 4 (TE8).
5. **The Terrain is deleted while its tab is open.** The tab says so and does nothing on clicks. Pinned in Task 3 (TE7).

---

### Task 1: Island-wide replace and Ids in use

**Files:**
- Modify: `src/engine_core/terrain/VoxelVolume.hpp/.cpp`, `src/engine_instances/Terrain.hpp/.cpp`
- Modify: `sandbox/terrain_voxel_tests.cpp`

**Interfaces:**
- Produces:

```cpp
// VoxelVolume
    // Every solid or band cell with Id from takes Id to, across every chunk,
    // with no size limit. Chunks whose Id mask lacks from are skipped
    // untouched. Returns how many chunks changed.
    std::size_t replace_everywhere(std::uint8_t from, std::uint8_t to);
    // Bumped by every change to the chunk map; ids_used() is cached per revision.
    std::uint64_t revision() const;

// Terrain
    // replace_everywhere through edit_volume, so it marks the place unsaved.
    void replace_material_everywhere(int from, int to);
```

`ids_used()` (plan 1a) becomes cached: recomputed only when `revision()` changed since the last call.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("VR1 replace_everywhere changes one Id across far-apart chunks", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(5000.f, 0.f, 0.f, 5.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 900.f, 0.f, 5.f), 4));
    volume.replace_everywhere(3, 7);
    REQUIRE(volume.cell(CellCoord{0, 0, 0}).material == 7);
    REQUIRE(volume.cell(CellCoord{5000, 0, 0}).material == 7);
    REQUIRE(volume.cell(CellCoord{0, 900, 0}).material == 4);
    REQUIRE((volume.ids_used()[0] >> 3 & 1u) == 0u);
    REQUIRE((volume.ids_used()[0] >> 7 & 1u) == 1u);
}

TEST_CASE("VR2 replace_everywhere leaves chunks without the Id untouched", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(300.f, 0.f, 0.f, 5.f), 4));
    const ChunkMap before = volume.chunks();
    REQUIRE(volume.replace_everywhere(3, 7) > 0u);
    for (const auto& [coord, chunk] : before) {
        if (coord.x >= 8) REQUIRE(volume.chunks().at(coord) == chunk);   // the far ball's chunks: same pointers
    }
    REQUIRE(volume.replace_everywhere(9, 1) == 0u);
}

TEST_CASE("VR3 revision moves with every change, and ids_used follows it", "[terrain]") {
    VoxelVolume volume;
    const std::uint64_t r0 = volume.revision();
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 5.f), 2));
    REQUIRE(volume.revision() != r0);
    REQUIRE((volume.ids_used()[0] >> 2 & 1u) == 1u);
    const std::uint64_t r1 = volume.revision();
    REQUIRE_FALSE(volume.fill(ball_at(1000.f, 0.f, 0.f, 0.f), 2));   // a no-op edit
    REQUIRE(volume.revision() == r1);
}
```

- [ ] **Step 2–5:** fail; implement (`replace_everywhere` walks `chunks_`, skips a chunk unless its `ids_used()` has `from`, clones, rewrites matching cells, `finish()`es, marks dirty; `revision_` bumps in `edit`, `set_chunks`, `clear`, `replace_everywhere` only when something changed); run `[terrain]`; commit (`Replace a terrain material across a whole island`).

---

### Task 2: The data helpers

**Files:**
- Create: `src/ide/TerrainMaterials.hpp`, `src/ide/TerrainMaterials.cpp`
- Create: `tests/TerrainEditorTest.cpp`; `CMakeLists.txt`: `add_executable(terrain-editor-tests tests/TerrainEditorTest.cpp)` linked to `studio`, added to the warnings list and the ctest `foreach(suite ...)` list exactly as `prefab-editor-tests` is (CMakeLists.txt ~668-672, ~928, ~1015)

**Interfaces:**

```cpp
namespace ide {

// One TerrainMaterial, as the Configure Terrain tab shows it.
struct TerrainMaterialView {
    engine_core::InstanceId id = 0;
    int material_id = 0;          // the TerrainMaterial's Id, 1-255
    std::string name;
    engine_core::InstanceId material = 0;   // 0: none or gone
    std::string material_name;
    bool material_missing = false;          // it names a Material that no longer exists
    bool in_use = false;                    // some voxel uses its Id
    bool operator==(const TerrainMaterialView&) const = default;   // or spelled out, as ModelView does
};

// What a Terrain's tab shows. The caller holds a read lock.
struct TerrainMaterialsView {
    bool alive = false;                        // the Terrain exists
    std::string terrain_name;
    std::vector<TerrainMaterialView> materials;   // by Id
    std::vector<int> unassigned_in_use;           // Ids voxels use that no TerrainMaterial holds, ascending
    std::uint64_t voxel_revision = 0;
};
TerrainMaterialsView read_terrain_materials(const engine_core::DataModel& world, engine_core::InstanceId terrain);

// SimulationThread. Each returns the new or changed instance, or 0 with error set.
engine_core::InstanceId add_terrain_material(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                             engine_core::InstanceId material, std::string& error);
std::optional<std::string> set_terrain_material(engine_core::DataModel& world, engine_core::InstanceId entry,
                                                engine_core::InstanceId material);
// What happens to a removed TerrainMaterial's voxels.
struct RemoveChoice {
    enum class Kind { KeepCells, Replace } kind = Kind::KeepCells;
    int replace_with = 0;   // Replace: another TerrainMaterial's Id, or 0 for the default
};
std::optional<std::string> remove_terrain_material(engine_core::DataModel& world, engine_core::InstanceId entry,
                                                   RemoveChoice choice);
// Unassigned Id from becomes Id to (0: default) across the Terrain.
std::optional<std::string> replace_unassigned(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                              int from, int to);

}  // namespace ide
```

`remove_terrain_material` with Replace runs `Terrain::replace_material_everywhere(id, replace_with)` first, then destroys the entry (`destroy_error` check, then `destroy_tree`), as `delete_instances` does.

- [ ] **Step 1: Write the failing tests**

Create `tests/TerrainEditorTest.cpp` with the same scaffolding as `tests/PrefabEditorTest.cpp` (`SimRole`, `Expect`, `gFailures`, a `Rig` holding a real `engine_core::Game`, `main` calling each test and returning 1 on failure). The rig makes two Materials ("Rock", "Grass") under the Assets service and a Terrain in Workspace. First test function:

```cpp
void data_helpers() {
    Rig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain, rig.rock, error);
    const InstanceId grass = ide::add_terrain_material(rig.game, rig.terrain, rig.grass, error);
    auto view = ide::read_terrain_materials(rig.game, rig.terrain);
    Expect(view.alive && view.materials.size() == 2, "both are listed");
    Expect(view.materials[0].material_id == 1 && view.materials[0].name == "Rock", "Rock is Id 1, named after its Material");
    Expect(!view.materials[0].in_use, "nothing uses it yet");

    rig.fill_ball(0, 0, 0, 5, 1);   // Terrain::edit_volume fill with Id 1
    rig.fill_ball(100, 0, 0, 5, 9); // an Id no TerrainMaterial holds
    view = ide::read_terrain_materials(rig.game, rig.terrain);
    Expect(view.materials[0].in_use && !view.materials[1].in_use, "In use follows the voxels");
    Expect(view.unassigned_in_use == std::vector<int>{9}, "Id 9 is used but unassigned");

    Expect(!ide::set_terrain_material(rig.game, grass, rig.rock), "Set points it at another Material");
    Expect(ide::read_terrain_materials(rig.game, rig.terrain).materials[1].material == rig.rock, "and it shows");

    Expect(!ide::remove_terrain_material(rig.game, rock, {ide::RemoveChoice::Kind::Replace, 2}), "Replace then remove");
    Expect(rig.cell_id(0, 0, 0) == 2, "Rock's voxels became Grass's Id");
    Expect(!ide::replace_unassigned(rig.game, rig.terrain, 9, 0), "unassigned Id 9 to the default");
    Expect(rig.cell_id(100, 0, 0) == 0, "and it is the default now");

    const InstanceId again = ide::add_terrain_material(rig.game, rig.terrain, 0, error);
    rig.fill_ball(200, 0, 0, 5, 1);
    Expect(!ide::remove_terrain_material(rig.game, again, {}), "Keep Cells removes only the TerrainMaterial");
    Expect(rig.cell_id(200, 0, 0) == 1, "its voxels keep Id 1");
}
```

Give `Rig` the small helpers `fill_ball(x, y, z, r, id)` (through `Terrain::edit_volume`) and `cell_id(x, y, z)`.

- [ ] **Step 2–5:** fail, implement (patterned on `PrefabModels.cpp`; `in_use` and `unassigned_in_use` from `VoxelVolume::ids_used()`, Id 0 excluded), build `terrain-editor-tests`, run it from the source dir (`build/Debug/terrain-editor-tests.exe`), commit (`Add the data behind the Configure Terrain tab`).

---

### Task 3: The pane

**Files:**
- Create: `src/ide/IdeTerrainEditor.hpp`, `src/ide/IdeTerrainEditor.cpp`
- Modify: `tests/TerrainEditorTest.cpp`, `CMakeLists.txt` (sources)

**Interfaces:**

```cpp
namespace ide {

struct TerrainEditorHost {
    std::function<void(engine_core::InstanceId terrain, engine_core::InstanceId material,
                       std::shared_ptr<InsertResult> result)> add;
    std::function<void(engine_core::InstanceId entry, engine_core::InstanceId material)> set_material;
    std::function<void(engine_core::InstanceId id, std::string name)> rename;
    std::function<void(engine_core::InstanceId entry, RemoveChoice choice)> remove;
    std::function<void(engine_core::InstanceId terrain, int from, int to)> replace_unassigned;
    std::function<void(std::string text)> notice;
};

class IdeTerrainEditor : public IdePane {
public:
    IdeTerrainEditor(engine_core::DataModel& world, engine_core::InstanceId terrain, TerrainEditorHost host = {});
    ~IdeTerrainEditor() override;
    engine_core::InstanceId terrain() const { return terrain_; }
    const TerrainMaterialsView& view() const { return view_; }
    void addMaterial();
    void requestRemove(engine_core::InstanceId entry);   // asks first when in use
    // For tests: the widgets a person would click.
    jadefx::Node* cardNode(engine_core::InstanceId entry) const;
    jadefx::Node* materialSlot(engine_core::InstanceId entry) const;
    jadefx::Node* deleteButton(engine_core::InstanceId entry) const;
    jadefx::Node* inUseBadge(engine_core::InstanceId entry) const;
    jadefx::Node* addTile() const;
    jadefx::Label* counter() const;
    jadefx::Node* unassignedRow() const;
    jadefx::Node* goneNote() const;
    jadefx::TextField* renameField(engine_core::InstanceId entry) const;
    jadefx::Alert* removeAlert() const;            // null when not asking
    jadefx::Node* pickerRow(engine_core::InstanceId choice) const;   // the open picker's row; 0 is Default/None
};

}  // namespace ide
```

Build it from `IdePrefabEditor` (read `src/ide/IdePrefabEditor.hpp/.cpp` first and mirror its structure, CSS approach, helper functions, refresh-by-polling in `layoutChildren` under a read lock with `kFrameLockWait`, `watch_changes` on the TerrainMaterial ids, the rename field, the context menu, `AssetPicker` use, Delete/F2 keys, and its "gone" note):

- **Header:** a badge icon (reuse an existing terrain-ish or generic icon; do not add art), the Terrain's name as title, a subtitle `"<n> / 255 materials"` (the counter), and an "Add Material" primary button.
- **Cards** (`te-card`), each: the Id in a small pill (`te-id`), the Name (double-click or F2 renames), an "In use" badge (`te-in-use`, styled like `.problems-badge`, shown only when in use), a Material slot (click opens `AssetPicker` with `asset_choices(world, "Material")`, None clears it; a missing Material shows as in the Prefab editor's `.missing` slot), and a delete icon.
- **Add tile** at the end of the grid, as "New Model" is: "New Material".
- **Unassigned row** under the header, shown only when `unassigned_in_use` is not empty: `"Ids 3, 7 have voxels but no material: they draw as the default."` and a "Replace…" button that opens a picker of this Terrain's TerrainMaterials plus "Default" (Id 0); choosing one calls `host.replace_unassigned` for each listed Id.
- **Remove flow** (`requestRemove`): not in use → `host.remove(entry, {KeepCells})`. In use → a `jadefx::Alert` (Warning) owned by the pane, as `IdeSearch` keeps `alert_`: header `"<Name> is used by voxels"`, text `"Replace them with another material, or keep them: they draw as the default material until a new material takes Id <n>. A replacement cannot be undone yet."`, buttons "Replace…" (element id `terrain-remove-replace`), "Keep Cells" (`terrain-remove-keep`), Cancel (`terrain-remove-cancel`). Replace… opens the same TerrainMaterials-plus-Default picker anchored to the card, excluding the one being removed; picking calls `host.remove(entry, {Replace, id})`. Keep Cells calls `host.remove(entry, {KeepCells})`. Cancel does nothing.
- **Refresh:** re-read when `tree_revision`, a watched-instance change, or `view_.voxel_revision` (from `VoxelVolume::revision()`) moved.

- [ ] **Step 1: Write the failing tests**

Add to `tests/TerrainEditorTest.cpp` a pane rig: the `Rig` from Task 2 plus a direct-write host (as PrefabEditorTest's host writes straight to the Game), the pane, a headless `jadefx::Scene`, and `frames`/`click`/`key` helpers copied from PrefabEditorTest. Tests:

```cpp
void TE1_cards_and_counter();        // two TerrainMaterials -> two cards in Id order, counter "2 / 255 materials"
void TE2_add_tile();                 // clicking New Material adds one; counter "3 / 255"
void TE3_set_material_and_rename();  // pick Grass in a card's slot -> set; F2, type, Enter -> renamed
void TE4_remove_unused_asks_nothing(); // delete icon on an unused card -> removed at once, no Alert
void TE5_remove_used_asks();         // in use -> Alert; Cancel -> nothing changed; again -> Keep Cells -> removed,
                                     // voxels keep the Id; again on another -> Replace… -> pick Default -> voxels Id 0
void TE6_script_changes_follow();    // add/destroy a TerrainMaterial directly on the Game -> cards follow after frames()
void TE7_deleted_terrain();          // destroy the Terrain -> goneNote visible; clicking Add does nothing
void TE9_unassigned_row();           // voxels on an unassigned Id -> row visible with the Id; Replace… -> pick one -> row gone
```

Write each concretely, in PrefabEditorTest's style (`Expect(condition, "plain sentence")`). Find Alert buttons by their element ids through the scene, as the IDE's unsaved-changes tests find `"unsaved-save"`.

- [ ] **Step 2–5:** fail, implement, run `terrain-editor-tests` (and `prefab-editor-tests` to confirm nothing shared broke), commit (`Add the Configure Terrain tab`).

---

### Task 4: Opening it from the Studio, with undo

**Files:**
- Modify: `src/engine_instances/Terrain.hpp/.cpp` (`context_actions`)
- Modify: `src/ide/IdeLayout.hpp`, `src/ide/IdeLayoutEditing.cpp` (`edit`, `edit_terrain`, `terrain_editor_host`, `open_terrains_`), `src/ide/IdeLayoutProject.cpp` or wherever `close_script_editors` closes Prefab tabs (~IdeLayoutEditing.cpp:701-745)
- Modify: `tests/TerrainEditorTest.cpp` or the studio layout test that covers `IdeLayout::edit` for Prefabs (find it with `grep -rn "edit_prefab\|open_prefabs_" tests sandbox`)

**Interfaces:**
- `void Terrain::context_actions(std::vector<ContextAction>& out) const` → `{InstanceAction::Edit, true}` then the base actions (as `Prefab::context_actions`, `AssetInstances.cpp:395-398`).
- `IdeLayout::edit`: an `is_terrain` branch → `edit_terrain(id, *home)`, mirroring `edit_prefab` (`IdeLayoutEditing.cpp:566-578`) with `open_terrains_` (`std::unordered_map<std::uint32_t, std::weak_ptr<IdeTerrainEditor>>`) and `add_select_to_tab_menu`.
- `terrain_editor_host()`, mirroring `prefab_editor_host()` (`:597-626`): every callback runs through `runner_.simulation().on_simulation(...)` inside `ScopedRecording step(world, "<name>")` with names "Add Terrain Material", "Set Terrain Material", "Remove Terrain Material", "Replace Terrain Material"; `rename` uses `IdeLayout::rename`; errors go to `toast_later`.
- `close_script_editors` closes Terrain tabs too and clears `open_terrains_`.

- [ ] **Step 1: Write the failing tests**

- **TE8 undo:** with the real host functions run on the test's Game inside `ScopedRecording` (factor the bodies of the host lambdas into free functions in `TerrainMaterials.cpp` — `run_add`, `run_set`, `run_remove` — so the test calls exactly what the Studio calls): Add, Set, Rename, Remove (Keep Cells), then four `history().undo()`s restore each step alone, in reverse.
- **Opening:** following whichever existing test drives `IdeLayout::edit` on a Prefab, a Terrain's primary context action is Edit and running it docks one `IdeTerrainEditor`; running it again reveals the same one.

- [ ] **Step 2–4:** fail, implement, run `terrain-editor-tests`, the full sandbox, and `prefab-editor-tests`.

- [ ] **Step 5: In the Studio.** Build Release `AnarchyStudio`, open a project, insert a Terrain, double-click it in Explorer, add two materials, pick Materials, fill a ball from the command bar with the first (`workspace.Terrain:FillBall(Vector3.new(0, 10, 0), 8, workspace.Terrain:GetMaterialById(1))`), see "In use", delete it, choose Keep Cells, then undo. Screenshot the tab for the report.

- [ ] **Step 6: Commit** (`Open the Configure Terrain tab from a Terrain, one undo step per change`).
