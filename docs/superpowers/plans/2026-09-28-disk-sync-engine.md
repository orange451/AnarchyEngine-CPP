# Disk Sync Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Project` can compare the disk with the place key by key, load every change only the disk made, apply a person's IDE / Disk choice per row, and report the rows still open, with Save's guard using the same per-key rule.

**Architecture:** A pure `JsonMerge` classifies each top-level key of three JSON objects: the base (`Project::files_`, now kept parsed), the disk (`PlanReader`), and the studio (`instance_json` of the authored tree). `Project::scan_disk()` builds a `Comparison` (disk plan, studio tree, the disk-only actions, the rows) without touching anything; `Project::apply_disk(choices)` runs those actions and the disk-side choices as one undo step, settles studio-side choices in the base, and refreshes the base wherever no row is left. The Milestone 1 guard switches to the same per-key rule for property files. This plan is the engine half of the Conflicts window spec; the studio half (window, ribbon, save gate, `reveal_window`, JadeFX `toFront()`) gets its own plan once this lands.

**Tech Stack:** C++17, CMake, Catch2 v3 (the `sandbox` target), the headless `studio-tests` harness.

**Spec:** `docs/superpowers/specs/2026-09-28-conflicts-window-design.md` (sections Architecture 1–7, Merge rules, Errors and edge cases, Testing).

## Global Constraints

- Branch `reload-from-disk`. Commit after each task; messages are one plain sentence, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- A property value is compared as the JSON text `write_json` produces (canonical: sorted keys, one number format). File bytes are compared only as a shortcut, and for script sources.
- A key missing from an instance file means the class default (the file leaves defaults out).
- Instance ids stay the same through an apply, except for an instance whose `class` changed, which is recreated with the same GUID.
- `apply_disk` is one undo step named "Changes from Disk", runs in edit mode only, and calls `capture_place()` when it changed anything.
- `Project`'s contract stays: every call runs on the thread that may mutate the DataModel.
- Every test in `./build/sandbox` passes after each task (180 test cases at the start), and `./build/studio-tests` exits 0.
- Comments are plain sentences that say why, as the surrounding code writes them.

## Review Focus

1. A hand edit that adds a key the engine does not know (an extra). Expect it to load as an extra and a save afterwards to write nothing. (Task 5, A13.)
2. A rename and a move of the same instance on disk at once. Expect both to load and the next save to write, move, and remove nothing. (Task 5, A5.)
3. An instance deleted on disk whose child the studio edited. Expect rows for both, and the disk's side on the parent to remove the child with it. (Task 4, D7; Task 5, A8.)
4. Undoing "Changes from Disk", then saving. Expect the save to write the studio's older values without a conflict. (Task 5, A1.)
5. An edit gesture still open when an apply runs (a property set with no waypoint yet). Expect "Changes from Disk" to be its own undo step. (Task 5, A1.)

## File Structure

- `src/engine_core/JsonMerge.hpp` / `.cpp` (new): `KeyChange`, `KeyMerge`, `merge_keys`, `same_value`, `display_value`.
- `src/engine_core/DataModel.hpp` / `.cpp`, `src/engine_instances/GameObject.*`, `TestTriangle.*`, `Script.*`: `default_properties`.
- `src/engine_core/Project.hpp` / `.cpp`: `SaveConflict` fields, `Files::props` / `Files::parent`, `detail::PlanNode`, the per-key guard, `DiskScan`, `DiskChoice`, `scan_disk`, `apply_disk`, `unsaved`.
- `sandbox/merge_tests.cpp` (new), `sandbox/project_tests.cpp`, `tests/SaveConflictTest.cpp`, `CMakeLists.txt`.

Commands (`build/` is configured):

- `cmake --build build --parallel --target sandbox && ./build/sandbox "[merge]"` (Tasks 1–2)
- `./build/sandbox "[guard]"`, `./build/sandbox "[disk]"`, `./build/sandbox "[project]"`
- `cmake --build build --parallel --target studio-tests && ./build/studio-tests`

---

### Task 1: `JsonMerge`

**Files:**
- Create: `src/engine_core/JsonMerge.hpp`, `src/engine_core/JsonMerge.cpp`
- Create: `sandbox/merge_tests.cpp`
- Modify: `CMakeLists.txt` (`engine_core` sources; `sandbox` sources)

**Interfaces:**
- Produces:
  - `enum class KeyChange { Unchanged, DiskOnly, StudioOnly, Agreed, Conflict };`
  - `struct KeyMerge { std::string key; KeyChange change = KeyChange::Unchanged; };`
  - `std::vector<KeyMerge> merge_keys(const JsonValue& base, const JsonValue& disk, const JsonValue& studio);` every key of the three objects except `id`, byte-sorted.
  - `bool same_value(const JsonValue* a, const JsonValue* b);` null means absent.
  - `std::string display_value(const JsonValue* value);`

- [ ] **Step 1: Write the failing tests**

Create `sandbox/merge_tests.cpp`:

```cpp
#include "JsonMerge.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using engine_core::JsonValue;
using engine_core::KeyChange;

namespace {

JsonValue json(const char* text) {
    JsonValue out;
    std::string error;
    REQUIRE(engine_core::parse_json(text, out, error));
    return out;
}

std::map<std::string, KeyChange> changes(const JsonValue& base, const JsonValue& disk, const JsonValue& studio) {
    std::map<std::string, KeyChange> out;
    for (const engine_core::KeyMerge& merged : engine_core::merge_keys(base, disk, studio)) {
        out[merged.key] = merged.change;
    }
    return out;
}

}  // namespace

TEST_CASE("M1 merge_keys sorts every key into how it changed", "[M1][merge]") {
    const JsonValue base = json(R"({"class": "GameObject", "id": "a", "Name": "A", "Color": [1, 0, 0],
                                   "Size": [2, 2, 2], "Gone": true, "Kept": 1})");
    const JsonValue disk = json(R"({"class": "GameObject", "id": "a", "Name": "A", "Color": [0, 0, 1],
                                   "Size": [2, 2, 2], "Kept": 2, "New": 1})");
    const JsonValue studio = json(R"({"class": "GameObject", "id": "a", "Name": "B", "Color": [0, 1, 0],
                                     "Size": [2, 2, 2], "Gone": true, "Kept": 2, "New": 1})");
    const std::map<std::string, KeyChange> expected = {
        {"class", KeyChange::Unchanged}, {"Name", KeyChange::StudioOnly}, {"Color", KeyChange::Conflict},
        {"Size", KeyChange::Unchanged},  {"Gone", KeyChange::DiskOnly},   {"Kept", KeyChange::Agreed},
        {"New", KeyChange::Agreed},
    };
    REQUIRE(changes(base, disk, studio) == expected);
}

TEST_CASE("M2 merge_keys lists keys in byte order, without id", "[M2][merge]") {
    const JsonValue same = json(R"({"id": "x", "class": "C", "a": 1, "B": 2})");
    std::vector<std::string> keys;
    for (const engine_core::KeyMerge& merged : engine_core::merge_keys(same, same, same)) {
        keys.push_back(merged.key);
    }
    REQUIRE(keys == std::vector<std::string>{"B", "a", "class"});
}

TEST_CASE("M3 same_value compares as the file writes values", "[M3][merge]") {
    const JsonValue one = json(R"({"n": [1.0, 2], "o": {"a": 1, "b": 2}})");
    const JsonValue two = json(R"({"n": [1, 2], "o": {"b": 2, "a": 1}})");
    REQUIRE(engine_core::same_value(one.find("n"), two.find("n")));
    REQUIRE(engine_core::same_value(one.find("o"), two.find("o")));
    REQUIRE(engine_core::same_value(nullptr, nullptr));
    REQUIRE_FALSE(engine_core::same_value(one.find("n"), nullptr));
    REQUIRE_FALSE(engine_core::same_value(one.find("n"), one.find("o")));
}

TEST_CASE("M4 display_value writes a value on one line", "[M4][merge]") {
    REQUIRE(engine_core::display_value(nullptr) == "(default)");
    const JsonValue color = json("[0.25, 0.5, 0.75]");
    REQUIRE(engine_core::display_value(&color) == "0.25, 0.5, 0.75");
    const JsonValue yes = JsonValue::boolean(true);
    REQUIRE(engine_core::display_value(&yes) == "true");
    const JsonValue name = JsonValue::string("Part");
    REQUIRE(engine_core::display_value(&name) == "Part");
    const JsonValue three = JsonValue::number(3);
    REQUIRE(engine_core::display_value(&three) == "3");
    const JsonValue nested = json(R"({"a": 1, "b": [1, "x"]})");
    const std::string text = engine_core::display_value(&nested);
    REQUIRE(text.find('\n') == std::string::npos);
    REQUIRE(text.front() == '{');
    REQUIRE(text.find("\"a\": 1") != std::string::npos);
}
```

In `CMakeLists.txt`, add `src/engine_core/JsonMerge.cpp` to `add_library(engine_core STATIC` right after `src/engine_core/PropertyBag.cpp`, and add `sandbox/merge_tests.cpp` to `add_executable(sandbox` right after `sandbox/project_tests.cpp`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build >/dev/null && cmake --build build --parallel --target sandbox`
Expected: the build fails: `JsonMerge.hpp` not found (and `JsonMerge.cpp` missing).

- [ ] **Step 3: Implement `JsonMerge`**

Create `src/engine_core/JsonMerge.hpp`:

```cpp
#pragma once

#include "PropertyBag.hpp"

#include <string>
#include <vector>

namespace engine_core {

// How one top-level key of an instance's file changed, across three versions:
// the last load or save (base), the disk now, and what the studio would write now.
enum class KeyChange {
    Unchanged,   // the disk and the studio both match the base
    DiskOnly,    // the disk changed it; the studio did not
    StudioOnly,  // the studio changed it; the disk did not
    Agreed,      // both changed it, to the same value
    Conflict,    // both changed it, to different values
};

struct KeyMerge {
    std::string key;
    KeyChange change = KeyChange::Unchanged;
};

// Every key of the three objects, byte-sorted, with how it changed. A key an
// object lacks has the value "absent", which for an instance file is the class
// default. "id" names the instance and is left out.
std::vector<KeyMerge> merge_keys(const JsonValue& base, const JsonValue& disk, const JsonValue& studio);

// Equal as write_json writes them. Null is absent: equal only to null.
bool same_value(const JsonValue* a, const JsonValue* b);

// A value on one line for a person: numbers as the file writes them, an array
// of numbers as "1, 0, 0", a string as its text, null (absent) as "(default)",
// and anything else as its JSON on one line.
std::string display_value(const JsonValue* value);

}  // namespace engine_core
```

Create `src/engine_core/JsonMerge.cpp`:

```cpp
#include "JsonMerge.hpp"

