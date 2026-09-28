# Save Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Save never writes over, deletes, or puts back a project file that changed outside the studio since it was last loaded or saved; it stops and lists those files, and the studio offers Overwrite or Cancel.

**Architecture:** `Project::files_` already holds every file's path and bytes as last loaded or saved: the base. Before `save_tree` touches disk, a new private `Project::outside_changes` compares each file the save would write, move, or delete against the disk and the base, and a guarded save throws `ProjectConflict` listing them. A file gone from disk whose instance the studio left alone is left gone. `SaveMode::Overwrite` saves anyway and removes stray files that claim a conflicting GUID. `IdeLayout::save_open_project` catches the conflict and shows an alert.

**Tech Stack:** C++17, CMake, Catch2 v3 (the `sandbox` target), the headless `studio-tests` harness, JadeFX alerts.

**Spec:** Reload From Disk tech spec, Milestone 1: https://claude.ai/code/artifact/c955ce1b-1a6f-48b5-b8df-92b60c7f2a17 (sections "Current behavior", "Model: a three-way comparison per instance", "Milestone 1: the save guard"). Milestones 2 (rescan) and 3 (watcher) get their own plans.

## Global Constraints

- Branch: `reload-from-disk`, off `main`. Commit after each task; messages match the repo's style (one plain sentence, no prefix) and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- The guard runs before anything is written: a guarded save that throws leaves every file on disk byte-for-byte unchanged, and leaves `files_`, `id_guid_`, `guid_id_`, and the authored dirty set untouched.
- Compare by bytes, never by modification time.
- A file whose bytes Save does not rewrite (a move only) is never read and never a conflict.
- Cost: one directory walk of `src/` per save, plus one read of each file Save rewrites or deletes.
- `Project`'s threading contract stays: every call runs on the thread that may mutate the DataModel.
- Every existing test in `./build/sandbox "[project]"` (25 cases, 211 assertions at the start) and `./build/studio-tests` keeps passing.
- Comments follow the codebase: plain sentences that say why, sparse.

## Review Focus

1. A script's two files: outside edits only the `.luau` while the studio changes only `Enabled` in the `.meta.json`. Expect no conflict and the outside source kept. (Task 1, G5.)
2. Byte-identical rewrites, as from a `git checkout` that touches files without changing them. Expect no conflict. (Task 1, G6.)
3. A save during play, where an instance changed before Test was also edited outside. Expect a conflict, since the place captured at Test is what gets written. (Task 1, G7.)
4. The studio adds a child under a folder instance deleted outside. Expect a conflict on the folder, never a folder written with no `init.json`. (Task 2, G14.)
5. Junk inside `src/` that mentions a GUID: `.DS_Store`, a hidden backup folder, a Finder copy named `A.<guid> copy.json`. Expect the GUID walk to ignore it, so a deleted file reads as deleted, not moved. (Task 2, G15.)

Known limit, not fixed here: Overwrite on an instance that was turned into a folder outside, with new children of its own, removes the stray `init.json` and leaves those children in a folder that no longer loads. Milestone 2's rescan brings such changes into the place before a Save.

## File Structure

- `src/engine_core/Project.hpp`: add `SaveConflict`, `ProjectConflict`, `SaveMode`, `describe_conflict`; `save(SaveMode)`; private `outside_changes`; `save_tree(bool, SaveMode)`.
- `src/engine_core/Project.cpp`: add `guid_claims` (anonymous namespace), `describe_conflict`, `ProjectConflict`'s constructor, `Project::outside_changes`; change `save`, `save_tree`.
- `sandbox/project_tests.cpp`: new `[guard]` test cases G1 to G18 at the end of the file.
- `src/ide/IdeLayout.hpp` / `.cpp`: `save_open_project(then, overwrite)`, new `confirm_overwrite`, `save_project` passes `then` through.
- `tests/SaveConflictTest.cpp` (new), `tests/StudioLayoutTest.cpp`, `CMakeLists.txt`: the studio test.
- `README.md`: one paragraph in "Projects".

Build and test commands (the `build/` directory is already configured):

- `cmake --build build --parallel --target sandbox && ./build/sandbox "[guard]"`
- `./build/sandbox "[project]"`
- `cmake --build build --parallel --target studio-tests && ./build/studio-tests`

---

### Task 1: The guard API, and outside edits under a studio change

**Files:**
- Modify: `src/engine_core/Project.hpp`
- Modify: `src/engine_core/Project.cpp` (`save`, `save_tree`, new definitions after `sanitize_file_name`)
- Test: `sandbox/project_tests.cpp` (append)

**Interfaces:**
- Produces (public, `engine_core`):
  - `struct SaveConflict { enum class Kind { EditedOutside, DeletedOutside, MovedOutside }; std::string guid; std::string path; Kind kind = Kind::EditedOutside; };`
  - `class ProjectConflict : public ProjectError { explicit ProjectConflict(std::vector<SaveConflict>); const std::vector<SaveConflict>& conflicts() const; };`
  - `enum class SaveMode { Guarded, Overwrite };`
  - `std::string describe_conflict(const SaveConflict&);` returns `"<path> changed on disk"`, `"<path> was deleted on disk"`, or `"<path> was moved or renamed on disk"`.
  - `void Project::save(SaveMode mode = SaveMode::Guarded);`