#include <algorithm>
#include <set>

namespace engine_core {

bool same_value(const JsonValue* a, const JsonValue* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return write_json(*a) == write_json(*b);
}

std::vector<KeyMerge> merge_keys(const JsonValue& base, const JsonValue& disk, const JsonValue& studio) {
    std::set<std::string> keys;
    for (const JsonValue* object : {&base, &disk, &studio}) {
        for (const JsonValue::Member& member : object->members()) {
            if (member.first != "id") {
                keys.insert(member.first);
            }
        }
    }
    std::vector<KeyMerge> out;
    for (const std::string& key : keys) {
        const JsonValue* was = base.find(key);
        const JsonValue* theirs = disk.find(key);
        const JsonValue* mine = studio.find(key);
        KeyMerge merged;
        merged.key = key;
        if (same_value(theirs, was)) {
            merged.change = same_value(mine, was) ? KeyChange::Unchanged : KeyChange::StudioOnly;
        } else if (same_value(mine, was)) {
            merged.change = KeyChange::DiskOnly;
        } else {
            merged.change = same_value(mine, theirs) ? KeyChange::Agreed : KeyChange::Conflict;
        }
        out.push_back(std::move(merged));
    }
    return out;
}

std::string display_value(const JsonValue* value) {
    if (value == nullptr) {
        return "(default)";
    }
    if (value->is_string()) {
        return value->as_string();
    }
    if (value->is_number()) {
        return format_json_number(value->as_number());
    }
    if (value->is_bool()) {
        return value->as_bool() ? "true" : "false";
    }
    if (value->is_array() && !value->items().empty() &&
        std::all_of(value->items().begin(), value->items().end(), [](const JsonValue& item) { return item.is_number(); })) {
        std::string out;
        for (const JsonValue& item : value->items()) {
            out += (out.empty() ? "" : ", ") + format_json_number(item.as_number());
        }
        return out;
    }
    // Anything else: its JSON, each line break and its indent folded to one space.
    const std::string text = write_json(*value);
    std::string out;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] != '\n') {
            out += text[at];
            continue;
        }
        while (at + 1 < text.size() && text[at + 1] == ' ') {
            ++at;
        }
        out += ' ';
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace engine_core
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[merge]"`
Expected: `All tests passed` (4 test cases).

Run: `./build/sandbox`
Expected: `All tests passed` (184 test cases).

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/JsonMerge.hpp src/engine_core/JsonMerge.cpp sandbox/merge_tests.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add a key-by-key merge of three versions of an instance file

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `default_properties`

**Files:**
- Modify: `src/engine_core/DataModel.hpp` (beside `save_properties`), `src/engine_core/DataModel.cpp` (after `DataModel::save_properties`)
- Modify: `src/engine_instances/GameObject.hpp` / `.cpp`, `TestTriangle.hpp` / `.cpp`, `Script.hpp` / `.cpp`
- Test: `sandbox/merge_tests.cpp` (append)

**Interfaces:**
- Produces: `virtual void DataModel::default_properties(PropertyBag& out) const;` Task 5 uses it to reset a key the disk's file no longer has.

- [ ] **Step 1: Write the failing test**

Add these includes at the top of `sandbox/merge_tests.cpp`, after `#include "JsonMerge.hpp"`:

```cpp
#include "Contract.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"
#include "types.hpp"
```

Append:

```cpp
namespace {

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

std::vector<std::string> default_keys(const engine_core::DataModel& object) {
    engine_core::PropertyBag defaults;
    object.default_properties(defaults);
    std::vector<std::string> out;
    for (const JsonValue::Member& member : defaults) {
        out.push_back(member.first);
    }
    return out;
}

}  // namespace

TEST_CASE("M5 default_properties is what save_properties leaves out", "[M5][merge]") {
    SimRole role;
    engine_core::Game game;
    std::vector<engine_core::DataModel*> objects = {
        &game.create(), &game.create<engine_core::GameObject>(), &game.create<engine_core::Script>(),
        &game.create<engine_core::ModuleScript>(), &game.create<engine_core::Folder>(),
        &game.create<engine_core::TestTriangle>()};
    for (engine_core::DataModel* object : objects) {
        INFO(object->class_name());
        engine_core::PropertyBag saved;
        object->save_properties(saved);
        REQUIRE(saved.empty());
        engine_core::PropertyBag defaults;
        object->default_properties(defaults);
        for (const JsonValue::Member& member : defaults) {
            std::string error;
            REQUIRE(object->load_property(member.first, member.second, error));
            REQUIRE(error.empty());
        }
        object->save_properties(saved);
        REQUIRE(saved.empty());
    }
    REQUIRE(default_keys(game).empty());
    REQUIRE(default_keys(*objects[0]) == std::vector<std::string>{"Simulated", "VisualOnly"});
    REQUIRE(default_keys(*objects[1]) ==
            std::vector<std::string>{"Color", "Simulated", "Size", "Transform", "VisualOnly"});
    REQUIRE(default_keys(*objects[2]) == std::vector<std::string>{"Enabled", "Simulated", "VisualOnly"});
    REQUIRE(default_keys(*objects[5]) == std::vector<std::string>{"Position", "Simulated", "VisualOnly"});
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails: no member named `default_properties`.

- [ ] **Step 3: Implement it**

`DataModel.hpp`, right after the `save_properties` declaration:

```cpp
    // The value save_properties leaves out, for every key this class owns: what
    // a key missing from its file means. The root owns none.
    virtual void default_properties(PropertyBag& out) const;
```

`DataModel.cpp`, right after `DataModel::save_properties`:

```cpp
void DataModel::default_properties(PropertyBag& out) const {
    const Slot* part = slot(id_);
    if (part == nullptr || part->instance != this) {
        return;
    }
    bag_set(out, "Simulated", JsonValue::boolean(false));
    bag_set(out, "VisualOnly", JsonValue::boolean(false));
}
```

`GameObject.hpp`, beside its `save_properties` override: `void default_properties(PropertyBag& out) const override;`. `GameObject.cpp`, after `GameObject::save_properties`:

```cpp
void GameObject::default_properties(PropertyBag& out) const {
    DataModel::default_properties(out);
    const Transform identity = transform_identity();
    bag_set(out, "Transform", json_floats(identity.m, 16));
    const ColorRgb white{};
    const float channels[3] = {white.r, white.g, white.b};
    bag_set(out, "Color", json_floats(channels, 3));
    const float one[3] = {1.f, 1.f, 1.f};
    bag_set(out, "Size", json_floats(one, 3));
}
```

`TestTriangle.hpp`, beside its `save_properties` override: `void default_properties(PropertyBag& out) const override;`. `TestTriangle.cpp`, after `TestTriangle::save_properties`:

```cpp
void TestTriangle::default_properties(PropertyBag& out) const {
    DataModel::default_properties(out);
    const float origin[3] = {0.f, 0.f, 0.f};
    bag_set(out, "Position", json_floats(origin, 3));
}
```

`Script.hpp`, beside its `save_properties` override: `void default_properties(PropertyBag& out) const override;`. `Script.cpp`, after `Script::save_properties`:

```cpp
void Script::default_properties(PropertyBag& out) const {
    LuaSource::default_properties(out);
    bag_set(out, "Enabled", JsonValue::boolean(true));
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[merge]"`
Expected: `All tests passed` (5 test cases).

Run: `./build/sandbox`
Expected: `All tests passed` (185 test cases).

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_instances/GameObject.hpp src/engine_instances/GameObject.cpp src/engine_instances/TestTriangle.hpp src/engine_instances/TestTriangle.cpp src/engine_instances/Script.hpp src/engine_instances/Script.cpp sandbox/merge_tests.cpp
git commit -m "$(cat <<'EOF'
Each class names the default of every property it saves

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: The base kept parsed, and Save's guard key by key

**Files:**
- Modify: `src/engine_core/Project.hpp` (`SaveConflict`, `Files`, `detail::PlanNode` forward declaration, `from_disk`)
- Modify: `src/engine_core/Project.cpp` (`PlanNode` into `detail`, `doc`; helpers; `load` ×2; `instance_json`; `plan_files`; `outside_changes`; `describe_conflict`)
- Test: `sandbox/project_tests.cpp` (update G2, G3, G5, G7, G16, G22, G23; add G27, G28), `tests/SaveConflictTest.cpp` (`touch`)

**Interfaces:**
- Consumes: Task 1's `merge_keys`, `KeyChange`, `display_value`.
- Produces:
  - `SaveConflict` fields `std::string key, studio, disk, name, where;`, with `operator==` over all eight fields.
  - `Project::Files` fields `JsonValue props; std::string parent;`; `static Files from_disk(const detail::PlanNode& node, std::string parent);`
  - In `Project.cpp`: `namespace detail { struct PlanNode { ... JsonValue doc; ... }; }`; anonymous-namespace templates `parents_of(nodes)` and `path_of(nodes, parents, index)`; `differing_lines(a, b)`; `JsonValue instance_json(const AuthoredNode&, const std::vector<AuthoredNode>&)`.

- [ ] **Step 1: Update the tests to edit files as another editor would**

In `sandbox/project_tests.cpp`, add inside the first anonymous namespace, after `write_bare_project`:

```cpp
// Sets one key of an instance file on disk, as another editor would.
void edit_key(const fs::path& file, const char* key, const engine_core::JsonValue& value) {
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(file), doc, error));
    doc.set(key, value);
    write_file(file, engine_core::write_json(doc));
}

engine_core::JsonValue triple(float x, float y, float z) {
    const float values[3] = {x, y, z};
    return engine_core::json_floats(values, 3);
}
```

Then change these outside edits (each is currently `write_file(<file>, read_file(<file>) + "\n")`, which the per-key guard no longer counts):

- G2: replace `write_file(dir.path / path, read_file(dir.path / path) + "\n");` with `edit_key(dir.path / path, "Size", triple(2, 2, 2));`, and after `REQUIRE(conflict.conflicts()[0].kind == engine_core::SaveConflict::Kind::EditedOutside);` add:

```cpp
        REQUIRE(conflict.conflicts()[0].key == "Size");
        REQUIRE(conflict.conflicts()[0].studio == "(default)");
        REQUIRE(conflict.conflicts()[0].disk == "2, 2, 2");
        REQUIRE(conflict.conflicts()[0].name == "A");
        REQUIRE(conflict.conflicts()[0].where == "game");
```

- G3: replace `const std::string outside = read_file(dir.path / path) + "\n";` and `write_file(dir.path / path, outside);` with `edit_key(dir.path / path, "Size", triple(2, 2, 2));` then `const std::string outside = read_file(dir.path / path);`, and after the `kind` check add `REQUIRE(conflicts[0].key.empty());`.
- G5, second section: after `REQUIRE(conflicts[0].path == luau);` add `REQUIRE(conflicts[0].key == "Source");`.
- G7: replace `write_file(dir.path / path, read_file(dir.path / path) + "\n");` with `edit_key(dir.path / path, "Size", triple(2, 2, 2));`.
- G16: replace `write_file(dir.path / path, read_file(dir.path / path) + "\n");` with `edit_key(dir.path / path, "Size", triple(2, 2, 2));`.
- G22: after `REQUIRE(conflicts.size() == 1);` add `REQUIRE(conflicts[0].key == "Name");`.
- G23: replace `const std::string a_outside = read_file(dir.path / a_path) + "\n";` and `write_file(dir.path / a_path, a_outside);` with `edit_key(dir.path / a_path, "Size", triple(2, 2, 2));` then `const std::string a_outside = read_file(dir.path / a_path);`; replace `const std::string b_outside = read_file(dir.path / b_path) + "\n";` and `write_file(dir.path / b_path, b_outside);` with `edit_key(dir.path / b_path, "Size", triple(2, 2, 2));` then `const std::string b_outside = read_file(dir.path / b_path);`.

Append:

```cpp
TEST_CASE("G27 a file only reformatted outside is no conflict", "[G27][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    std::string wide = read_file(dir.path / path);
    for (std::size_t at = wide.find("\n  "); at != std::string::npos; at = wide.find("\n  ", at + 5)) {
        wide.replace(at, 3, "\n    ");
    }
    write_file(dir.path / path, wide);

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / path).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos);
}

TEST_CASE("G28 a property file that does not parse is one row for its instance", "[G28][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    write_file(dir.path / path, "{\"class\": ");

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].key.empty());
    REQUIRE(conflicts[0].disk == "can't be read");
    project.save(conflicts);
    REQUIRE(read_file(dir.path / path).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos);
}
```

In `tests/SaveConflictTest.cpp`, add `#include "PropertyBag.hpp"` after `#include "Project.hpp"`, and replace the body of the `touch` lambda with a real property edit:

```cpp
    // Something outside the studio changes a property in a file.
    auto touch = [&root](const std::string& file) {
        engine_core::JsonValue doc;
        std::string error;
        engine_core::parse_json(ReadBytes(root / file), doc, error);
        const float size[3] = {2.f, 2.f, 2.f};
        doc.set("Size", engine_core::json_floats(size, 3));
        const std::string bytes = engine_core::write_json(doc);
        WriteBytes(root / file, bytes);
        return bytes;
    };
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails: `SaveConflict` has no member `key` (and `studio`, `disk`, `name`, `where`).

- [ ] **Step 3: Extend `SaveConflict` and `Files` in `Project.hpp`**

Before `class ProjectError`, add:

```cpp
namespace detail {
struct PlanNode;
}  // namespace detail
```

Replace the body of `struct SaveConflict` after `enum class Kind { ... };` so it reads:

```cpp
    std::string guid;
    // As last loaded or saved: relative to the project root, written with '/'.
    // An added file's path is where it is now.
    std::string path;
    Kind kind = Kind::EditedOutside;
    // What the row is about: a key of the instance's file, "Parent", "Source",
    // or "class". Empty for the whole instance.
    std::string key;
    // Each side as a person reads it: a value, or a word such as "deleted".
    std::string studio;
    std::string disk;
    // The instance's Name, and where it sits, as the explorers show them
    // ("game.Box"). Empty when neither side has it.
    std::string name;
    std::string where;
```

Replace `inline bool operator==(const SaveConflict& a, const SaveConflict& b)` with:

```cpp
inline bool operator==(const SaveConflict& a, const SaveConflict& b) {
    return a.guid == b.guid && a.path == b.path && a.kind == b.kind && a.key == b.key && a.studio == b.studio &&
           a.disk == b.disk && a.name == b.name && a.where == b.where;
}
```

In `struct Files`, after `std::string source_bytes;`, add:

```cpp
        // The properties file parsed: the base each key is compared against. A
        // key settled for the studio's side is patched here, and props_bytes
        // rewritten to match.
        JsonValue props;
        // The parent's GUID. Empty for the root.
        std::string parent;
```

After the `plan_files` declaration, add:

```cpp
    // An instance's files as the disk has them, parent its parent's GUID.
    static Files from_disk(const detail::PlanNode& node, std::string parent);
```

- [ ] **Step 4: `PlanNode` into `detail`, with its parsed file**

In `Project.cpp`, the anonymous namespace currently holds `struct PlanNode` right after `reserved_key`. Close the anonymous namespace before it, define it in `detail` with a new `doc` field, and reopen:

```cpp
}  // namespace