- Produces (private): `std::vector<SaveConflict> Project::outside_changes(const std::map<std::string, Files>& next) const;` (Task 2 widens it); `void save_tree(bool full, SaveMode mode = SaveMode::Guarded);`

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/project_tests.cpp`:

```cpp
namespace {

// The conflicts a guarded save stopped on. Empty when it saved.
std::vector<engine_core::SaveConflict> save_conflicts(Project& project) {
    try {
        project.save();
    } catch (const engine_core::ProjectConflict& conflict) {
        return conflict.conflicts();
    }
    return {};
}

}  // namespace

TEST_CASE("G1 an outside edit to an instance the studio left alone survives a save", "[G1][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    const std::string outside = read_file(dir.path / leaf(game, a.id())) + "\n";
    write_file(dir.path / leaf(game, a.id()), outside);

    b.set_color(rgb(0.f, 0.f, 1.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, b.id())});
    REQUIRE(read_file(dir.path / leaf(game, a.id())) == outside);
}

TEST_CASE("G2 an outside edit under a studio edit stops the save and writes nothing", "[G2][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    const std::string path = leaf(game, a.id());
    write_file(dir.path / path, read_file(dir.path / path) + "\n");

    a.set_color(rgb(1.f, 0.f, 0.f));
    b.set_color(rgb(0.f, 1.f, 0.f));
    const auto before = tree_files(dir.path);
    std::string message;
    try {
        project.save();
    } catch (const engine_core::ProjectConflict& conflict) {
        message = conflict.what();
        REQUIRE(conflict.conflicts().size() == 1);
        REQUIRE(conflict.conflicts()[0].guid == game.guid(a.id()));
        REQUIRE(conflict.conflicts()[0].path == path);
        REQUIRE(conflict.conflicts()[0].kind == engine_core::SaveConflict::Kind::EditedOutside);
    }
    REQUIRE(message.find(path) != std::string::npos);
    // Nothing was written, B included, and the next save sees the same thing.
    REQUIRE(tree_files(dir.path) == before);
    REQUIRE(save_conflicts(project).size() == 1);
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("G3 a studio delete of a file edited outside stops the save", "[G3][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    const std::string path = leaf(game, a);
    const std::string outside = read_file(dir.path / path) + "\n";
    write_file(dir.path / path, outside);

    game.destroy(a);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::EditedOutside);
    REQUIRE(read_file(dir.path / path) == outside);
}

TEST_CASE("G4 a folder rename carries a child edited outside along", "[G4][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& holder = add_part(game, 0, "Holder");
    engine_core::GameObject& inner = add_part(game, holder.id(), "Inner");
    project.save();
    const std::string inner_name = "/Inner." + game.guid(inner.id()) + ".json";
    const fs::path old_path = dir.path / ("src/Holder." + game.guid(holder.id()) + inner_name);
    const std::string outside = read_file(old_path) + "\n";
    write_file(old_path, outside);

    // Inner's bytes do not change, so the save only moves its file.
    game.set_name(holder.id(), "Box");
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / ("src/Box." + game.guid(holder.id()) + inner_name)) == outside);
}

TEST_CASE("G5 a script's two files are checked one by one", "[G5][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const std::string luau = leaf(game, main.id(), ".luau");
    write_file(dir.path / luau, "print(\"outside\")\n");

    SECTION("a studio change to the other file saves, and keeps the outside source") {
        main.set_enabled(false);
        REQUIRE(save_conflicts(project).empty());
        REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, main.id(), ".meta.json")});
        REQUIRE(read_file(dir.path / luau) == "print(\"outside\")\n");
    }
    SECTION("a studio change to the same file stops the save") {
        main.set_source("print(2)\n");
        const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
        REQUIRE(conflicts.size() == 1);
        REQUIRE(conflicts[0].path == luau);
        REQUIRE(read_file(dir.path / luau) == "print(\"outside\")\n");
    }
}