namespace detail {

// One instance read from disk, before anything is created.
struct PlanNode {
    std::string guid;
    std::string class_name;
    std::string name;
    PropertyBag properties;
    bool has_order = false;
    std::vector<std::string> order;
    bool has_source = false;
    std::string source;
    std::string props_path;
    std::string props_bytes;
    std::string source_path;
    std::string source_bytes;
    std::vector<std::size_t> children;
    // The properties file as parsed.
    JsonValue doc;
};

}  // namespace detail

namespace {

using detail::PlanNode;
```

In `PlanReader::read_props`, after `node.name = name->as_string();`, add `node.doc = doc;`.

After `guid_claims` (and `own_folder`), add:

```cpp
// Each node's parent, by index. Parents come before children.
template <typename Node>
std::vector<std::size_t> parents_of(const std::vector<Node>& nodes) {
    std::vector<std::size_t> out(nodes.size(), 0);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        for (std::size_t child : nodes[index].children) {
            out[child] = index;
        }
    }
    return out;
}

// "game.Box.Part": the Names from the root down to index. The root is "game".
template <typename Node>
std::string path_of(const std::vector<Node>& nodes, const std::vector<std::size_t>& parents, std::size_t index) {
    std::vector<const std::string*> names;
    for (std::size_t at = index; at != 0; at = parents[at]) {
        names.push_back(&nodes[at].name);
    }
    std::string out = "game";
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        out += "." + **it;
    }
    return out;
}

// The first line where a and b differ, from each, as "2: print(1)".
std::pair<std::string, std::string> differing_lines(const std::string& a, const std::string& b) {
    std::istringstream left(a);
    std::istringstream right(b);
    std::string one;
    std::string two;
    for (int line = 1;; ++line) {
        const bool more_left = static_cast<bool>(std::getline(left, one));
        const bool more_right = static_cast<bool>(std::getline(right, two));
        if (!more_left && !more_right) {
            // Only line endings differ.
            return {"(line endings)", "(line endings)"};
        }
        if (more_left != more_right || one != two) {
            const std::string at = std::to_string(line) + ": ";
            return {more_left ? at + one : "(no line " + std::to_string(line) + ")",
                    more_right ? at + two : "(no line " + std::to_string(line) + ")"};
        }
    }
}
```

Add `#include "JsonMerge.hpp"` after `#include "GameObject.hpp"`.

- [ ] **Step 5: The base keeps the parsed file and the parent**

Split `instance_bytes` into a JSON builder and a writer. Replace the whole function with:

```cpp
JsonValue instance_json(const AuthoredNode& node, const std::vector<AuthoredNode>& tree) {
    JsonValue doc = JsonValue::object();
    doc.set("class", JsonValue::string(node.class_name));
    doc.set("id", JsonValue::string(node.guid));
    doc.set("Name", JsonValue::string(node.name));
    for (const JsonValue::Member& member : node.properties) {
        if (reserved_key(member.first)) {
            fail("instance " + node.guid + ": property \"" + member.first + "\" is reserved");
        }
        doc.set(member.first, member.second);
    }
    // Written only when the order is not the default GUID sort.
    std::vector<std::string> order;
    order.reserve(node.children.size());
    for (std::size_t child : node.children) {
        order.push_back(tree[child].guid);
    }
    if (!std::is_sorted(order.begin(), order.end())) {
        std::vector<JsonValue> items;
        for (std::string& guid : order) {
            items.push_back(JsonValue::string(std::move(guid)));
        }
        doc.set("children", JsonValue::array(std::move(items)));
    }
    return doc;
}

std::string instance_bytes(const JsonValue& doc, const AuthoredNode& node) {
    try {
        return write_json(doc);
    } catch (const std::invalid_argument&) {
        fail("instance " + node.guid + " (" + node.name + ") has a non-finite number");
    }
}
```

After `Project::instance_for`, add:

```cpp
Project::Files Project::from_disk(const detail::PlanNode& node, std::string parent) {
    Files files;
    files.props_path = node.props_path;
    files.props_bytes = node.props_bytes;
    files.has_source = node.has_source;
    files.source_path = node.source_path;
    files.source_bytes = node.source_bytes;
    files.props = node.doc;
    files.parent = std::move(parent);
    return files;
}
```

In both `Project::load` overloads, replace the loop that fills `files_`:

```cpp
    const std::vector<std::size_t> parents = parents_of(plan);
    for (std::size_t index = 0; index < plan.size(); ++index) {
        const PlanNode& node = plan[index];
        project.files_[node.guid] = from_disk(node, index == 0 ? std::string() : plan[parents[index]].guid);
        project.id_guid_[ids[index]] = node.guid;
        project.guid_id_[node.guid] = ids[index];
    }
```

In `Project::plan_files`, add `const std::vector<std::size_t> parents = parents_of(tree);` after the `if (tree.empty())` return, and replace the block from `if (node.has_properties) {` to its closing `}` (the `else` included) with:

```cpp
        files.parent = index == 0 ? std::string() : tree[parents[index]].guid;
        if (node.has_properties) {
            files.props = instance_json(node, tree);
            files.props_bytes = instance_bytes(files.props, node);
            files.source_bytes = node.source;
        } else {
            const Files& known = cache.at(node.guid);
            files.props = known.props;
            files.props_bytes = known.props_bytes;
            files.source_bytes = known.source_bytes;
        }
```

- [ ] **Step 6: The guard, key by key**

In `describe_conflict`, replace the `EditedOutside` case with:

```cpp
    case SaveConflict::Kind::EditedOutside:
        return conflict.key.empty() ? conflict.path + " changed on disk"
                                    : conflict.path + ": " + conflict.key + " changed on disk";
```

In `Project::outside_changes`, replace the block that computes `parent` (`std::vector<std::size_t> parent(tree.size(), 0);` and its loop) with:

```cpp
    const std::vector<std::size_t> parent = parents_of(tree);
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t index = 0; index < tree.size(); ++index) {
        index_of.emplace(tree[index].guid, index);
    }
    // A row names its instance as the explorers do: its Name, and where it sits.
    auto row = [&](const std::string& guid, const std::string& path, SaveConflict::Kind kind) {
        SaveConflict out;
        out.guid = guid;
        out.path = path;
        out.kind = kind;
        if (const auto live = index_of.find(guid); live != index_of.end()) {
            out.name = tree[live->second].name;
            out.where = live->second == 0 ? std::string() : path_of(tree, parent, parent[live->second]);
        } else if (const auto base = files_.find(guid); base != files_.end()) {
            if (const JsonValue* name = base->second.props.find("Name"); name != nullptr && name->is_string()) {
                out.name = name->as_string();
            }
        }
        return out;
    };
```

Change every other `conflicts.push_back({...})` in the function to use `row`:
- `conflicts.push_back({guid, *gone, elsewhere ? SaveConflict::Kind::MovedOutside : SaveConflict::Kind::DeletedOutside});` becomes `conflicts.push_back(row(guid, *gone, elsewhere ? SaveConflict::Kind::MovedOutside : SaveConflict::Kind::DeletedOutside));`
- in the left-gone conversion loop, `conflicts.push_back({guid, gone->second, moved(...) ? ... : ...});` becomes `conflicts.push_back(row(guid, gone->second, moved(guid, files_.at(guid)) ? SaveConflict::Kind::MovedOutside : SaveConflict::Kind::DeletedOutside));`
- in the added-files loop, `conflicts.push_back({claimed, path, SaveConflict::Kind::AddedOutside});` becomes `conflicts.push_back(row(claimed, path, SaveConflict::Kind::AddedOutside));`

Replace the final edit check in the `files_` loop:

```cpp
        for (const Own& file : own) {
            if (file.rewritten && read_file(disk_path(root_, *file.path)) != *file.bytes) {
                conflicts.push_back(row(guid, *file.path, SaveConflict::Kind::EditedOutside));
                break;
            }
        }
```

(or its `{guid, *file.path, SaveConflict::Kind::EditedOutside}` form) with:

```cpp
        // What the save rewrites or deletes, against the disk: the properties
        // key by key, so formatting alone is no change, and a script's source
        // whole. An instance the studio deleted is one row for all of it.
        std::vector<SaveConflict> edited;
        auto whole = [&](const std::string& path) {
            SaveConflict out = row(guid, path, SaveConflict::Kind::EditedOutside);
            out.studio = "deleted";
            out.disk = "changed on disk";
            return out;
        };
        if (own[0].rewritten) {
            const std::string bytes = read_file(disk_path(root_, base.props_path));
            JsonValue disk;
            std::string error;
            if (bytes == base.props_bytes) {
                // Unchanged on disk.
            } else if (!parse_json(bytes, disk, error) || !disk.is_object()) {
                SaveConflict unreadable = row(guid, base.props_path, SaveConflict::Kind::EditedOutside);
                unreadable.disk = "can't be read";
                edited.push_back(std::move(unreadable));
            } else {
                const JsonValue none = JsonValue::object();
                const JsonValue& mine = removed ? none : planned->second.props;
                for (const KeyMerge& merged : merge_keys(base.props, disk, mine)) {
                    // The save would write over a value that changed on disk.
                    if (merged.change != KeyChange::DiskOnly && merged.change != KeyChange::Conflict) {
                        continue;
                    }
                    if (removed) {
                        edited.push_back(whole(base.props_path));
                        break;
                    }
                    SaveConflict key = row(guid, base.props_path, SaveConflict::Kind::EditedOutside);
                    key.key = merged.key;
                    key.studio = display_value(mine.find(merged.key));
                    key.disk = display_value(disk.find(merged.key));
                    edited.push_back(std::move(key));
                }
            }
        }
        if (own.size() > 1 && own[1].rewritten) {
            const std::string text = read_file(disk_path(root_, base.source_path));
            if (text != base.source_bytes && (removed || text != planned->second.source_bytes)) {
                if (removed) {
                    if (edited.empty()) {
                        edited.push_back(whole(base.props_path));
                    }
                } else {
                    SaveConflict source = row(guid, base.source_path, SaveConflict::Kind::EditedOutside);
                    source.key = "Source";
                    std::tie(source.studio, source.disk) = differing_lines(planned->second.source_bytes, text);
                    edited.push_back(std::move(source));
                }
            }
        }
        conflicts.insert(conflicts.end(), edited.begin(), edited.end());
```

Change the final sort to order by path, then key:

```cpp
    std::sort(conflicts.begin(), conflicts.end(), [](const SaveConflict& a, const SaveConflict& b) {
        return std::tie(a.path, a.key) < std::tie(b.path, b.key);
    });
```

Add `#include <tuple>` to `Project.cpp`'s standard includes.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox studio-tests && ./build/sandbox "[guard]"`
Expected: `All tests passed` (28 test cases).

Run: `./build/sandbox && ./build/studio-tests; echo "studio exit $?"`
Expected: `All tests passed` (187 test cases) and `studio exit 0`.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_tests.cpp tests/SaveConflictTest.cpp
git commit -m "$(cat <<'EOF'
Save's guard compares property files key by key

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: `scan_disk`

**Files:**
- Modify: `src/engine_core/Project.hpp` (`DiskScan`, `scan_disk`, private `Comparison` / `compare_disk`)
- Modify: `src/engine_core/Project.cpp` (helpers; `Project::Comparison`; `compare_disk`; `scan_disk`)
- Test: `sandbox/project_tests.cpp` (append D1–D13)

**Interfaces:**
- Consumes: Task 1 (`merge_keys`, `same_value`, `display_value`), Task 3 (`Files::props`/`parent`, `detail::PlanNode::doc`, `parents_of`, `path_of`, `differing_lines`, `instance_json`, `SaveConflict` fields).
- Produces (public): `struct DiskScan { std::vector<std::string> loaded; std::vector<SaveConflict> skipped; std::vector<SaveConflict> conflicts; bool has_disk_changes = false; };` and `DiskScan Project::scan_disk() const;` (throws `ProjectError` when `src/` does not read, or a class rejects a value).
- Produces (private, for Task 5): `struct Project::Comparison` with `Action { enum class Type { Create, Restore, Recreate, Set, Destroy }; Type type; std::string guid; std::string key; }`, `plan`, `plan_parents`, `on_disk`, `tree`, `tree_parents`, `in_studio`, `actions`, `rows`, and `disk_parent(index)`; `Comparison compare_disk() const;`

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/project_tests.cpp`:

```cpp
namespace {

const engine_core::SaveConflict& only_row(const engine_core::DiskScan& scan) {
    REQUIRE(scan.conflicts.size() == 1);
    return scan.conflicts[0];
}

std::string box_dir(const DataModel& game, InstanceId box) { return "src/Box." + game.guid(box); }

}  // namespace

TEST_CASE("D1 a scan with nothing changed finds nothing", "[D1][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    add_part(game, 0, "A");
    add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE_FALSE(scan.has_disk_changes);
}

TEST_CASE("D2 a property only the disk changed is a change to load, not a row", "[D2][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Size", triple(2, 2, 2));
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE(scan.has_disk_changes);
}

TEST_CASE("D3 a property both sides changed differently is a row", "[D3][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    engine_core::GameObject& a = add_part(game, box, "A");
    project.save();
    const std::string path = box_dir(game, box) + "/A." + game.guid(a.id()) + ".json";
    edit_key(dir.path / path, "Color", triple(0, 0, 1));
    a.set_color(rgb(1.f, 0.f, 0.f));
    const engine_core::SaveConflict& row = only_row(project.scan_disk());
    REQUIRE(row.guid == game.guid(a.id()));
    REQUIRE(row.path == path);
    REQUIRE(row.kind == engine_core::SaveConflict::Kind::EditedOutside);
    REQUIRE(row.key == "Color");
    REQUIRE(row.studio == "1, 0, 0");
    REQUIRE(row.disk == "0, 0, 1");
    REQUIRE(row.name == "A");
    REQUIRE(row.where == "game.Box");
}

TEST_CASE("D4 a property both sides changed the same way is nothing", "[D4][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Color", triple(0, 0, 1));
    a.set_color(rgb(0.f, 0.f, 1.f));
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE_FALSE(scan.has_disk_changes);
}

TEST_CASE("D5 a move on disk is a Parent change", "[D5][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    const InstanceId q = add_part(game, 0, "Q").id();
    add_part(game, q, "QKeep");
    const InstanceId loose = add_part(game, 0, "Loose").id();
    project.save();
    fs::rename(dir.path / leaf(game, loose),
               dir.path / box_dir(game, box) / ("Loose." + game.guid(loose) + ".json"));

    SECTION("the studio left it alone: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("the studio moved it too: a row") {
        game.set_parent(loose, q);
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.key == "Parent");
        REQUIRE(row.studio == "game.Q");
        REQUIRE(row.disk == "game.Box");
    }
}

TEST_CASE("D6 a source both sides changed is a row showing the first differing line", "[D6][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\nprint(2)\n");
    project.save();
    write_file(dir.path / leaf(game, main.id(), ".luau"), "print(1)\nprint(\"disk\")\n");
    main.set_source("print(1)\nprint(\"studio\")\n");
    const engine_core::SaveConflict& row = only_row(project.scan_disk());
    REQUIRE(row.key == "Source");
    REQUIRE(row.studio == "2: print(\"studio\")");
    REQUIRE(row.disk == "2: print(\"disk\")");
}

TEST_CASE("D7 an instance deleted on disk", "[D7][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    add_part(game, box, "Other");
    project.save();
    fs::remove_all(dir.path / box_dir(game, box));

    SECTION("the studio left it alone: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("the studio changed something inside it: rows for it and for the child") {
        game.game_object(keep)->set_color(rgb(1.f, 0.f, 0.f));
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.size() == 2);
        std::set<std::string> names;
        for (const engine_core::SaveConflict& row : scan.conflicts) {
            REQUIRE(row.kind == engine_core::SaveConflict::Kind::DeletedOutside);
            REQUIRE(row.key.empty());
            REQUIRE(row.studio == "changed in the studio");
            REQUIRE(row.disk == "deleted");
            names.insert(row.name);
        }
        REQUIRE(names == std::set<std::string>{"Box", "Keep"});
    }
}