TEST_CASE("G6 a file rewritten with the same bytes is no conflict", "[G6][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    write_file(dir.path / path, read_file(dir.path / path));

    a.set_color(rgb(1.f, 0.f, 0.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written == std::vector<std::string>{path});
}

TEST_CASE("G7 a save during play checks the place captured at Test", "[G7][guard][project]") {
    TempDir dir;
    ScriptRig rig;
    {
        Project project = Project::create(dir.path, rig.game);
        add_part(rig.game, 0, "Door");
        project.save();
    }
    Project project = Project::load(dir.path, rig.game);
    const InstanceId door = rig.game.find_first_child(0, "Door");
    REQUIRE(door != 0);
    const std::string path = leaf(rig.game, door);
    write_file(dir.path / path, read_file(dir.path / path) + "\n");
    rig.game.game_object(door)->set_color(rgb(1.f, 0.f, 0.f));

    rig.game.start_simulation();
    rig.frames(1, 0.05);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    rig.game.stop_simulation();
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails, since `engine_core::SaveConflict` and `engine_core::ProjectConflict` are not declared.

- [ ] **Step 3: Add the API to `Project.hpp`**

Add `#include <set>` beside the other standard includes. After `class ProjectError`, add:

```cpp
// A file that changed on disk since the last load or save, which a save would
// have written over, deleted, or put back.
struct SaveConflict {
    enum class Kind {
        EditedOutside,   // its bytes differ from the last load or save
        DeletedOutside,  // it is gone, and no other file claims its GUID
        MovedOutside,    // it is gone, and a file elsewhere claims its GUID
    };
    std::string guid;
    // As last loaded or saved: relative to the project root, written with '/'.
    std::string path;
    Kind kind = Kind::EditedOutside;
};

// "src/Part.3f2a.json changed on disk", "... was deleted on disk", or
// "... was moved or renamed on disk".
std::string describe_conflict(const SaveConflict& conflict);

// Thrown by a guarded save before it touches disk. Sorted by path; the message
// names the first.
class ProjectConflict : public ProjectError {
public:
    explicit ProjectConflict(std::vector<SaveConflict> conflicts);
    const std::vector<SaveConflict>& conflicts() const { return conflicts_; }

private:
    std::vector<SaveConflict> conflicts_;
};

// Guarded stops at files changed on disk. Overwrite writes the studio's version anyway.
enum class SaveMode { Guarded, Overwrite };
```

Replace the `save()` declaration and its comment with:

```cpp
    // Edit mode writes the live tree. Play writes the place snapshot, never
    // an instance created during play. Only files whose bytes changed are
    // written. A renamed or moved instance's files move; a destroyed one's are deleted.
    // A guarded save first compares every file it would write or delete with
    // the last load or save, and throws ProjectConflict, writing nothing, when
    // one changed on disk.
    void save(SaveMode mode = SaveMode::Guarded);
```

In the private section, replace `void save_tree(bool full);` with:

```cpp
    void save_tree(bool full, SaveMode mode = SaveMode::Guarded);
    // The files next would write over or delete that changed on disk since the
    // last load or save, sorted by path.
    std::vector<SaveConflict> outside_changes(const std::map<std::string, Files>& next) const;
```

- [ ] **Step 4: Implement it in `Project.cpp`**

Right after the closing brace of `sanitize_file_name`, add:

```cpp
std::string describe_conflict(const SaveConflict& conflict) {
    switch (conflict.kind) {
    case SaveConflict::Kind::EditedOutside:
        return conflict.path + " changed on disk";
    case SaveConflict::Kind::DeletedOutside:
        return conflict.path + " was deleted on disk";
    case SaveConflict::Kind::MovedOutside:
        return conflict.path + " was moved or renamed on disk";
    }
    return conflict.path;
}

namespace {

std::string conflict_message(const std::vector<SaveConflict>& conflicts) {
    if (conflicts.empty()) {
        return "files changed on disk";
    }
    std::string text = describe_conflict(conflicts.front()) + " since it was loaded or saved";
    if (conflicts.size() > 1) {
        text += " (and " + std::to_string(conflicts.size() - 1) + " more)";
    }
    return text;
}

}  // namespace

ProjectConflict::ProjectConflict(std::vector<SaveConflict> conflicts)
    : ProjectError(conflict_message(conflicts)), conflicts_(std::move(conflicts)) {}
```

Replace `void Project::save() { save_tree(false); }` with:

```cpp
void Project::save(SaveMode mode) { save_tree(false, mode); }
```

Before `void Project::save_tree`, add:

```cpp
std::vector<SaveConflict> Project::outside_changes(const std::map<std::string, Files>& next) const {
    std::vector<SaveConflict> conflicts;
    std::error_code error;
    for (const auto& [guid, base] : files_) {
        const auto planned = next.find(guid);
        const bool removed = planned == next.end();
        // A file's bytes matter only where the save rewrites or deletes it. A
        // move alone carries whatever is on disk to the new path.
        struct Own {
            const std::string* path;
            const std::string* bytes;
            bool rewritten;
        };
        std::vector<Own> own{{&base.props_path, &base.props_bytes,
                              removed || planned->second.props_bytes != base.props_bytes}};
        if (base.has_source) {
            own.push_back({&base.source_path, &base.source_bytes,
                           removed || !planned->second.has_source ||
                               planned->second.source_bytes != base.source_bytes});
        }
        for (const Own& file : own) {
            const fs::path target = disk_path(root_, *file.path);
            if (!file.rewritten || !fs::exists(target, error)) {
                continue;
            }
            if (read_file(target) != *file.bytes) {
                conflicts.push_back({guid, *file.path, SaveConflict::Kind::EditedOutside});
                break;
            }
        }
    }
    std::sort(conflicts.begin(), conflicts.end(),
              [](const SaveConflict& a, const SaveConflict& b) { return a.path < b.path; });
    return conflicts;
}
```

Change the definition line `void Project::save_tree(bool full) {` to `void Project::save_tree(bool full, SaveMode mode) {`, and right after the line `std::map<std::string, Files> next = plan_files(tree, layout.src, files_);` add:

```cpp
    // A file changed on disk since the last load or save stops a guarded save
    // here, before anything is written.
    if (mode == SaveMode::Guarded) {
        std::vector<SaveConflict> conflicts = outside_changes(next);
        if (!conflicts.empty()) {
            throw ProjectConflict(std::move(conflicts));
        }
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[guard]"`
Expected: `All tests passed` (G1 to G7). G1 and G4 pass before this task too; they pin behavior the guard must keep.

Run: `./build/sandbox "[project]"`
Expected: `All tests passed`, with 32 test cases.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_tests.cpp
git commit -m "$(cat <<'EOF'
Save stops before writing over a file changed on disk

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Files gone from disk: left gone, or a conflict

**Files:**
- Modify: `src/engine_core/Project.hpp` (`outside_changes` signature)
- Modify: `src/engine_core/Project.cpp` (new `guid_claims`; `outside_changes`; `save_tree`)
- Test: `sandbox/project_tests.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `SaveConflict`, `ProjectConflict`, `SaveMode`, `save_conflicts` test helper.
- Produces (private): `std::vector<SaveConflict> Project::outside_changes(const std::vector<AuthoredNode>& tree, const std::map<std::string, Files>& next, const std::map<std::string, std::vector<std::string>>& claims, std::set<std::string>& left_gone) const;` and, in `Project.cpp`'s anonymous namespace, `std::map<std::string, std::vector<std::string>> guid_claims(const fs::path& root, const std::string& src);`. Task 3 uses both from `save_tree`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/project_tests.cpp`:

```cpp
namespace {

// src/ files whose own name carries guid: a leaf's .json, .meta.json, or .luau.
std::vector<std::string> leaf_files_of(const fs::path& root, const std::string& guid) {
    std::vector<std::string> out;
    for (const auto& [path, bytes] : tree_files(root)) {
        if (path.substr(path.rfind('/') + 1).find("." + guid + ".") != std::string::npos) {
            out.push_back(path);
        }
    }
    return out;
}

// A folder instance Box holding Keep, and a leaf beside it.
struct BoxAndLeaf {
    InstanceId box = 0;
    InstanceId keep = 0;
    InstanceId leaf = 0;

    BoxAndLeaf(DataModel& game, const char* leaf_name) {
        box = add_part(game, 0, "Box").id();
        keep = add_part(game, box, "Keep").id();
        leaf = add_part(game, 0, leaf_name).id();
    }

    // Where a file for the leaf lands when it is moved into Box.
    fs::path moved(const fs::path& root, const DataModel& game) const {
        return root / ("src/Box." + game.guid(box)) /
               (engine_core::sanitize_file_name(game.name(leaf)) + "." + game.guid(leaf) + ".json");
    }
};

}  // namespace

TEST_CASE("G8 a file deleted outside stays deleted when the studio left it alone", "[G8][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    fs::remove(dir.path / leaf(game, a.id()));

    b.set_color(rgb(0.f, 0.f, 1.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, b.id())});
    REQUIRE_FALSE(fs::exists(dir.path / leaf(game, a.id())));
    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE_FALSE(fs::exists(dir.path / leaf(game, a.id())));
}

TEST_CASE("G9 a leaf moved outside keeps one file when the studio left it alone", "[G9][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string guid = game.guid(ids.leaf);
    fs::rename(dir.path / leaf(game, ids.leaf), ids.moved(dir.path, game));

    game.game_object(ids.keep)->set_color(rgb(0.f, 1.f, 0.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(leaf_files_of(dir.path, guid).size() == 1);
    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    REQUIRE(again.guid(again.parent(by_guid(again, guid))) == game.guid(ids.box));
}

TEST_CASE("G10 a file deleted outside under a studio edit stops the save", "[G10][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    fs::remove(dir.path / path);

    a.set_color(rgb(1.f, 0.f, 0.f));
    const auto before = tree_files(dir.path);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::DeletedOutside);
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("G11 a file moved outside under a studio edit stops the save", "[G11][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    // A dot in the Name: the GUID is still the last part of the file name.
    const BoxAndLeaf ids(game, "Loose.v2");
    project.save();
    const std::string path = leaf(game, ids.leaf);
    fs::rename(dir.path / path, ids.moved(dir.path, game));

    game.game_object(ids.leaf)->set_color(rgb(1.f, 0.f, 0.f));
    const auto before = tree_files(dir.path);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].guid == game.guid(ids.leaf));
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::MovedOutside);
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("G12 a file deleted on both sides saves quietly", "[G12][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    fs::remove(dir.path / leaf(game, a));

    game.destroy(a);
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().removed.empty());
}

TEST_CASE("G13 a studio delete of a file moved outside stops the save", "[G13][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string path = leaf(game, ids.leaf);
    const fs::path moved = ids.moved(dir.path, game);
    fs::rename(dir.path / path, moved);

    game.destroy(ids.leaf);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::MovedOutside);
    REQUIRE(fs::exists(moved));
}

TEST_CASE("G14 a new child under a folder deleted outside stops the save", "[G14][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string box_dir = "src/Box." + game.guid(ids.box);
    fs::remove_all(dir.path / box_dir);

    add_part(game, ids.box, "New");
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    bool box_listed = false;
    for (const engine_core::SaveConflict& conflict : conflicts) {
        box_listed = box_listed || (conflict.guid == game.guid(ids.box) && conflict.path == box_dir + "/init.json" &&
                                    conflict.kind == engine_core::SaveConflict::Kind::DeletedOutside);
    }
    REQUIRE(box_listed);
    REQUIRE_FALSE(fs::exists(dir.path / box_dir));
}

TEST_CASE("G15 junk that mentions a GUID does not make a deleted file look moved", "[G15][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    const std::string guid = game.guid(a.id());
    const std::string bytes = read_file(dir.path / path);
    write_file(dir.path / "src" / ".DS_Store", "junk");
    write_file(dir.path / "src" / ".backup" / ("A." + guid + ".json"), bytes);
    write_file(dir.path / "src" / ("A." + guid + " copy.json"), bytes);
    fs::remove(dir.path / path);

    a.set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::DeletedOutside);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[guard]"`
Expected: FAIL in G8 (`written` also lists A's file, which Save put back), G9 (two files carry the GUID), G10, G11, G13, and G14 (no conflict reported), and G15 (no conflict reported). G12 passes already.

- [ ] **Step 3: Widen `outside_changes` in `Project.hpp`**

Replace the Task 1 declaration and its comment with:

```cpp
    // The files next would write over, delete, or put back that changed on disk
    // since the last load or save, sorted by path. A GUID whose files are gone
    // while the studio left it alone goes in left_gone instead: the save leaves
    // it gone. claims is every src/ file by the GUID in its name.
    std::vector<SaveConflict> outside_changes(const std::vector<AuthoredNode>& tree,
                                              const std::map<std::string, Files>& next,
                                              const std::map<std::string, std::vector<std::string>>& claims,
                                              std::set<std::string>& left_gone) const;
```

- [ ] **Step 4: Add `guid_claims` to `Project.cpp`**

In the first anonymous namespace, right after `class PlanReader`'s closing `};`, add:

```cpp
// Every file under src that carries a GUID in its name, by GUID: <Name>.<guid>.json,
// .meta.json, and .luau, and a folder's init files under <Name>.<guid>/. Names
// that fit none of these, dotfiles, dot folders, and .tmp files are skipped, as
// a load skips or reports them.
std::map<std::string, std::vector<std::string>> guid_claims(const fs::path& root, const std::string& src) {
    std::map<std::string, std::vector<std::string>> claims;
    const fs::path top = disk_path(root, src);
    std::error_code error;
    for (fs::recursive_directory_iterator it(top, error), end; !error && it != end; it.increment(error)) {
        const std::string name = utf8(it->path().filename());
        std::error_code kind;
        if (it->is_directory(kind)) {
            if (!name.empty() && name[0] == '.') {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (name.empty() || name[0] == '.' || ends_with(name, ".tmp") || !it->is_regular_file(kind)) {
            continue;
        }
        std::string guid;
        if (name == "init.json" || name == "init.meta.json" || name == "init.luau") {
            // The root's init.json names no GUID; its GUID is inside.
            if (it->path().parent_path() == top || !split_stem(utf8(it->path().parent_path().filename()), guid)) {
                continue;
            }
        } else {
            std::string stem;
            if (ends_with(name, ".meta.json")) {
                stem = name.substr(0, name.size() - 10);
            } else if (ends_with(name, ".luau")) {
                stem = name.substr(0, name.size() - 5);
            } else if (ends_with(name, ".json")) {
                stem = name.substr(0, name.size() - 5);
            } else {
                continue;
            }
            if (!split_stem(stem, guid)) {
                continue;
            }
        }
        claims[guid].push_back(it->path().lexically_relative(root).generic_u8string());
    }
    return claims;
}
```

- [ ] **Step 5: Replace `Project::outside_changes`**

Replace the whole Task 1 definition with:

```cpp
std::vector<SaveConflict> Project::outside_changes(const std::vector<AuthoredNode>& tree,
                                                   const std::map<std::string, Files>& next,
                                                   const std::map<std::string, std::vector<std::string>>& claims,
                                                   std::set<std::string>& left_gone) const {
    // A parent counts as changed when a child's file lands in its folder anew,
    // a new child or one the studio moved or renamed: without the parent's own
    // file, that folder would not load.
    std::set<std::string> receives;
    for (const AuthoredNode& node : tree) {
        for (std::size_t child : node.children) {
            const std::string& id = tree[child].guid;
            const auto base = files_.find(id);
            if (base == files_.end() || base->second.props_path != next.at(id).props_path) {
                receives.insert(node.guid);
                break;
            }
        }
    }

    std::vector<SaveConflict> conflicts;
    std::error_code error;
    for (const auto& [guid, base] : files_) {
        const auto planned = next.find(guid);
        const bool removed = planned == next.end();
        // A file's bytes matter only where the save rewrites or deletes it. A
        // move alone carries whatever is on disk to the new path.
        struct Own {
            const std::string* path;
            const std::string* bytes;
            bool rewritten;
        };
        std::vector<Own> own{{&base.props_path, &base.props_bytes,
                              removed || planned->second.props_bytes != base.props_bytes}};
        if (base.has_source) {
            own.push_back({&base.source_path, &base.source_bytes,
                           removed || !planned->second.has_source ||
                               planned->second.source_bytes != base.source_bytes});
        }
        const bool touched = removed || receives.count(guid) != 0 || planned->second.props_path != base.props_path ||
                             planned->second.source_path != base.source_path ||
                             planned->second.has_source != base.has_source ||
                             std::any_of(own.begin(), own.end(), [](const Own& file) { return file.rewritten; });

        const std::string* gone = nullptr;
        for (const Own& file : own) {
            if (gone == nullptr && !fs::exists(disk_path(root_, *file.path), error)) {
                gone = file.path;
            }
        }
        if (gone != nullptr) {
            if (!touched) {
                // The studio left it alone, so the save leaves it gone.
                left_gone.insert(guid);
                continue;
            }
            bool elsewhere = false;
            if (const auto claimed = claims.find(guid); claimed != claims.end()) {
                for (const std::string& path : claimed->second) {
                    elsewhere = elsewhere || (path != base.props_path && path != base.source_path);
                }
            }
            if (removed && !elsewhere) {
                continue;  // Deleted on both sides.
            }
            conflicts.push_back(
                {guid, *gone, elsewhere ? SaveConflict::Kind::MovedOutside : SaveConflict::Kind::DeletedOutside});
            continue;
        }
        for (const Own& file : own) {
            if (file.rewritten && read_file(disk_path(root_, *file.path)) != *file.bytes) {
                conflicts.push_back({guid, *file.path, SaveConflict::Kind::EditedOutside});
                break;
            }
        }
    }
    std::sort(conflicts.begin(), conflicts.end(),
              [](const SaveConflict& a, const SaveConflict& b) { return a.path < b.path; });
    return conflicts;
}
```

- [ ] **Step 6: Use it in `save_tree`**

Replace the Task 1 block after `plan_files` with:

```cpp
    // A file changed on disk since the last load or save stops a guarded save
    // here, before anything is written.
    const std::map<std::string, std::vector<std::string>> claims = guid_claims(root_, layout.src);
    std::set<std::string> left_gone;
    std::vector<SaveConflict> conflicts = outside_changes(tree, next, claims, left_gone);
    if (!conflicts.empty() && mode == SaveMode::Guarded) {
        throw ProjectConflict(std::move(conflicts));
    }
```

In the loop `for (const auto& [guid, files] : next) {`, make the first statements:

```cpp
        if (left_gone.count(guid) != 0) {
            // Gone from disk while the studio left it alone: the save leaves it gone.
            continue;
        }
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[guard]"`
Expected: `All tests passed` (G1 to G15).

Run: `./build/sandbox "[project]"`
Expected: `All tests passed`, 40 test cases.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_tests.cpp
git commit -m "$(cat <<'EOF'
Save leaves files deleted on disk deleted, and stops when the studio changed them

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Overwrite leaves one file per GUID

**Files:**
- Modify: `src/engine_core/Project.cpp` (`save_tree`)
- Test: `sandbox/project_tests.cpp` (append)

**Interfaces:**
- Consumes: Task 2's `claims`, `conflicts`, and `left_gone` locals in `save_tree`; `BoxAndLeaf`, `leaf_files_of`, `save_conflicts` test helpers.
- Produces: `project.save(SaveMode::Overwrite)` writes the studio's version of every conflicting file and removes any other file that claims a conflicting GUID. Task 4 calls it.

- [ ] **Step 1: Write the tests**

Append to `sandbox/project_tests.cpp`. G16 and G18 pass once Task 2 is in; they pin Overwrite for edits and deletes. G17 drives this task's code.

```cpp
TEST_CASE("G16 Overwrite writes the studio's version over an outside edit", "[G16][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    write_file(dir.path / path, read_file(dir.path / path) + "\n");

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    REQUIRE(save_conflicts(project).size() == 1);
    project.save(engine_core::SaveMode::Overwrite);
    REQUIRE(project.last_save().written == std::vector<std::string>{path});
    REQUIRE(read_file(dir.path / path).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos);
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written.empty());
}

TEST_CASE("G17 Overwrite of a file moved outside leaves one file for its GUID", "[G17][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string guid = game.guid(ids.leaf);
    const fs::path moved = ids.moved(dir.path, game);
    fs::rename(dir.path / leaf(game, ids.leaf), moved);

    game.game_object(ids.leaf)->set_color(rgb(0.25f, 0.5f, 0.75f));
    REQUIRE(save_conflicts(project).size() == 1);
    project.save(engine_core::SaveMode::Overwrite);
    REQUIRE(leaf_files_of(dir.path, guid) == std::vector<std::string>{leaf(game, ids.leaf)});
    REQUIRE_FALSE(fs::exists(moved));
    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    const InstanceId loose = by_guid(again, guid);
    REQUIRE(again.parent(loose) == 0);
    REQUIRE(again.game_object(loose)->color().b == 0.75f);
}

TEST_CASE("G18 Overwrite puts back a file deleted outside", "[G18][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    const std::string guid = game.guid(a.id());
    fs::remove(dir.path / path);

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    REQUIRE(save_conflicts(project).size() == 1);
    project.save(engine_core::SaveMode::Overwrite);
    REQUIRE(fs::exists(dir.path / path));
    Project loaded = Project::load(dir.path);
    REQUIRE(loaded.datamodel().game_object(by_guid(loaded.datamodel(), guid))->color().b == 0.75f);
}
```

- [ ] **Step 2: Run the tests to verify G17 fails**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[guard]"`
Expected: FAIL in G17 only: `leaf_files_of` lists two files, the one written at the studio's path and the one moved outside.

- [ ] **Step 3: Remove stray claims in `save_tree`**

In `save_tree`, right after the closing brace of the `for (const auto& [guid, files] : next)` placement loop and before the `for (const auto& [guid, files] : files_)` removal loop, add:

```cpp
    // Overwrite: a GUID the save wrote over a conflict keeps only the files it
    // wrote. Any other file claiming it, as one moved outside, would load as a
    // second instance with the same GUID.
    for (const SaveConflict& conflict : conflicts) {
        const auto claimed = claims.find(conflict.guid);
        if (claimed == claims.end()) {
            continue;
        }
        const auto planned = next.find(conflict.guid);
        for (const std::string& path : claimed->second) {
            if (planned != next.end() && (path == planned->second.props_path || path == planned->second.source_path)) {
                continue;
            }
            const fs::path target = disk_path(root_, path);
            if (fs::exists(target, error)) {
                remove_file(target);
                report.removed.push_back(path);
                note_vacated(path);
            }
        }
    }
```

A guarded save never reaches this loop with conflicts, since it threw.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[guard]"`
Expected: `All tests passed` (G1 to G18).

Run: `./build/sandbox "[project]"`
Expected: `All tests passed`, 43 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/Project.cpp sandbox/project_tests.cpp
git commit -m "$(cat <<'EOF'
Overwrite leaves one file for each GUID it writes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: The studio asks before saving over changes on disk

**Files:**
- Modify: `src/ide/IdeLayout.hpp` (forward declaration; `save_open_project`; new `confirm_overwrite`)
- Modify: `src/ide/IdeLayout.cpp` (`save_open_project`, `save_project`, new `confirm_overwrite`)
- Create: `tests/SaveConflictTest.cpp`
- Modify: `tests/StudioLayoutTest.cpp` (prototype, call)
- Modify: `CMakeLists.txt` (`studio-tests` sources)
- Modify: `README.md` ("Projects")

**Interfaces:**
- Consumes: `engine_core::SaveMode`, `engine_core::ProjectConflict`, `engine_core::SaveConflict`, `engine_core::describe_conflict` from Tasks 1 to 3.
- Produces: `bool IdeLayout::save_open_project(std::function<void()> then = {}, bool overwrite = false);` and `void IdeLayout::confirm_overwrite(const std::vector<engine_core::SaveConflict>& conflicts, std::function<void()> then);`. The alert's buttons have element ids `save-conflict-overwrite` and `save-conflict-cancel`.

- [ ] **Step 1: Write the failing studio test**

Create `tests/SaveConflictTest.cpp`:

```cpp
#include "ide/IdeLayout.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Project.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

}  // namespace

// Saving over a file changed outside the studio asks first. Cancel writes
// nothing, and Overwrite writes the studio's version. Replaces the open place.
int RunSaveConflictTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("anarchy-conflict-test-" + std::to_string(stamp));
    const fs::path root = folder / "ConflictPlace";
    std::string part_file;
    layout.simulation().on_simulation([&](engine_core::DataModel&) {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        engine_core::GameObject& part = game.create_game_object();
        game.set_name(part.id(), "Part");
        game.set_parent(part.id(), 0);
        project.save();
        part_file = "src/Part." + game.guid(part.id()) + ".json";
    });
    layout.open_project_at(root);

    // The studio changes the part's color while something else edits its file.
    layout.simulation().on_simulation([](engine_core::DataModel& game) {
        engine_core::ColorRgb color;
        color.r = 0.25f;
        color.g = 0.5f;
        color.b = 0.75f;
        game.game_object(game.find_first_child(0, "Part"))->set_color(color);
    });
    const std::string outside = ReadBytes(root / part_file) + "\n";
    WriteBytes(root / part_file, outside);

    auto save = [&scene] {
        scene.noteKey(jadefx::Key::S, true, false, jadefx::Key::ModControl);
        scene.noteKey(jadefx::Key::S, false, false, 0);
    };
    save();
    auto* cancel = dynamic_cast<jadefx::Button*>(scene.getElementById("save-conflict-cancel"));
    expect(cancel != nullptr, "saving over a file changed on disk asks first");
    expect(ReadBytes(root / part_file) == outside, "and writes nothing while it asks");
    if (cancel != nullptr) {
        cancel->fire();
    }
    expect(ReadBytes(root / part_file) == outside, "Cancel leaves the changed file as it is");
    expect(layout.has_unsaved_changes(), "and the place still has unsaved changes");

    save();
    auto* overwrite = dynamic_cast<jadefx::Button*>(scene.getElementById("save-conflict-overwrite"));
    expect(overwrite != nullptr, "saving again asks again");
    if (overwrite != nullptr) {
        overwrite->fire();
    }
    expect(ReadBytes(root / part_file).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos,
           "Overwrite writes the studio's color");
    expect(!layout.has_unsaved_changes(), "and the place is saved");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
```

In `tests/StudioLayoutTest.cpp`, add beside the other prototypes (after `int RunPreferencesTests();`):

```cpp
int RunSaveConflictTests(ide::IdeLayout& layout, jadefx::Scene& scene);
```

and right after the block that ends with the toast test's `fs::remove_all(folder, error);` and its closing `}`, add:

```cpp
    failures += RunSaveConflictTests(layout, *scene);
```

In `CMakeLists.txt`, add `tests/SaveConflictTest.cpp` to `add_executable(studio-tests`, after `tests/PreferencesTest.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --parallel --target studio-tests && ./build/studio-tests`
Expected: `FAIL saving over a file changed on disk asks first` and `FAIL saving again asks again`. The guarded save throws `ProjectConflict`, which today's `catch (const std::exception&)` turns into an error alert with no such buttons.

- [ ] **Step 3: Declare the new members in `IdeLayout.hpp`**

Add `struct SaveConflict;` to the `namespace engine_core` forward declarations. Replace `bool save_open_project();` with:

```cpp
    // Saves the open project. When files changed on disk since it was opened or
    // saved, asks whether to overwrite them and returns false; if Overwrite
    // saves, then runs. overwrite saves over them without asking.
    bool save_open_project(std::function<void()> then = {}, bool overwrite = false);
    // Lists the files a guarded save found changed on disk. Overwrite saves over
    // them and runs then; Cancel writes nothing.
    void confirm_overwrite(const std::vector<engine_core::SaveConflict>& conflicts, std::function<void()> then);
```

- [ ] **Step 4: Implement them in `IdeLayout.cpp`**

Replace the start of `IdeLayout::save_open_project` through its error check with:

```cpp
bool IdeLayout::save_open_project(std::function<void()> then, bool overwrite) {
    // Open editors write Source first. During play the save writes the place
    // captured at Test, so play edits stay out of it either way.
    flush_editors();
    std::string error;
    std::vector<engine_core::SaveConflict> conflicts;
    run_now([&](engine_core::DataModel&) {
        try {
            project_->save(overwrite ? engine_core::SaveMode::Overwrite : engine_core::SaveMode::Guarded);
        } catch (const engine_core::ProjectConflict& conflict) {
            conflicts = conflict.conflicts();
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!conflicts.empty()) {
        confirm_overwrite(conflicts, std::move(then));
        return false;
    }
    if (!error.empty()) {
        show_error("Could not save project", error);
        return false;
    }
```

The rest of the function (`mark_saved();` and the toast) stays. In `IdeLayout::save_project`, replace `if (save_open_project() && then) {` with `if (save_open_project(then) && then) {`.

After `IdeLayout::save_open_project`, add:

```cpp
void IdeLayout::confirm_overwrite(const std::vector<engine_core::SaveConflict>& conflicts,
                                  std::function<void()> then) {
    runner_.simulation().scripts().append_output(
        engine_core::ScriptRuntime::OutputKind::Error,
        "Not saved: " + engine_core::describe_conflict(conflicts.front()) +
            (conflicts.size() > 1 ? " (and " + std::to_string(conflicts.size() - 1) + " more)" : std::string()));
    if (scene_ == nullptr || prompt_open_) {
        return;
    }
    prompt_open_ = true;
    std::string detail;
    const std::size_t shown = std::min<std::size_t>(conflicts.size(), 5);
    for (std::size_t index = 0; index < shown; ++index) {
        detail += engine_core::describe_conflict(conflicts[index]) + "\n";
    }
    if (conflicts.size() > shown) {
        detail += "and " + std::to_string(conflicts.size() - shown) + " more\n";
    }
    detail += "\nOverwrite writes the studio's version over them. Cancel saves nothing.";
    const jadefx::ButtonType overwrite("Overwrite", jadefx::ButtonType::Data::OkDone);
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Warning, detail,
                                                 std::vector<jadefx::ButtonType>{overwrite, jadefx::ButtonType::Cancel()});
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(conflicts.size() == 1
                             ? std::string("A file changed on disk since the project was opened or saved.")
                             : std::to_string(conflicts.size()) +
                                   " files changed on disk since the project was opened or saved.");
    alert->setOnClosed([this, overwrite, then = std::move(then)](const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice != nullptr && *choice == overwrite && project_ && save_open_project({}, true) && then) {
            then();
        }
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene_);
    // Stable names for the two answers, so a test can find them.
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {
        {&overwrite, "save-conflict-overwrite"}, {&jadefx::ButtonType::Cancel(), "save-conflict-cancel"}};
    for (const auto& [type, id] : ids) {
        if (jadefx::Button* button = alert->lookupButton(*type)) {
            button->setElementId(id);
        }
    }
    alerts_.push_back(std::move(alert));
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target studio-tests && ./build/studio-tests`
Expected: exits 0 with no `FAIL` lines.

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[project]"`
Expected: `All tests passed`, 43 test cases.

- [ ] **Step 6: Document it in `README.md`**

In the "Projects" section, replace:

```
A toast at the bottom right of the window says what each one did, such as `Saved MyPlace (2 files changed)`. The title shows
```

with:

```
A toast at the bottom right of the window says what each one did, such as `Saved MyPlace (2 files changed)`. Save never writes over a file that changed on disk after the project was opened or last saved, as by git or another editor: it lists those files, and Overwrite writes the studio's version over them while Cancel saves nothing. A file deleted outside the studio stays deleted, unless the studio changed that instance too. The title shows
```

- [ ] **Step 7: Build everything and run the whole suite**

Run: `cmake --build build --parallel && ./build/engine-tests && ./build/properties-tests && ./build/console-tests && ./build/studio-tests && ./build/mcp-tests && ./build/sandbox`
Expected: every binary exits 0; the sandbox prints `All tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/ide/IdeLayout.hpp src/ide/IdeLayout.cpp tests/SaveConflictTest.cpp tests/StudioLayoutTest.cpp CMakeLists.txt README.md
git commit -m "$(cat <<'EOF'
The studio asks before saving over files changed on disk

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```