TEST_CASE("D8 an instance deleted in the studio", "[D8][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    const std::string path = leaf(game, a);

    SECTION("the disk left it alone: nothing, a save removes it") {
        game.destroy(a);
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE_FALSE(scan.has_disk_changes);
    }
    SECTION("the disk changed it: a row") {
        edit_key(dir.path / path, "Size", triple(2, 2, 2));
        game.destroy(a);
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.kind == engine_core::SaveConflict::Kind::EditedOutside);
        REQUIRE(row.key.empty());
        REQUIRE(row.studio == "deleted");
        REQUIRE(row.disk == "changed on disk");
        REQUIRE(row.name == "A");
    }
}

TEST_CASE("D9 a class changed on disk", "[D9][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "class", engine_core::JsonValue::string("Folder"));

    SECTION("the studio left it alone: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("the studio changed it: a row") {
        a.set_color(rgb(1.f, 0.f, 0.f));
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.key == "class");
        REQUIRE(row.studio == "GameObject");
        REQUIRE(row.disk == "Folder");
    }
}

TEST_CASE("D10 an instance added on disk", "[D10][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    write_file(dir.path / box_dir(game, box) / "Added.added-0001.json", meta("Folder", "added-0001", "Added"));

    SECTION("under an instance the studio has: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("under one the studio deleted: a row for that one") {
        const std::string box_guid = game.guid(box);
        game.destroy(keep);
        game.destroy(box);
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.guid == box_guid);
        REQUIRE(row.key.empty());
        REQUIRE(row.disk == "changed on disk");
    }
}

TEST_CASE("D11 a value a class rejects makes the scan throw", "[D11][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Color", engine_core::JsonValue::string("red"));
    REQUIRE_THROWS_AS(project.scan_disk(), ProjectError);
}

TEST_CASE("D12 a file only reformatted is no change", "[D12][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const std::string path = leaf(game, a.id());
    std::string wide = read_file(dir.path / path);
    for (std::size_t at = wide.find("\n  "); at != std::string::npos; at = wide.find("\n  ", at + 5)) {
        wide.replace(at, 3, "\n    ");
    }
    write_file(dir.path / path, wide);
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE_FALSE(scan.has_disk_changes);
}

TEST_CASE("D13 a key removed on disk is a change to load", "[D13][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const fs::path file = dir.path / leaf(game, a.id());
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(file), doc, error));
    REQUIRE(doc.erase("Color"));
    write_file(file, engine_core::write_json(doc));
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE(scan.has_disk_changes);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails: no type `DiskScan`, no member `scan_disk`.

- [ ] **Step 3: Declare it in `Project.hpp`**

After `class ProjectConflict`, add:

```cpp
// What the disk holds that the place does not, against the last load or save.
struct DiskScan {
    // Instances apply_disk changed from the disk, by Name. Empty from scan_disk.
    std::vector<std::string> loaded;
    // Choices apply_disk did not apply: their row changed since it was listed.
    std::vector<SaveConflict> skipped;
    // Rows that need a person's choice, by where, name, then key.
    std::vector<SaveConflict> conflicts;
    // True when the disk has changes the studio did not make, which apply_disk loads.
    bool has_disk_changes = false;
};
```

In `class Project`, after `unsaved`'s future place (right after `place_fingerprint`'s declaration), add:

```cpp
    // Reads src/ and compares it, key by key, with the last load or save and
    // with the place. Touches nothing. Throws ProjectError when src/ does not
    // read as a project, or holds a value its class rejects.
    DiskScan scan_disk() const;
```

In the private section, after `outside_changes`, add:

```cpp
    // Base, disk, and studio side by side, and what differs. Defined in Project.cpp.
    struct Comparison;
    Comparison compare_disk() const;
```

- [ ] **Step 4: Implement the comparison**

In `Project.cpp`, add `#include <functional>` to the standard includes.

In the anonymous namespace, after `differing_lines`, add:

```cpp
// An instance's file as merge_keys compares it: with its parent's GUID as
// "Parent" and a script's text as "Source". The root has no parent.
JsonValue compared_json(JsonValue props, const std::string& parent, bool root, bool has_source,
                        const std::string& source) {
    if (!root) {
        props.set("Parent", JsonValue::string(parent));
    }
    if (has_source) {
        props.set("Source", JsonValue::string(source));
    }
    return props;
}

// b differs from a in some key.
bool differs(const JsonValue& a, const JsonValue& b) {
    for (const KeyMerge& merged : merge_keys(a, a, b)) {
        if (merged.change == KeyChange::StudioOnly) {
            return true;
        }
    }
    return false;
}
```

Right before `void Project::save_tree`, add:

```cpp
struct Project::Comparison {
    struct Action {
        enum class Type { Create, Restore, Recreate, Set, Destroy };
        Type type = Type::Set;
        std::string guid;
        // Set: the key, "Parent", "Source", or "children".
        std::string key;
    };
    std::vector<detail::PlanNode> plan;
    std::vector<std::size_t> plan_parents;
    std::unordered_map<std::string, std::size_t> on_disk;
    std::vector<AuthoredNode> tree;
    std::vector<std::size_t> tree_parents;
    std::unordered_map<std::string, std::size_t> in_studio;
    // What only the disk changed, in the order to apply it.
    std::vector<Action> actions;
    std::vector<SaveConflict> rows;

    std::string disk_parent(std::size_t index) const {
        return index == 0 ? std::string() : plan[plan_parents[index]].guid;
    }
    std::string studio_parent(std::size_t index) const {
        return index == 0 ? std::string() : tree[tree_parents[index]].guid;
    }
};

Project::Comparison Project::compare_disk() const {
    using Type = Comparison::Action::Type;
    Layout layout;
    std::string ignored;
    read_project_json(root_, ignored, layout);
    Comparison out;
    out.plan = PlanReader(root_, layout).read();
    {
        // A value a class rejects, such as a Color that is not numbers, stops
        // the scan here rather than halfway through an apply.
        Game scratch;
        scratch.history().set_enabled(false);
        build(scratch, out.plan);
    }
    out.plan_parents = parents_of(out.plan);
    for (std::size_t index = 0; index < out.plan.size(); ++index) {
        out.on_disk.emplace(out.plan[index].guid, index);
    }
    out.tree = game_->authored_tree(nullptr);
    out.tree_parents = parents_of(out.tree);
    for (std::size_t index = 0; index < out.tree.size(); ++index) {
        out.in_studio.emplace(out.tree[index].guid, index);
    }

    auto base_json = [&](const Files& base) {
        return compared_json(base.props, base.parent, base.parent.empty(), base.has_source, base.source_bytes);
    };
    auto disk_json = [&](std::size_t index) {
        const PlanNode& node = out.plan[index];
        return compared_json(node.doc, out.disk_parent(index), index == 0, node.has_source, node.source);
    };
    auto studio_json = [&](std::size_t index) {
        const AuthoredNode& node = out.tree[index];
        return compared_json(instance_json(node, out.tree), out.studio_parent(index), index == 0, node.has_source,
                             node.source);
    };
    // The studio changed index, or anything under it, since the base.
    std::function<bool(std::size_t)> studio_below = [&](std::size_t index) {
        const AuthoredNode& node = out.tree[index];
        const auto base = files_.find(node.guid);
        if (base == files_.end() || differs(base_json(base->second), studio_json(index))) {
            return true;
        }
        return std::any_of(node.children.begin(), node.children.end(), studio_below);
    };
    // The disk changed index, or added or changed anything under it.
    std::function<bool(std::size_t)> disk_below = [&](std::size_t index) {
        const PlanNode& node = out.plan[index];
        const auto base = files_.find(node.guid);
        if (base == files_.end() || differs(base_json(base->second), disk_json(index))) {
            return true;
        }
        return std::any_of(node.children.begin(), node.children.end(), disk_below);
    };
    auto add_row = [&](const std::string& guid, const std::string& path, SaveConflict::Kind kind, std::string key,
                       std::string studio, std::string disk) {
        SaveConflict row;
        row.guid = guid;
        row.path = path;
        row.kind = kind;
        row.key = std::move(key);
        row.studio = std::move(studio);
        row.disk = std::move(disk);
        if (const auto live = out.in_studio.find(guid); live != out.in_studio.end()) {
            row.name = out.tree[live->second].name;
            row.where = live->second == 0 ? std::string() : path_of(out.tree, out.tree_parents, out.tree_parents[live->second]);
        } else if (const auto file = out.on_disk.find(guid); file != out.on_disk.end()) {
            row.name = out.plan[file->second].name;
            row.where = file->second == 0 ? std::string() : path_of(out.plan, out.plan_parents, out.plan_parents[file->second]);
        }
        out.rows.push_back(std::move(row));
    };
    // A parent's GUID as the path of that parent on one side.
    auto parent_text = [&](bool studio_side, const std::string& guid) {
        if (studio_side) {
            const auto found = out.in_studio.find(guid);
            return found == out.in_studio.end() ? guid : path_of(out.tree, out.tree_parents, found->second);
        }
        const auto found = out.on_disk.find(guid);
        return found == out.on_disk.end() ? guid : path_of(out.plan, out.plan_parents, found->second);
    };
    auto key_row = [&](const std::string& guid, const std::string& path, const std::string& key,
                       const JsonValue& mine, const JsonValue& disk) {
        const JsonValue* studio = mine.find(key);
        const JsonValue* theirs = disk.find(key);
        std::string studio_text;
        std::string disk_text;
        if (key == "Parent") {
            studio_text = parent_text(true, studio != nullptr ? studio->as_string() : std::string());
            disk_text = parent_text(false, theirs != nullptr ? theirs->as_string() : std::string());
        } else if (key == "Source") {
            std::tie(studio_text, disk_text) = differing_lines(studio != nullptr ? studio->as_string() : std::string(),
                                                               theirs != nullptr ? theirs->as_string() : std::string());
        } else {
            studio_text = display_value(studio);
            disk_text = display_value(theirs);
        }
        add_row(guid, path, SaveConflict::Kind::EditedOutside, key, studio_text, disk_text);
    };
    auto act = [&](Type type, const std::string& guid, std::string key = {}) {
        out.actions.push_back({type, guid, std::move(key)});
    };

    // Everything on disk, parents before children.
    std::vector<bool> creatable(out.plan.size(), false);
    for (std::size_t index = 0; index < out.plan.size(); ++index) {
        const PlanNode& node = out.plan[index];
        const auto base = files_.find(node.guid);
        const auto studio = out.in_studio.find(node.guid);
        const bool in_base = base != files_.end();
        const bool live = studio != out.in_studio.end();
        if (!in_base && !live) {
            // Added on disk: made under its disk parent when that parent is in
            // the studio, or is made too. Under one the studio deleted, that
            // parent's row carries it.
            const std::string parent = out.disk_parent(index);
            creatable[index] = out.in_studio.count(parent) != 0 ||
                               (files_.count(parent) == 0 && creatable[out.plan_parents[index]]);
            if (creatable[index]) {
                act(Type::Create, node.guid);
            }
            continue;
        }
        if (!live) {
            // The studio deleted it. A save removes it, unless the disk changed
            // it or added under it.
            if (disk_below(index)) {
                add_row(node.guid, base->second.props_path, SaveConflict::Kind::EditedOutside, "", "deleted",
                        "changed on disk");
            }
            continue;
        }
        const JsonValue disk = disk_json(index);
        const JsonValue mine = studio_json(studio->second);
        const JsonValue was = in_base ? base_json(base->second) : JsonValue::object();
        const std::string path = in_base ? base->second.props_path : node.props_path;
        // A new class is a new instance: taken whole, or kept whole.
        if (index != 0 && in_base && !same_value(was.find("class"), disk.find("class"))) {
            if (differs(was, mine)) {
                add_row(node.guid, path, SaveConflict::Kind::EditedOutside, "class", display_value(mine.find("class")),
                        display_value(disk.find("class")));
            } else {
                act(Type::Recreate, node.guid);
            }
            continue;
        }
        for (const KeyMerge& merged : merge_keys(was, disk, mine)) {
            if (merged.key == "class") {
                continue;
            }
            if (merged.change == KeyChange::DiskOnly) {
                act(Type::Set, node.guid, merged.key);
            } else if (merged.change == KeyChange::Conflict) {
                key_row(node.guid, path, merged.key, mine, disk);
            }
        }
    }
    // Gone from disk.
    for (const auto& [guid, files] : files_) {
        if (out.on_disk.count(guid) != 0) {
            continue;
        }
        const auto studio = out.in_studio.find(guid);
        if (studio == out.in_studio.end()) {
            continue;  // Deleted on both sides.
        }
        if (studio_below(studio->second)) {
            add_row(guid, files.props_path, SaveConflict::Kind::DeletedOutside, "", "changed in the studio", "deleted");
        } else {
            act(Type::Destroy, guid);
        }
    }
    std::sort(out.rows.begin(), out.rows.end(), [](const SaveConflict& a, const SaveConflict& b) {
        return std::tie(a.where, a.name, a.guid, a.key) < std::tie(b.where, b.name, b.guid, b.key);
    });
    return out;
}

DiskScan Project::scan_disk() const {
    Comparison compared = compare_disk();
    DiskScan out;
    out.conflicts = std::move(compared.rows);
    out.has_disk_changes = !compared.actions.empty();
    return out;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[disk]"`
Expected: `All tests passed` (13 test cases).

Run: `./build/sandbox`
Expected: `All tests passed` (200 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_tests.cpp
git commit -m "$(cat <<'EOF'
Compare the disk with the place key by key

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: `apply_disk`

**Files:**
- Modify: `src/engine_core/Project.hpp` (`DiskChoice`, `apply_disk`, private `settle`, `apply_changes`, `refresh_base`)
- Modify: `src/engine_core/Project.cpp` (helpers `create_from`, `set_key`, `order_children_as`; the members)
- Test: `sandbox/project_tests.cpp` (append A1–A15)

**Interfaces:**
- Consumes: Task 2's `default_properties`; Task 4's `Comparison`, `compare_disk`, `DiskScan`.
- Produces: `struct DiskChoice { SaveConflict conflict; bool disk = false; };` and `DiskScan Project::apply_disk(const std::vector<DiskChoice>& choices = {});` (throws `ProjectError` during a test, or when `src/` does not read).

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/project_tests.cpp`:

```cpp
namespace {

bool has_key(const DataModel& object, const char* key) {
    engine_core::PropertyBag bag;
    object.save_properties(bag);
    return engine_core::bag_find(bag, key) != nullptr;
}

}  // namespace

TEST_CASE("A1 disk changes load as one undo step; undone, a save writes the studio's values", "[A1][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    edit_key(dir.path / leaf(game, a), "Size", triple(2, 2, 2));
    // A studio edit whose gesture is still open.
    b.set_color(rgb(1.f, 0.f, 0.f));

    const engine_core::DiskScan result = project.apply_disk();
    REQUIRE(result.loaded == std::vector<std::string>{"A"});
    REQUIRE(result.conflicts.empty());
    REQUIRE(has_key(*game.instance(a), "Size"));
    REQUIRE_FALSE(project.scan_disk().has_disk_changes);

    game.history().undo();
    REQUIRE_FALSE(has_key(*game.instance(a), "Size"));
    REQUIRE(b.color().r == 1.f);
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / leaf(game, a)).find("Size") == std::string::npos);
}

TEST_CASE("A2 each side of a row", "[A2][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Color", triple(0, 0, 1));
    edit_key(dir.path / leaf(game, b.id()), "Color", triple(0, 0, 1));
    a.set_color(rgb(1.f, 0.f, 0.f));
    b.set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> rows = project.scan_disk().conflicts;
    REQUIRE(rows.size() == 2);
    std::vector<engine_core::DiskChoice> choices;
    for (const engine_core::SaveConflict& row : rows) {
        choices.push_back({row, row.name == "A"});
    }

    const engine_core::DiskScan result = project.apply_disk(choices);
    REQUIRE(result.conflicts.empty());
    REQUIRE(result.skipped.empty());
    REQUIRE(a.color().b == 1.f);
    REQUIRE(b.color().r == 1.f);
    // B's studio value is settled: a save writes it over the disk's.
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / leaf(game, b.id())).find("\"Color\": [1, 0, 0]") != std::string::npos);
    REQUIRE(read_file(dir.path / leaf(game, a.id())).find("\"Color\": [0, 0, 1]") != std::string::npos);
}

TEST_CASE("A3 a row that changed after it was listed is skipped and listed again", "[A3][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const fs::path file = dir.path / leaf(game, a.id());
    edit_key(file, "Color", triple(0, 0, 1));
    a.set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> listed = project.scan_disk().conflicts;
    REQUIRE(listed.size() == 1);
    edit_key(file, "Color", triple(0, 1, 0));

    const engine_core::DiskScan result = project.apply_disk({{listed[0], true}});
    REQUIRE(result.skipped == listed);
    REQUIRE(result.conflicts.size() == 1);
    REQUIRE(result.conflicts[0].disk == "0, 1, 0");
    REQUIRE(a.color().r == 1.f);
}

TEST_CASE("A4 a loaded property keeps the instance's id", "[A4][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const std::string guid = game.guid(a);
    project.save();
    edit_key(dir.path / leaf(game, a), "Size", triple(2, 2, 2));
    project.apply_disk();
    REQUIRE(game.find_guid(guid) == a);
}

TEST_CASE("A5 a rename and a move on disk load together, and the next save is quiet", "[A5][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    const InstanceId loose = add_part(game, 0, "Loose").id();
    project.save();
    const fs::path from = dir.path / leaf(game, loose);
    const fs::path to = dir.path / box_dir(game, box) / ("Tight." + game.guid(loose) + ".json");
    fs::rename(from, to);
    edit_key(to, "Name", engine_core::JsonValue::string("Tight"));

    const engine_core::DiskScan result = project.apply_disk();
    REQUIRE(result.conflicts.empty());
    REQUIRE(game.name(loose) == "Tight");
    REQUIRE(game.parent(loose) == box);
    const auto before = tree_files(dir.path);
    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE(project.last_save().moved.empty());
    REQUIRE(project.last_save().removed.empty());
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("A6 an instance added on disk is made with its GUID, and the next save is quiet", "[A6][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    project.save();
    write_file(dir.path / box_dir(game, box) / "Added.added-0001.json", meta("Folder", "added-0001", "Added"));

    const engine_core::DiskScan result = project.apply_disk();
    REQUIRE(result.loaded == std::vector<std::string>{"Added"});
    const std::optional<InstanceId> added = game.find_guid("added-0001");
    REQUIRE(added.has_value());
    REQUIRE(game.parent(*added) == box);
    REQUIRE(std::string(game.instance(*added)->class_name()) == "Folder");
    project.save();
    REQUIRE(project.last_save().written.empty());
}

TEST_CASE("A7 an instance deleted on disk is destroyed, and one undo brings it back", "[A7][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const std::string guid = game.guid(a);
    project.save();
    fs::remove(dir.path / leaf(game, a));

    project.apply_disk();
    REQUIRE_FALSE(game.find_guid(guid).has_value());
    game.history().undo();
    REQUIRE(game.find_guid(guid).has_value());
}

TEST_CASE("A8 rows for an instance deleted on disk", "[A8][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    add_part(game, box, "Other");
    project.save();
    const std::string box_guid = game.guid(box);
    const std::string keep_guid = game.guid(keep);
    fs::remove_all(dir.path / box_dir(game, box));
    game.game_object(keep)->set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> rows = project.scan_disk().conflicts;
    REQUIRE(rows.size() == 2);

    SECTION("the studio's side keeps them, and a save writes them back") {
        std::vector<engine_core::DiskChoice> choices;
        for (const engine_core::SaveConflict& row : rows) {
            choices.push_back({row, false});
        }
        REQUIRE(project.apply_disk(choices).conflicts.empty());
        REQUIRE(save_conflicts(project).empty());
        Project loaded = Project::load(dir.path);
        REQUIRE(loaded.datamodel().find_guid(keep_guid).has_value());
    }
    SECTION("the disk's side on the parent removes the child with it") {
        const auto parent_row = std::find_if(rows.begin(), rows.end(),
                                             [&](const engine_core::SaveConflict& row) { return row.guid == box_guid; });
        REQUIRE(parent_row != rows.end());
        REQUIRE(project.apply_disk({{*parent_row, true}}).conflicts.empty());
        REQUIRE_FALSE(game.find_guid(box_guid).has_value());
        REQUIRE_FALSE(game.find_guid(keep_guid).has_value());
    }
}

TEST_CASE("A9 a row for an instance deleted in the studio and changed on disk", "[A9][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    const std::string box_guid = game.guid(box);
    const std::string keep_guid = game.guid(keep);
    const std::string folder = box_dir(game, box);
    write_file(dir.path / folder / "Added.added-0001.json", meta("Folder", "added-0001", "Added"));
    game.destroy(keep);
    game.destroy(box);
    const std::vector<engine_core::SaveConflict> rows = project.scan_disk().conflicts;
    REQUIRE(rows.size() == 1);

    SECTION("the disk's side brings it back, with everything under it") {
        REQUIRE(project.apply_disk({{rows[0], true}}).conflicts.empty());
        const std::optional<InstanceId> back = game.find_guid(box_guid);
        REQUIRE(back.has_value());
        REQUIRE(game.parent(*game.find_guid(keep_guid)) == *back);
        REQUIRE(game.parent(*game.find_guid("added-0001")) == *back);
    }
    SECTION("the studio's side lets the delete stand, and a save removes it all") {
        REQUIRE(project.apply_disk({{rows[0], false}}).conflicts.empty());
        REQUIRE(save_conflicts(project).empty());
        REQUIRE_FALSE(fs::exists(dir.path / folder));
    }
}

TEST_CASE("A10 a class changed on disk makes the instance again, keeping its children", "[A10][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const InstanceId child = add_part(game, a, "Child").id();
    const std::string guid = game.guid(a);
    project.save();
    edit_key(dir.path / ("src/A." + guid + "/init.json"), "class", engine_core::JsonValue::string("Folder"));

    REQUIRE(project.apply_disk().conflicts.empty());
    const std::optional<InstanceId> made = game.find_guid(guid);
    REQUIRE(made.has_value());
    REQUIRE(std::string(game.instance(*made)->class_name()) == "Folder");
    REQUIRE(game.parent(child) == *made);
}

TEST_CASE("A11 a key removed on disk puts the class default back", "[A11][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const fs::path file = dir.path / leaf(game, a.id());
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(file), doc, error));
    REQUIRE(doc.erase("Color"));
    write_file(file, engine_core::write_json(doc));

    project.apply_disk();
    REQUIRE_FALSE(has_key(a, "Color"));
}

TEST_CASE("A12 a source only the disk changed loads", "[A12][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    project.save();
    write_file(dir.path / leaf(game, main.id(), ".luau"), "print(\"disk\")\n");
    project.apply_disk();
    REQUIRE(main.source() == "print(\"disk\")\n");
}

TEST_CASE("A13 a key the engine does not know loads as an extra, and the next save is quiet", "[A13][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    engine_core::JsonValue custom = engine_core::JsonValue::object();
    custom.set("x", engine_core::JsonValue::number(1));
    edit_key(dir.path / leaf(game, a), "Custom", custom);

    project.apply_disk();
    REQUIRE(engine_core::bag_find(game.extra_properties(a), "Custom") != nullptr);
    project.save();
    REQUIRE(project.last_save().written.empty());
}

TEST_CASE("A14 an apply during a test throws", "[A14][disk][project]") {
    TempDir dir;
    ScriptRig rig;
    {
        Project project = Project::create(dir.path, rig.game);
        add_part(rig.game, 0, "Door");
        project.save();
    }
    Project project = Project::load(dir.path, rig.game);
    rig.game.start_simulation();
    REQUIRE_THROWS_AS(project.apply_disk(), ProjectError);
    rig.game.stop_simulation();
}

TEST_CASE("A15 a sibling order only the disk changed loads", "[A15][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "One");
    add_part(game, box, "Two");
    project.save();
    std::vector<std::string> order = child_guids(game, box);
    std::reverse(order.begin(), order.end());
    std::vector<engine_core::JsonValue> items;
    for (const std::string& guid : order) {
        items.push_back(engine_core::JsonValue::string(guid));
    }
    edit_key(dir.path / box_dir(game, box) / "init.json", "children", engine_core::JsonValue::array(std::move(items)));

    project.apply_disk();
    REQUIRE(child_guids(game, box) == order);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails: no type `DiskChoice`, no member `apply_disk`.

- [ ] **Step 3: Declare it in `Project.hpp`**

After `struct DiskScan`, add:

```cpp
// A person's pick for one row: disk true takes the disk's side.
struct DiskChoice {
    SaveConflict conflict;
    bool disk = false;
};
```

After the `scan_disk` declaration, add:

```cpp
    // Edit mode. Loads every change only the disk made, and the disk's side of
    // each choice, into the place as one undo step named "Changes from Disk".
    // The studio's side of a choice is settled in the base, so the next save
    // writes the studio's value. Scans first: a choice whose row changed since
    // it was listed is skipped, and listed again. Then the base takes the
    // disk's files wherever no row is left. Returns the rows still open.
    DiskScan apply_disk(const std::vector<DiskChoice>& choices = {});
```

In the private section, after `compare_disk`, add:

```cpp
    // The studio's side of a row: the base takes the disk's value there.
    void settle(const Comparison& compared, const SaveConflict& conflict);
    // The changes only the disk made, and the disk's side of chosen rows.
    void apply_changes(const Comparison& compared, std::vector<std::string>& loaded);
    // The base takes the disk's files wherever after has no row.
    void refresh_base(const Comparison& after);
```

- [ ] **Step 4: Implement the helpers**

In `Project.cpp`'s anonymous namespace, right after `build` (which `create_from` follows), add:

```cpp
// A new instance from its disk file, with its GUID, under parent.
InstanceId create_from(DataModel& world, const PlanNode& node, InstanceId parent) {
    DataModel& object = find_factory(node.class_name)(world);
    const InstanceId id = object.id();
    world.set_guid(id, node.guid);
    world.set_name(id, node.name);
    apply_properties(object, node);
    if (auto* lua = dynamic_cast<LuaSource*>(&object)) {
        lua->set_source(node.source);
    }
    world.set_parent(id, parent);
    return id;
}

// One property from the disk's file, or the class default when the file no
// longer has it. A key the class does not own is an extra.
void set_key(DataModel& world, InstanceId id, DataModel& object, const std::string& key, const JsonValue* value) {
    std::string error;
    if (value != nullptr) {
        if (!object.load_property(key, *value, error)) {
            world.set_extra_property(id, key, *value);
        }
        return;
    }
    PropertyBag defaults;
    object.default_properties(defaults);
    if (const JsonValue* fallback = bag_find(defaults, key)) {
        object.load_property(key, *fallback, error);
    } else {
        world.erase_extra_property(id, key);
    }
}

// id's children in the order a load gives them: those doc's "children" lists,
// in that order, then the rest by GUID. A child moves by leaving and rejoining
// its parent, since setting the same parent does nothing.
void order_children_as(DataModel& world, InstanceId id, const JsonValue& doc) {
    std::vector<std::string> listed;
    if (const JsonValue* children = doc.find("children"); children != nullptr && children->is_array()) {
        for (const JsonValue& item : children->items()) {
            if (item.is_string()) {
                listed.push_back(item.as_string());
            }
        }
    }
    const std::vector<InstanceId> now = world.get_children(id);
    std::vector<InstanceId> order = now;
    auto rank = [&](InstanceId child) {
        const std::string guid = world.guid(child);
        const auto at = std::find(listed.begin(), listed.end(), guid);
        return std::make_pair(static_cast<std::size_t>(at - listed.begin()), at == listed.end() ? guid : std::string());
    };
    std::stable_sort(order.begin(), order.end(), [&](InstanceId a, InstanceId b) { return rank(a) < rank(b); });
    if (order == now) {
        return;
    }
    for (InstanceId child : order) {
        world.set_parent(child, DataModel::kNoParent);
        world.set_parent(child, id);
    }
}
```

Add `#include "LuaSource.hpp"` to `Project.cpp` if `LuaSource` is not yet declared there (it is used by `build` already, so check with `grep -n LuaSource src/engine_core/Project.cpp`).

- [ ] **Step 5: Implement `apply_disk` and its members**

After `Project::scan_disk`, add:

```cpp
DiskScan Project::apply_disk(const std::vector<DiskChoice>& choices) {
    using Type = Comparison::Action::Type;
    DataModel& world = *game_;
    if (world.simulation_running()) {
        fail("changes from disk load in edit mode; stop the test first");
    }
    Comparison compared = compare_disk();
    DiskScan out;
    for (const DiskChoice& choice : choices) {
        if (std::find(compared.rows.begin(), compared.rows.end(), choice.conflict) == compared.rows.end()) {
            out.skipped.push_back(choice.conflict);
            continue;
        }
        if (!choice.disk) {
            settle(compared, choice.conflict);
            continue;
        }
        Comparison::Action action;
        action.guid = choice.conflict.guid;
        if (choice.conflict.kind == SaveConflict::Kind::DeletedOutside) {
            action.type = Type::Destroy;
        } else if (choice.conflict.key.empty()) {
            action.type = Type::Restore;
        } else if (choice.conflict.key == "class") {
            action.type = Type::Recreate;
        } else {
            action.type = Type::Set;
            action.key = choice.conflict.key;
        }
        compared.actions.push_back(std::move(action));
    }
    if (!compared.actions.empty()) {
        // An edit still open is its own step; this one is "Changes from Disk".
        world.history().end_gesture();
        const std::optional<std::string> recording = world.history().try_begin_recording("Changes from Disk");
        apply_changes(compared, out.loaded);
        if (recording) {
            world.history().finish_recording(*recording, FinishRecordingOperation::Commit);
        }
        world.capture_place();
    }
    Comparison after = compare_disk();
    refresh_base(after);
    out.conflicts = std::move(after.rows);
    out.has_disk_changes = !after.actions.empty();
    return out;
}

void Project::settle(const Comparison& compared, const SaveConflict& conflict) {
    if (conflict.kind == SaveConflict::Kind::DeletedOutside) {
        // Kept: a save writes it as a new file.
        files_.erase(conflict.guid);
        return;
    }
    const auto disk = compared.on_disk.find(conflict.guid);
    if (disk == compared.on_disk.end()) {
        return;
    }
    const std::size_t index = disk->second;
    if (conflict.key.empty()) {
        // The studio's delete stands. The base takes the disk's files under it
        // too, so a save removes them all.
        std::set<std::size_t> within{index};
        for (std::size_t at = index; at < compared.plan.size(); ++at) {
            if (at != index && within.count(compared.plan_parents[at]) == 0) {
                continue;
            }
            within.insert(at);
            files_[compared.plan[at].guid] = from_disk(compared.plan[at], compared.disk_parent(at));
        }
        return;
    }
    const detail::PlanNode& node = compared.plan[index];
    const auto base = files_.find(conflict.guid);
    if (base == files_.end() || conflict.key == "class") {
        files_[conflict.guid] = from_disk(node, compared.disk_parent(index));
        return;
    }
    Files& files = base->second;
    if (conflict.key == "Parent") {
        files.parent = compared.disk_parent(index);
    } else if (conflict.key == "Source") {
        files.source_bytes = node.source;
    } else {
        if (const JsonValue* value = node.doc.find(conflict.key)) {
            files.props.set(conflict.key, *value);
        } else {
            files.props.erase(conflict.key);
        }
        files.props_bytes = write_json(files.props);
    }
}

void Project::apply_changes(const Comparison& compared, std::vector<std::string>& loaded) {
    using Type = Comparison::Action::Type;
    using Action = Comparison::Action;
    DataModel& world = *game_;
    auto note = [&loaded](const std::string& name) {
        if (std::find(loaded.begin(), loaded.end(), name) == loaded.end()) {
            loaded.push_back(name);
        }
    };
    auto each = [&compared](Type type, const std::function<void(const Action&)>& run) {
        for (const Action& action : compared.actions) {
            if (action.type == type) {
                run(action);
            }
        }
    };
    // Parents that took a child from the disk, to order as the disk does.
    std::set<std::string> received;

    // New instances first, parents before children, so every later step finds them.
    each(Type::Create, [&](const Action& action) {
        const std::size_t index = compared.on_disk.at(action.guid);
        const std::string parent = compared.disk_parent(index);
        if (const std::optional<InstanceId> up = world.find_guid(parent)) {
            create_from(world, compared.plan[index], *up);
            received.insert(parent);
            note(compared.plan[index].name);
        }
    });
    // An instance the studio deleted, back from the disk with everything under it.
    each(Type::Restore, [&](const Action& action) {
        const std::size_t top = compared.on_disk.at(action.guid);
        std::set<std::size_t> within{top};
        for (std::size_t index = top; index < compared.plan.size(); ++index) {
            if (index != top && within.count(compared.plan_parents[index]) == 0) {
                continue;
            }
            within.insert(index);
            const detail::PlanNode& node = compared.plan[index];
            const std::string parent = compared.disk_parent(index);
            const std::optional<InstanceId> up = world.find_guid(parent);
            if (world.find_guid(node.guid) || !up) {
                continue;
            }
            create_from(world, node, *up);
            received.insert(parent);
            note(node.name);
        }
    });
    // A new class is a new instance with the same GUID, children, and place among its siblings.
    each(Type::Recreate, [&](const Action& action) {
        const std::optional<InstanceId> old = world.find_guid(action.guid);
        if (!old || *old == 0) {
            return;
        }
        const detail::PlanNode& node = compared.plan[compared.on_disk.at(action.guid)];
        const InstanceId parent = world.parent(*old);
        const std::vector<InstanceId> siblings = world.get_children(parent);
        const std::vector<InstanceId> kids = world.get_children(*old);
        for (InstanceId kid : kids) {
            world.set_parent(kid, DataModel::kNoParent);
        }
        world.destroy(*old);
        const InstanceId made = create_from(world, node, parent);
        for (InstanceId kid : kids) {
            world.set_parent(kid, made);
        }
        for (InstanceId sibling : siblings) {
            const InstanceId at = sibling == *old ? made : sibling;
            world.set_parent(at, DataModel::kNoParent);
            world.set_parent(at, parent);
        }
        note(node.name);
    });
    // Keys, shallowest instance first, so a parent moves before a child moves under it.
    std::vector<const Action*> sets;
    for (const Action& action : compared.actions) {
        if (action.type == Type::Set) {
            sets.push_back(&action);
        }
    }
    auto depth = [&](const Action* action) {
        std::size_t out = 0;
        for (std::size_t at = compared.on_disk.at(action->guid); at != 0; at = compared.plan_parents[at]) {
            ++out;
        }
        return out;
    };
    std::stable_sort(sets.begin(), sets.end(), [&](const Action* a, const Action* b) { return depth(a) < depth(b); });
    for (const Action* action : sets) {
        const std::optional<InstanceId> id = world.find_guid(action->guid);
        if (!id) {
            continue;
        }
        const std::size_t index = compared.on_disk.at(action->guid);
        const detail::PlanNode& node = compared.plan[index];
        DataModel& object = *id == 0 ? world : *world.instance(*id);
        if (action->key == "Name") {
            world.set_name(*id, node.name);
        } else if (action->key == "Parent") {
            const std::string parent = compared.disk_parent(index);
            const std::optional<InstanceId> up = world.find_guid(parent);
            bool cycle = false;
            for (InstanceId cursor = up ? *up : 0; up && cursor != 0 && cursor != DataModel::kNoParent;
                 cursor = world.parent(cursor)) {
                cycle = cycle || cursor == *id;
            }
            if (up && !cycle && world.parent(*id) != *up) {
                world.set_parent(*id, *up);
                received.insert(parent);
            }
        } else if (action->key == "Source") {
            if (auto* lua = dynamic_cast<LuaSource*>(&object)) {
                lua->set_source(node.source);
            }
        } else if (action->key == "children") {
            order_children_as(world, *id, node.doc);
        } else {
            set_key(world, *id, object, action->key, node.doc.find(action->key));
        }
        note(node.name);
    }
    for (const std::string& parent : received) {
        const bool open = std::any_of(compared.rows.begin(), compared.rows.end(), [&](const SaveConflict& row) {
            return row.guid == parent && row.key == "children";
        });
        const std::optional<InstanceId> id = world.find_guid(parent);
        const auto disk = compared.on_disk.find(parent);
        if (!open && id && disk != compared.on_disk.end()) {
            order_children_as(world, *id, compared.plan[disk->second].doc);
        }
    }
    // Deleted on disk, last, after anything under them moved out.
    each(Type::Destroy, [&](const Action& action) {
        if (const std::optional<InstanceId> id = world.find_guid(action.guid); id && *id != 0) {
            note(world.name(*id));
            world.destroy_tree(*id);
        }
    });
}

void Project::refresh_base(const Comparison& after) {
    std::set<std::string> open;
    for (const SaveConflict& row : after.rows) {
        open.insert(row.guid);
    }
    for (std::size_t index = 0; index < after.plan.size(); ++index) {
        const detail::PlanNode& node = after.plan[index];
        if (open.count(node.guid) != 0) {
            continue;  // Still in question: its base stays until a choice or a save.
        }
        // Added on disk under an instance the studio deleted: that instance's row carries it.
        if (files_.count(node.guid) == 0 && after.in_studio.count(node.guid) == 0) {
            continue;
        }
        files_[node.guid] = from_disk(node, after.disk_parent(index));
    }
    for (auto it = files_.begin(); it != files_.end();) {
        if (after.on_disk.count(it->first) == 0 && open.count(it->first) == 0) {
            it = files_.erase(it);
        } else {
            ++it;
        }
    }
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[disk]"`
Expected: `All tests passed` (28 test cases).

Run: `./build/sandbox && ./build/studio-tests; echo "studio exit $?"`
Expected: `All tests passed` (215 test cases) and `studio exit 0`.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_tests.cpp
git commit -m "$(cat <<'EOF'
Load changes from disk, and apply a choice for each conflict

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: `unsaved`

**Files:**
- Modify: `src/engine_core/Project.hpp`, `src/engine_core/Project.cpp`
- Test: `sandbox/project_tests.cpp` (append U1–U4)

**Interfaces:**
- Consumes: Task 3's `Files::props`, Task 1's `merge_keys`.
- Produces: `bool Project::unsaved() const;` the studio plan's title `*` uses it for an open project.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/project_tests.cpp`:

```cpp
TEST_CASE("U1 unsaved follows edits and their undo", "[U1][disk][project]") {
    SimRole role;
    TempDir dir;
    {
        Project project = Project::create(dir.path);
        add_part(project.datamodel(), 0, "A");
        project.save();
    }
    Project project = Project::load(dir.path);
    DataModel& game = project.datamodel();
    REQUIRE_FALSE(project.unsaved());
    const InstanceId a = game.find_first_child(0, "A");
    game.history().set_pending_gesture("Color");
    game.game_object(a)->set_color(rgb(1.f, 0.f, 0.f));
    game.history().end_gesture();
    REQUIRE(project.unsaved());
    game.history().undo();
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("U2 changes loaded from disk are not unsaved", "[U2][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    edit_key(dir.path / leaf(game, a), "Size", triple(2, 2, 2));
    project.apply_disk();
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("U3 a file only reformatted on disk is not unsaved", "[U3][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const std::string path = leaf(game, a.id());
    std::string wide = read_file(dir.path / path);
    for (std::size_t at = wide.find("\n  "); at != std::string::npos; at = wide.find("\n  ", at + 5)) {
        wide.replace(at, 3, "\n    ");
    }
    write_file(dir.path / path, wide);
    project.apply_disk();
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("U4 a root saved as DataModel is not unsaved", "[U4][disk][project]") {
    SimRole role;
    TempDir dir;
    write_bare_project(dir.path);
    Project project = Project::load(dir.path);
    REQUIRE_FALSE(project.unsaved());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails: no member named `unsaved`.

- [ ] **Step 3: Implement it**

`Project.hpp`, after `place_fingerprint`:

```cpp
    // A save would write, move, or remove something: the place differs from the
    // last load or save, compared key by key, so a file on disk that is only
    // formatted differently does not count.
    bool unsaved() const;
```

`Project.cpp`, after `Project::place_fingerprint`:

```cpp
bool Project::unsaved() const {
    const std::vector<AuthoredNode> tree = game_->authored_tree(nullptr);
    Layout layout;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        std::string ignored;
        read_project_json(root_, ignored, layout);
    }
    std::map<std::string, Files> next;
    try {
        next = plan_files(tree, layout.src, files_);
    } catch (const ProjectError&) {
        return true;
    }
    if (next.size() != files_.size()) {
        return true;
    }
    for (const auto& [guid, planned] : next) {
        const auto base = files_.find(guid);
        if (base == files_.end()) {
            return true;
        }
        const Files& was = base->second;
        if (planned.props_path != was.props_path || planned.has_source != was.has_source ||
            planned.source_path != was.source_path || planned.source_bytes != was.source_bytes) {
            return true;
        }
        for (const KeyMerge& merged : merge_keys(was.props, was.props, planned.props)) {
            // The root's class is always the studio's; files saved before say DataModel.
            if (merged.change == KeyChange::StudioOnly && !(guid == tree[0].guid && merged.key == "class")) {
                return true;
            }
        }
    }
    return false;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[disk]"`
Expected: `All tests passed` (32 test cases).

Run: `cmake --build build --parallel && for t in engine-tests properties-tests console-tests studio-tests mcp-tests sandbox shell-tests explorer-tests; do ./build/$t >/dev/null 2>&1; echo "$t exit $?"; done`
Expected: every line ends `exit 0`.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_tests.cpp
git commit -m "$(cat <<'EOF'
A project says whether a save would change anything, key by key

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```
