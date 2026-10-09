# Studio Plugins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Users save a Folder as a plugin. The studio loads it from the config folder and reloads it when its file changes. Plugin scripts get a `plugin` global that adds toolbar buttons to a Plugins ribbon tab, and dock widgets that show the plugin's own GUI.

**Architecture:** There are four layers, built bottom-up:
1. `engine_core/InstanceFile` turns the clipboard's `CopiedNode` trees into `.aeinst`/`.aeplugin` JSON and back.
2. `ide/PluginLoader` loads, diffs, and unloads user plugins under `Core` through `ScriptRuntime::register_plugin`.
3. `engine_core/PluginUi` plus new Lua userdata give plugins `plugin`, toolbars, and buttons. The IDE reads that state once a frame and draws a tabbed ribbon.
4. `DockWidget`, a `GuiBase` instance, is drawn by a new `runner::GuiTree` (extracted from `GuiLayer`) inside an `ide::PluginWidgetPane`.

**Tech Stack:** C++17, Luau, JadeFX, Catch2 (sandbox), the hand-rolled `Expect` harness (studio-tests), CMake with Visual Studio on Windows.

**Spec:** `docs/superpowers/specs/2026-10-09-studio-plugins-design.md`

## Global Constraints

- Plugin files: `config_directory()/plugins/*.aeplugin`. Instance files: `.aeinst`. Format tag `"format": "aeinst"`, `"version": 1`.
- The word "model" is never used for these files, in code, UI text, or docs.
- Engine distances are "units", never "studs".
- Icon paths passed by plugins must start with `icons/`, contain no `..`, and resolve against `resources/icons/`.
- Pane names for plugin widgets: `plugin:<PluginName>/<id>`.
- Plugins live in `Core`: never saved, never recorded in undo.
- `plugin` is set only on plugin-VM threads; it reads `nil` in Play and Console.
- Lua runs only on the simulation thread. UI code reaches the DataModel through `Engine::on_simulation`, `IdeLayout::run_now`, or a `DataModelLock` read with a 1 ms timeout.
- Match the surrounding code: comments are full sentences saying what and why, with no `TODO`s. Function names are `snake_case` in engine/ide code and `camelCase` on JadeFX node subclasses, as the existing files do.

## Build and test commands (PowerShell, repo root)

- Sandbox (Catch2): `cmake --build build --config Debug --target sandbox`, then `build\Debug\sandbox.exe "[TAG]"`.
- Studio tests: `cmake --build build --config Debug --target studio-tests`, then `build\Debug\studio-tests.exe` (prints `FAIL ...` lines; exit code is the failure count).
- Re-run CMake configure (`cmake -S . -B build`) after adding a source file to `CMakeLists.txt`.
- Known pre-existing flakes (ignore): studio-tests "six frames", Debug Q-decomposition timeouts under load, DS1/DS3 CloudCover default.

## Review Focus

1. **A plugin file that is written while the studio polls.** A half-written or locked file must log one error and retry on the next modification-time change, never crash or leave a half-built tree in Core. The test lives in Task 3.
2. **Saving a Folder that holds a disabled Script, an empty Folder, or a ModuleScript only.** The saved plugin must load with the same tree. A plugin with zero enabled Scripts is still a plugin, with no toolbar. Tests in Tasks 1 and 3.
3. **Two plugins with the same toolbar name.** They must be two separate groups, each removed only with its own plugin. Test in Task 6.
4. **A plugin that errors on load, then is fixed and saved again.** The reload must run the new source with no stale toolbar or widget left from the failed load. Test in Task 3 (errors) and Task 6 (toolbars gone after unload).
5. **A widget's GUI that the plugin reparents out of the DockWidget, or destroys.** The pane must show the change on the next frame and must not keep dead nodes. Test in Task 9.

---

## Sub-project 1: Instance files

### Task 1: `engine_core/InstanceFile`: copy trees and their JSON

**Files:**
- Create: `src/engine_core/InstanceFile.hpp`, `src/engine_core/InstanceFile.cpp`
- Modify: `src/ide/CutSet.hpp` (drop `CopiedNode`, `copy_set`'s helpers move), `src/ide/CutSet.cpp:128-222`
- Modify: `CMakeLists.txt`: add `src/engine_core/InstanceFile.cpp` to the `engine_core` library sources (beside `src/engine_core/LuaApi.cpp`, ~line 611) and `sandbox/instance_file_tests.cpp` to `add_executable(sandbox ...)`
- Test: `sandbox/instance_file_tests.cpp` (new)

**Interfaces:**
- Produces (namespace `engine_core`):
  ```cpp
  struct CopiedNode {
      std::string class_name;
      std::string name;
      PropertyBag properties;
      bool has_source = false;
      std::string source;
      std::vector<CopiedNode> children;
  };
  // The instance and everything under it, as copy and paste carry it.
  CopiedNode copy_tree(const DataModel& game, InstanceId id);
  // Builds each root under parent. False when none could be built. made gets each built root's id;
  // refused, when given, the first reason one was refused.
  bool paste_copies(DataModel& world, const std::vector<CopiedNode>& roots, InstanceId parent,
                    std::vector<InstanceId>* made = nullptr, std::string* refused = nullptr);
  JsonValue write_instance_file(const std::vector<CopiedNode>& roots);
  bool read_instance_file(const JsonValue& json, std::vector<CopiedNode>& roots, std::string& error);
  bool save_instance_file(const std::filesystem::path& path, const std::vector<CopiedNode>& roots, std::string& error);
  bool load_instance_file(const std::filesystem::path& path, std::vector<CopiedNode>& roots, std::string& error);
  ```
- `ide::copy_set(game, ids)` keeps its signature. It becomes `cut_set` followed by `engine_core::copy_tree` per id. `ide` code that names `CopiedNode` or `paste_copies` gets them through `using engine_core::CopiedNode; using engine_core::paste_copies;` in `CutSet.hpp`.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/instance_file_tests.cpp`:

```cpp
// Instance files: a subtree as one JSON file, through the clipboard's copy and paste.

#include "support.hpp"

#include "Folder.hpp"
#include "Gui.hpp"
#include "InstanceFile.hpp"
#include "ModuleScript.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

namespace {

using engine_core::CopiedNode;
using engine_core::InstanceId;

InstanceId sample_tree(engine_core::DataModel& game) {
    engine_core::Folder& root = game.create<engine_core::Folder>();
    game.set_name(root.id(), "MyPlugin");
    game.set_parent(root.id(), workspace_of(game));
    engine_core::Script& init = add_script(game, root.id(), "init", "print('hi')");
    init.set_enabled(true);
    engine_core::Script& off = add_script(game, root.id(), "Off", "print('off')");
    off.set_enabled(false);
    engine_core::ModuleScript& lib = game.create<engine_core::ModuleScript>();
    game.set_name(lib.id(), "Lib");
    lib.set_source("return 42");
    game.set_parent(lib.id(), root.id());
    engine_core::Pane& pane = game.create<engine_core::Pane>();
    game.set_name(pane.id(), "Panel");
    game.set_parent(pane.id(), root.id());
    engine_core::Label& label = game.create<engine_core::Label>();
    game.set_name(label.id(), "Title");
    game.set_parent(label.id(), pane.id());
    engine_core::Css& css = game.create<engine_core::Css>();
    game.set_name(css.id(), "Style");
    game.set_parent(css.id(), pane.id());
    engine_core::Folder& empty = game.create<engine_core::Folder>();
    game.set_name(empty.id(), "Empty");
    game.set_parent(empty.id(), root.id());
    return root.id();
}

// Name/class shape of a tree, for comparing two trees.
std::string shape(const CopiedNode& node) {
    std::string out = node.class_name + ":" + node.name;
    if (node.has_source) {
        out += "{" + node.source + "}";
    }
    out += "[";
    for (const CopiedNode& child : node.children) {
        out += shape(child) + ",";
    }
    return out + "]";
}

}  // namespace

TEST_CASE("IF1 a tree saves to JSON and reads back the same", "[IF1]") {
    ScriptRig rig;
    const InstanceId root = sample_tree(rig.game);
    const std::vector<CopiedNode> before{engine_core::copy_tree(rig.game, root)};

    const engine_core::JsonValue json = engine_core::write_instance_file(before);
    REQUIRE(json.find("format")->as_string() == "aeinst");
    REQUIRE(json.find("version")->as_number() == 1);

    std::vector<CopiedNode> after;
    std::string error;
    REQUIRE(engine_core::read_instance_file(json, after, error));
    REQUIRE(error.empty());
    REQUIRE(after.size() == 1);
    REQUIRE(shape(after[0]) == shape(before[0]));
    REQUIRE(after[0].children[1].properties == before[0].children[1].properties);  // Off stays disabled
}

TEST_CASE("IF2 a file holds several roots, and pasting builds each", "[IF2]") {
    ScriptRig rig;
    const InstanceId a = sample_tree(rig.game);
    const InstanceId b = sample_tree(rig.game);
    rig.game.set_name(b, "Other");
    TempDir dir;
    std::string error;
    REQUIRE(engine_core::save_instance_file(dir / "two.aeinst",
                                            {engine_core::copy_tree(rig.game, a), engine_core::copy_tree(rig.game, b)},
                                            error));
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "two.aeinst.tmp"));
    std::vector<CopiedNode> roots;
    REQUIRE(engine_core::load_instance_file(dir / "two.aeinst", roots, error));
    REQUIRE(roots.size() == 2);

    const InstanceId target = rig.game.create<engine_core::Folder>().id();
    rig.game.set_parent(target, workspace_of(rig.game));
    std::vector<InstanceId> made;
    REQUIRE(engine_core::paste_copies(rig.game, roots, target, &made));
    REQUIRE(made.size() == 2);
    REQUIRE(rig.game.name(made[1]) == "Other");
    REQUIRE(rig.game.find_first_child(made[0], "Lib") != 0);
}

TEST_CASE("IF3 a bad file names where it went wrong and builds nothing", "[IF3]") {
    auto read = [](const char* text) {
        engine_core::JsonValue json;
        std::string error;
        if (!engine_core::parse_json(text, json, error)) {
            return std::string("parse: ") + error;
        }
        std::vector<CopiedNode> roots;
        REQUIRE_FALSE(engine_core::read_instance_file(json, roots, error));
        REQUIRE(roots.empty());
        return error;
    };
    REQUIRE(read(R"({"format":"other","version":1,"roots":[]})").find("format") != std::string::npos);
    REQUIRE(read(R"({"format":"aeinst","version":2,"roots":[]})").find("version") != std::string::npos);
    REQUIRE(read(R"({"format":"aeinst","version":1,"roots":[{"name":"x"}]})").find("roots[0].class") !=
            std::string::npos);
    REQUIRE(read(R"({"format":"aeinst","version":1,"roots":[{"class":"NoSuchClass","name":"x"}]})")
                .find("roots[0].class") != std::string::npos);
    REQUIRE(read(R"({"format":"aeinst","version":1,"roots":[{"class":"Folder","name":"x","children":[{"class":"Folder"}]}]})")
                .find("roots[0].children[0].name") != std::string::npos);

    TempDir dir;
    std::filesystem::create_directories(dir.path);
    std::ofstream(dir.path / "half.aeinst") << R"({"format":"aeinst","vers)";
    std::vector<CopiedNode> roots;
    std::string error;
    REQUIRE_FALSE(engine_core::load_instance_file(dir / "half.aeinst", roots, error));
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(engine_core::load_instance_file(dir / "missing.aeinst", roots, error));
}

TEST_CASE("IF4 an unknown property key survives a round trip", "[IF4]") {
    ScriptRig rig;
    const InstanceId root = rig.game.create<engine_core::Folder>().id();
    rig.game.set_parent(root, workspace_of(rig.game));
    rig.game.set_extra_property(root, "FutureThing", engine_core::JsonValue::number(7));
    const engine_core::JsonValue json = engine_core::write_instance_file({engine_core::copy_tree(rig.game, root)});
    std::vector<CopiedNode> roots;
    std::string error;
    REQUIRE(engine_core::read_instance_file(json, roots, error));
    const engine_core::JsonValue* kept = engine_core::bag_find(roots[0].properties, "FutureThing");
    REQUIRE(kept != nullptr);
    REQUIRE(kept->as_number() == 7);
}
```

- [ ] **Step 2: Run the tests to see them fail**

Add both new source files to `CMakeLists.txt`, re-configure, and build `sandbox`.
Expected: the compile fails with `InstanceFile.hpp: No such file`.

- [ ] **Step 3: Write `InstanceFile.hpp`**

```cpp
#pragma once

#include "DataModel.hpp"
#include "PropertyBag.hpp"
#include "types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace engine_core {

// One instance and everything under it, as copy and paste and instance files
// carry it: its class, name, saved properties (the ones its class does not
// know too), a script's source, and its children in order.
struct CopiedNode {
    std::string class_name;
    std::string name;
    PropertyBag properties;
    bool has_source = false;
    std::string source;
    std::vector<CopiedNode> children;
};

// The instance and everything under it. An empty node for a dead id.
CopiedNode copy_tree(const DataModel& game, InstanceId id);

// Builds each root, with its children, under parent, last among its
// children. A class that cannot be made, a full place, or a parent that
// refuses one leaves that root out, and refused, when given, gets the first
// reason. False when none could be built. Runs on the simulation thread.
bool paste_copies(DataModel& world, const std::vector<CopiedNode>& roots, InstanceId parent,
                  std::vector<InstanceId>* made = nullptr, std::string* refused = nullptr);

// An instance file (.aeinst, and .aeplugin, which is the same format):
// {"format": "aeinst", "version": 1, "roots": [node...]}, where a node is
// {"class", "name", "properties"?, "source"?, "children"?}.
JsonValue write_instance_file(const std::vector<CopiedNode>& roots);
// False, with error naming the JSON path that is wrong, for another format
// or version, a node missing its class or name, a class no one can make, or
// a value of the wrong kind. roots is left empty then.
bool read_instance_file(const JsonValue& json, std::vector<CopiedNode>& roots, std::string& error);
// Writes path.tmp, then renames it over path, so a reader never sees half a file.
bool save_instance_file(const std::filesystem::path& path, const std::vector<CopiedNode>& roots, std::string& error);
bool load_instance_file(const std::filesystem::path& path, std::vector<CopiedNode>& roots, std::string& error);

}  // namespace engine_core
```

- [ ] **Step 4: Write `InstanceFile.cpp`**

Move `copy_node` and `build_copy` out of `CutSet.cpp:128-194` into this file unchanged, renaming `copy_node` to `copy_tree` and qualifying nothing, since this file is already in namespace `engine_core`. Move `paste_copies` (CutSet.cpp:206-222) unchanged. Then add the JSON functions:

```cpp
#include "InstanceFile.hpp"

#include "FileBytes.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"

#include <optional>
#include <system_error>

namespace engine_core {
namespace {

// ... copy_tree's body (was copy_node) and build_copy, moved from CutSet.cpp ...

JsonValue node_json(const CopiedNode& node) {
    JsonValue out = JsonValue::object();
    out.set("class", JsonValue::string(node.class_name));
    out.set("name", JsonValue::string(node.name));
    if (!node.properties.empty()) {
        JsonValue properties = JsonValue::object();
        for (const JsonValue::Member& member : node.properties) {
            properties.set(member.first, member.second);
        }
        out.set("properties", std::move(properties));
    }
    if (node.has_source) {
        out.set("source", JsonValue::string(node.source));
    }
    if (!node.children.empty()) {
        std::vector<JsonValue> children;
        for (const CopiedNode& child : node.children) {
            children.push_back(node_json(child));
        }
        out.set("children", JsonValue::array(std::move(children)));
    }
    return out;
}

bool read_node(const JsonValue& json, const std::string& where, CopiedNode& out, std::string& error) {
    if (!json.is_object()) {
        error = where + " must be an object";
        return false;
    }
    const JsonValue* klass = json.find("class");
    if (klass == nullptr || !klass->is_string()) {
        error = where + ".class must be a string";
        return false;
    }
    if (!lua_class_creatable(klass->as_string().c_str())) {
        error = where + ".class: no class " + klass->as_string();
        return false;
    }
    const JsonValue* name = json.find("name");
    if (name == nullptr || !name->is_string()) {
        error = where + ".name must be a string";
        return false;
    }
    out.class_name = klass->as_string();
    out.name = name->as_string();
    if (const JsonValue* properties = json.find("properties")) {
        if (!properties->is_object()) {
            error = where + ".properties must be an object";
            return false;
        }
        for (const JsonValue::Member& member : properties->members()) {
            bag_set(out.properties, member.first, member.second);
        }
    }
    if (const JsonValue* source = json.find("source")) {
        if (!source->is_string()) {
            error = where + ".source must be a string";
            return false;
        }
        out.has_source = true;
        out.source = source->as_string();
    }
    if (const JsonValue* children = json.find("children")) {
        if (!children->is_array()) {
            error = where + ".children must be an array";
            return false;
        }
        for (std::size_t i = 0; i < children->items().size(); ++i) {
            CopiedNode child;
            if (!read_node(children->items()[i], where + ".children[" + std::to_string(i) + "]", child, error)) {
                return false;
            }
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

}  // namespace

JsonValue write_instance_file(const std::vector<CopiedNode>& roots) {
    JsonValue out = JsonValue::object();
    out.set("format", JsonValue::string("aeinst"));
    out.set("version", JsonValue::number(1));
    std::vector<JsonValue> items;
    for (const CopiedNode& root : roots) {
        items.push_back(node_json(root));
    }
    out.set("roots", JsonValue::array(std::move(items)));
    return out;
}

bool read_instance_file(const JsonValue& json, std::vector<CopiedNode>& roots, std::string& error) {
    roots.clear();
    const JsonValue* format = json.find("format");
    if (format == nullptr || format->as_string() != "aeinst") {
        error = "format must be \"aeinst\"";
        return false;
    }
    const JsonValue* version = json.find("version");
    if (version == nullptr || !version->is_number() || version->as_number() != 1) {
        error = "version must be 1";
        return false;
    }
    const JsonValue* items = json.find("roots");
    if (items == nullptr || !items->is_array()) {
        error = "roots must be an array";
        return false;
    }
    std::vector<CopiedNode> out;
    for (std::size_t i = 0; i < items->items().size(); ++i) {
        CopiedNode node;
        if (!read_node(items->items()[i], "roots[" + std::to_string(i) + "]", node, error)) {
            return false;
        }
        out.push_back(std::move(node));
    }
    roots = std::move(out);
    return true;
}

bool save_instance_file(const std::filesystem::path& path, const std::vector<CopiedNode>& roots, std::string& error) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    if (!write_file(temp, write_json(write_instance_file(roots)), error)) {
        return false;
    }
    std::error_code code;
    std::filesystem::rename(temp, path, code);
    if (code) {
        std::filesystem::remove(temp, code);
        error = "could not replace " + path.u8string();
        return false;
    }
    return true;
}

bool load_instance_file(const std::filesystem::path& path, std::vector<CopiedNode>& roots, std::string& error) {
    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }
    JsonValue json;
    if (!parse_json(text, json, error)) {
        return false;
    }
    return read_instance_file(json, roots, error);
}

}  // namespace engine_core
```

`lua_class_creatable(name)` must exist. Check `LuaApi.hpp` for the registry's lookup (`register_lua_creatable` is at `ScriptBindings.cpp:250`). If there is no public "is this creatable" query, add one next to `register_lua_creatable`: `bool lua_class_creatable(const char* name);`, true when a creator is registered. `build_copy` already calls `lua_create_instance`, so the two agree.

- [ ] **Step 5: Point `CutSet` at it**

In `CutSet.hpp`, delete the `CopiedNode` struct and the `paste_copies` declaration. Add `#include "InstanceFile.hpp"` and `using engine_core::CopiedNode; using engine_core::paste_copies;` inside `namespace ide`. In `CutSet.cpp`, delete the moved anonymous-namespace functions and `paste_copies`. `copy_set` becomes:

```cpp
std::vector<CopiedNode> copy_set(const engine_core::DataModel& game, const std::vector<engine_core::InstanceId>& ids) {
    std::vector<CopiedNode> out;
    for (engine_core::InstanceId id : cut_set(game, ids)) {
        out.push_back(engine_core::copy_tree(game, id));
    }
    return out;
}
```

- [ ] **Step 6: Run the tests**

Build `sandbox`, then run `build\Debug\sandbox.exe "[IF1],[IF2],[IF3],[IF4]"`. Expected: all pass.
Also build `explorer-tests` and `properties-tests`, then run `build\Debug\explorer-tests.exe` (clipboard callers compile and still pass).

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/InstanceFile.* src/ide/CutSet.* sandbox/instance_file_tests.cpp CMakeLists.txt src/engine_core/LuaApi.*
git commit -m "Instance files: a subtree as one .aeinst JSON file"
```

---

## Sub-project 2: Plugin loading

### Task 2: A Folder in Core can be one plugin

**Files:**
- Modify: `src/engine_core/ScriptRuntime.hpp:154-179`, `src/engine_core/ScriptRuntime.cpp:940-970` (`start_core_scripts`), `:1744` (`register_plugin`)
- Test: `sandbox/plugin_tests.cpp` (append)

**Interfaces:**
- Produces:
  - `bool ScriptRuntime::register_plugin(InstanceId root, std::string name = {});` An empty `name` takes the root's Name.
  - `const std::string* ScriptRuntime::plugin_name(InstanceId root) const;` Null for a root that is not registered.
  - `std::uint32_t ScriptRuntime::plugin_serial(InstanceId root) const;` 0 for none.
- `Plugin` (private struct) gains `std::string name;`.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/plugin_tests.cpp`:

```cpp
TEST_CASE("PL11 a Folder registered in Core is one plugin, and its Scripts do not register alone", "[PL11]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "A", "print('a')");
    add_script(rig.game, folder, "B", "print('b')");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(folder, "ToolsFile"));
    rig.runtime.start_core_scripts();
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{folder});
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"a\n", "b\n"});
    REQUIRE(*rig.runtime.plugin_name(folder) == "ToolsFile");
    REQUIRE(rig.runtime.plugin_serial(folder) != 0);

    // A Script added later under the registered Folder still does not become its own plugin.
    add_script(rig.game, folder, "C", "print('c')");
    rig.runtime.start_core_scripts();
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{folder});

    // A Folder in Core that is not registered keeps today's behaviour: each Script is a plugin.
    const InstanceId loose = add_folder(rig.game, "Loose", rig.game.core());
    const InstanceId s = add_script(rig.game, loose, "S", "print('s')").id();
    rig.runtime.start_core_scripts();
    REQUIRE(rig.runtime.is_plugin(s));
}
```

- [ ] **Step 2: Run it to see it fail.** Run `build\Debug\sandbox.exe "[PL11]"`. Expected: a compile error (no `register_plugin` overload with a name, no `plugin_name`).

- [ ] **Step 3: Implement**

In `ScriptRuntime.hpp`, change the declaration and add the two readers beside `is_plugin`:

```cpp
    // ... (existing comment) name is what plugin.Name reads; empty takes the root's Name.
    bool register_plugin(InstanceId root, std::string name = {});
    // The registered root's name, or null.
    const std::string* plugin_name(InstanceId root) const;
    // The registered root's serial, which owns its threads, or 0.
    std::uint32_t plugin_serial(InstanceId root) const;
```

Add `std::string name;` to `struct Plugin`. In `register_plugin`, build the record as
`Plugin plugin{root, plugin_serial_, name.empty() ? game_->name(root) : std::move(name)};`.

In `start_core_scripts`, extend the nesting walk so that a registered root counts as nesting:

```cpp
        for (InstanceId up = game_->parent(id); up != 0 && up != DataModel::kNoParent && up != core;
             up = game_->parent(up)) {
            if (dynamic_cast<Script*>(game_->instance(up)) != nullptr || is_plugin(up)) {
                nested = true;
                break;
            }
        }
```

Implement both readers with `std::find_if` over `plugins_`, matching on `root` and `game_->alive(root)`.

- [ ] **Step 4: Run the tests.** Run `build\Debug\sandbox.exe "[PL1],[PL2],[PL3],[PL4],[PL11]"` and `build\Debug\sandbox.exe "[core]"` (the Core tests). Expected: all pass.

- [ ] **Step 5: Commit.** `git commit -am "Plugins: a registered Folder in Core is one plugin"`

### Task 3: User plugins: load, diff, unload, reload

**Files:**
- Modify: `src/ide/PluginLoader.hpp`, `src/ide/PluginLoader.cpp`
- Modify: `src/engine_core/ScriptRuntime.hpp/.cpp`: add `fire_plugin_unloading(InstanceId root)`, a no-op stub until Task 5 fills it.
- Test: `sandbox/plugin_tests.cpp` (append; `PluginLoader` is in `studio_core`, which sandbox links)

**Interfaces:**
- Produces (namespace `ide`):
  ```cpp
  inline constexpr const char* kPluginExtension = ".aeplugin";
  // What the plugins folder holds: each .aeplugin's stem, path, time, and size.
  struct PluginStamp {
      std::string name;
      std::filesystem::path path;
      std::filesystem::file_time_type time{};
      std::uintmax_t size = 0;
      bool operator==(const PluginStamp& o) const { return name == o.name && time == o.time && size == o.size; }
  };
  // The folder's .aeplugin files sorted by name. Empty when the folder is missing.
  std::vector<PluginStamp> scan_plugins(const std::filesystem::path& folder);
  // The name a Folder saves as: its Name with characters a file name cannot hold replaced by '_', or "Plugin".
  std::string plugin_file_name(const std::string& folder_name);

  class PluginLoader {
  public:
      std::size_t load(...);                       // unchanged: the built-ins
      // SimulationThread. Brings Core's user plugins to what stamps says: loads the new, unloads the gone,
      // reloads the changed. Errors go to scripts' output as "Plugin \"X\" failed to load: why".
      void sync_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                     const std::vector<PluginStamp>& stamps);
      // The user plugins loaded now, by name: what sync_user last saw for each, and its root (0 when it failed).
      struct UserPlugin { PluginStamp stamp; engine_core::InstanceId root = 0; };
      const std::vector<UserPlugin>& user() const { return user_; }
  private:
      void unload_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, UserPlugin& plugin);
      std::vector<engine_core::InstanceId> loaded_;
      std::vector<UserPlugin> user_;
  };
  ```
- Produces (`ScriptRuntime`): `void fire_plugin_unloading(InstanceId root);` It fires `plugin.Unloading` and runs its handlers now. Task 5 gives it a body.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "InstanceFile.hpp"
#include "PluginLoader.hpp"

namespace {

// Writes a plugin file holding a Folder named name with one Script per source.
void write_plugin(const std::filesystem::path& folder, const std::string& name,
                  const std::vector<std::string>& sources) {
    engine_core::CopiedNode root;
    root.class_name = "Folder";
    root.name = name;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        engine_core::CopiedNode script;
        script.class_name = "Script";
        script.name = "S" + std::to_string(i);
        script.has_source = true;
        script.source = sources[i];
        root.children.push_back(std::move(script));
    }
    std::filesystem::create_directories(folder);
    std::string error;
    REQUIRE(engine_core::save_instance_file(folder / (name + ide::kPluginExtension), {root}, error));
}

}  // namespace

TEST_CASE("PL12 user plugins load, unload, and reload with their files", "[PL12]") {
    ScriptRig rig;
    TempDir dir;
    ide::PluginLoader loader;
    write_plugin(dir.path, "Alpha", {"print('alpha 1')", "task.wait(100) print('never')"});
    rig.runtime.drain_output();

    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user().size() == 1);
    const InstanceId root = loader.user()[0].root;
    REQUIRE(rig.game.parent(root) == rig.game.core());
    REQUIRE(rig.runtime.is_plugin(root));
    REQUIRE(*rig.runtime.plugin_name(root) == "Alpha");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"alpha 1\n"});

    // Unchanged files do nothing.
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user()[0].root == root);

    // A changed file reloads with the new source; the old waiting thread is gone.
    write_plugin(dir.path, "Alpha", {"print('alpha 2, longer')"});
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE_FALSE(rig.game.alive(root));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"alpha 2, longer\n"});
    rig.frames(2, 200.0);
    REQUIRE(rig.runtime.drain_output().lines.empty());

    // A removed file unloads.
    std::filesystem::remove(dir.path / "Alpha.aeplugin");
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user().empty());
    REQUIRE(rig.game.get_children(rig.game.core()).empty());
    REQUIRE(rig.runtime.plugins().empty());
}

TEST_CASE("PL13 a bad plugin file logs once, leaves Core clean, and loads once fixed", "[PL13]") {
    ScriptRig rig;
    TempDir dir;
    std::filesystem::create_directories(dir.path);
    std::ofstream(dir.path / "Broken.aeplugin") << R"({"format":"aeinst","vers)";
    ide::PluginLoader loader;
    rig.runtime.drain_output();

    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    const std::vector<std::string> out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].find("Plugin \"Broken\" failed to load") != std::string::npos);
    REQUIRE(rig.game.get_children(rig.game.core()).empty());
    // Polling again with the same stamp does not repeat the error.
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(rig.runtime.drain_output().lines.empty());

    // A root that is not a Folder is refused too.
    engine_core::CopiedNode script;
    script.class_name = "Script";
    script.name = "Lonely";
    std::string error;
    REQUIRE(engine_core::save_instance_file(dir.path / "Lonely.aeplugin", {script}, error));
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(texts(rig.runtime.drain_output())[0].find("Plugin \"Lonely\" failed to load: its root must be one Folder") !=
            std::string::npos);

    // Fixed: it loads.
    write_plugin(dir.path, "Broken", {"print('fixed')"});
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"fixed\n"});
}

TEST_CASE("PL14 plugin file names and the scan order", "[PL14]") {
    REQUIRE(ide::plugin_file_name("Terrain Tools") == "Terrain Tools");
    REQUIRE(ide::plugin_file_name("a/b:c*?") == "a_b_c__");
    REQUIRE(ide::plugin_file_name("") == "Plugin");
    TempDir dir;
    write_plugin(dir.path, "Zed", {});
    write_plugin(dir.path, "Apple", {});
    std::ofstream(dir.path / "notes.txt") << "x";
    const std::vector<ide::PluginStamp> stamps = ide::scan_plugins(dir.path);
    REQUIRE(stamps.size() == 2);
    REQUIRE(stamps[0].name == "Apple");
    REQUIRE(ide::scan_plugins(dir.path / "missing").empty());
}
```

- [ ] **Step 2: Run them to see them fail.** Run `build\Debug\sandbox.exe "[PL12],[PL13],[PL14]"`. Expected: compile errors for the missing `scan_plugins`, `sync_user`, and `kPluginExtension`.

- [ ] **Step 3: Implement**

In `PluginLoader.cpp`:

```cpp
std::string plugin_file_name(const std::string& folder_name) {
    std::string out;
    for (char c : folder_name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || static_cast<unsigned char>(c) < 32;
        out += bad ? '_' : c;
    }
    // Windows drops trailing dots and spaces from file names.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? "Plugin" : out;
}

std::vector<PluginStamp> scan_plugins(const std::filesystem::path& folder) {
    std::vector<PluginStamp> out;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const std::filesystem::path& path = it->path();
        if (path.extension() != kPluginExtension || !it->is_regular_file(error)) {
            continue;
        }
        PluginStamp stamp;
        stamp.name = path.stem().u8string();
        stamp.path = path;
        stamp.time = std::filesystem::last_write_time(path, error);
        stamp.size = std::filesystem::file_size(path, error);
        if (!error) {
            out.push_back(std::move(stamp));
        }
        error.clear();
    }
    std::sort(out.begin(), out.end(), [](const PluginStamp& a, const PluginStamp& b) { return a.name < b.name; });
    return out;
}

void PluginLoader::unload_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, UserPlugin& plugin) {
    if (plugin.root != 0 && game.alive(plugin.root)) {
        scripts.fire_plugin_unloading(plugin.root);
        scripts.unregister_plugin(plugin.root);
        game.destroy(plugin.root);
    }
    plugin.root = 0;
}

void PluginLoader::sync_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                             const std::vector<PluginStamp>& stamps) {
    std::vector<UserPlugin> next;
    for (UserPlugin& have : user_) {
        const auto found = std::find_if(stamps.begin(), stamps.end(),
                                        [&](const PluginStamp& s) { return s.name == have.stamp.name; });
        if (found == stamps.end() || !(*found == have.stamp)) {
            unload_user(game, scripts, have);
        } else {
            next.push_back(have);
        }
    }
    for (const PluginStamp& stamp : stamps) {
        const bool kept = std::any_of(next.begin(), next.end(),
                                      [&](const UserPlugin& p) { return p.stamp.name == stamp.name; });
        if (kept) {
            continue;
        }
        UserPlugin plugin;
        plugin.stamp = stamp;
        std::vector<engine_core::CopiedNode> roots;
        std::string error;
        if (engine_core::load_instance_file(stamp.path, roots, error) &&
            (roots.size() != 1 || roots[0].class_name != "Folder")) {
            error = "its root must be one Folder";
        }
        std::vector<engine_core::InstanceId> made;
        if (error.empty() && !engine_core::paste_copies(game, roots, game.core(), &made, &error)) {
            if (error.empty()) {
                error = "nothing could be built";
            }
        }
        if (!error.empty()) {
            for (engine_core::InstanceId id : made) {
                game.destroy(id);
            }
            scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                  "Plugin \"" + stamp.name + "\" failed to load: " + error);
        } else {
            plugin.root = made[0];
            scripts.register_plugin(plugin.root, stamp.name);
        }
        // A failed load keeps its stamp, so the same broken file is not retried every poll.
        next.push_back(std::move(plugin));
    }
    std::sort(next.begin(), next.end(),
              [](const UserPlugin& a, const UserPlugin& b) { return a.stamp.name < b.stamp.name; });
    user_ = std::move(next);
    scripts.start_core_scripts();
}
```

Note: `paste_copies` parents the Folder under Core before its children are built. Each Script then enters Core, and `start_core_scripts` (which runs after `register_plugin`) finds the registered Folder above it and does not register the Script alone. That's Task 2.

Add `void fire_plugin_unloading(InstanceId root) {}` to `ScriptRuntime` for now. Declare it in the header with the comment "Fires the plugin's Unloading and runs its handlers now. SimulationThread, outside any Lua call." Task 5 implements it.

- [ ] **Step 4: Run the tests.** Run `build\Debug\sandbox.exe "[PL12],[PL13],[PL14],[PL11]"`. Expected: all pass.

- [ ] **Step 5: Commit.** `git commit -am "Plugins: load user plugins from a folder and reload on change"`

### Task 4: IDE wiring: plugins folder, polling, Save as Plugin, Open Plugins Folder

**Files:**
- Modify: `src/engine_core/DataModel.hpp:33-45` (`InstanceAction`), `src/engine_core/DataModel.cpp:1558` (`action_label`)
- Modify: `src/engine_instances/Folder.hpp/.cpp` (`context_actions`)
- Modify: `src/ide/IdeExplorer.cpp:79-95` (`ActionIcon`), `src/ide/IdeAssets.cpp:385-402` (switch must cover the new value)
- Modify: `src/ide/IdeLayout.hpp`, `src/ide/IdeLayout.cpp` (constructor: `plugins_dir_`; File menu item; `flushFrame` polling; `load_plugins`), `src/ide/IdeLayoutEditing.cpp:86` (`run_action`)
- Test: `sandbox/plugin_tests.cpp` (the Folder offers the action), and `tests/PluginsTest.cpp` (new, studio-tests)

**Interfaces:**
- Produces: `InstanceAction::SaveAsPlugin`, label `"Save as Plugin"`, icon `"Export.png"`.
- Produces (`IdeLayout`):
  - `void save_as_plugin(std::uint32_t folder);`
  - `void poll_plugins(bool now);` On the UI thread. It scans at most once a second unless `now`, and posts `sync_user` to the simulation thread when the scan differs from the last one.
  - `std::filesystem::path plugins_dir_;` Empty when there is no config folder, as in tests that pass none.
  - `std::vector<PluginStamp> plugin_stamps_;`, `double plugin_poll_at_ = 0;`

- [ ] **Step 1: Write the failing tests**

Sandbox (append to `plugin_tests.cpp`):

```cpp
TEST_CASE("PL15 a Folder offers Save as Plugin; other classes do not", "[PL15]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "F", workspace_of(rig.game));
    std::vector<engine_core::ContextAction> actions;
    rig.game.instance(folder)->context_actions(actions);
    REQUIRE(std::any_of(actions.begin(), actions.end(), [](const engine_core::ContextAction& a) {
        return a.action == engine_core::InstanceAction::SaveAsPlugin;
    }));
    REQUIRE(std::string(engine_core::action_label(engine_core::InstanceAction::SaveAsPlugin)) == "Save as Plugin");
    actions.clear();
    add_script(rig.game, folder, "S", "").context_actions(actions);
    REQUIRE(std::none_of(actions.begin(), actions.end(), [](const engine_core::ContextAction& a) {
        return a.action == engine_core::InstanceAction::SaveAsPlugin;
    }));
}
```

Studio-tests, new `tests/PluginsTest.cpp`, registered in `tests/StudioLayoutTest.cpp` as `int RunPluginsTests();` and called after `RunStatusBarTests`. Add the file to `studio-tests` in `CMakeLists.txt`:

```cpp
#include "ide/IdeLayout.hpp"
#include "ide/PluginLoader.hpp"

#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Script.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

// Plugins in the studio: Save as Plugin writes the file, and the folder's poll loads it.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

std::filesystem::path TempConfig() {
    std::random_device device;
    return std::filesystem::temp_directory_path() / ("ae-plugins-" + std::to_string(device()));
}

}  // namespace

int RunPluginsTests() {
    const std::filesystem::path config = TempConfig();
    std::filesystem::create_directories(config);
    {
        ide::IdeLayout layout(1280, 800, config);
        auto scene = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
        layout.mount(*scene);
        layout.start();
        double time = 0.1;
        auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                scene->layout(1280, 800, time);
                layout.flush_frame_for_tests();
                time += 0.02;
            }
        };
        engine_core::InstanceId folder = 0;
        layout.run_now([&](engine_core::DataModel& game) {
            engine_core::Folder& made = game.create<engine_core::Folder>();
            game.set_name(made.id(), "Hello Tool");
            game.set_parent(made.id(), game.scene_service("Workspace"));
            engine_core::Script& script = game.create<engine_core::Script>();
            game.set_name(script.id(), "init");
            script.set_source("_G.helloLoaded = (_G.helloLoaded or 0) + 1");
            game.set_parent(script.id(), made.id());
            folder = made.id();
        });
        layout.save_as_plugin(folder);
        frames(3);
        Expect(std::filesystem::exists(config / "plugins" / "Hello Tool.aeplugin"), "Save as Plugin writes the file");
        bool loaded = false;
        layout.run_now([&](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(game.core())) {
                loaded = loaded || game.name(id) == "Hello Tool";
            }
        });
        Expect(loaded, "the saved plugin is loaded into Core without waiting for the poll");

        std::filesystem::remove(config / "plugins" / "Hello Tool.aeplugin");
        layout.poll_plugins(true);
        frames(2);
        bool gone = true;
        layout.run_now([&](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(game.core())) {
                gone = gone && game.name(id) != "Hello Tool";
            }
        });
        Expect(gone, "deleting the file unloads the plugin");
    }
    std::error_code error;
    std::filesystem::remove_all(config, error);
    return gFailures;
}
```

If `IdeLayout` has no public `flush_frame_for_tests()`, add one that calls `flushFrame()`. It has the same shape as the other test hooks in `IdeLayout.hpp`; check there for an existing one before adding it. `save_as_plugin` skips the replace prompt when the file does not exist, which is the case here.

- [ ] **Step 2: Run them to see them fail.** Build `sandbox` and `studio-tests`. Expected: compile errors for `SaveAsPlugin`, `save_as_plugin`, and `poll_plugins`.

- [ ] **Step 3: Implement the action**
  - `DataModel.hpp`: `enum class InstanceAction { Edit, Cut, Copy, Paste, Duplicate, Rename, Delete, SaveAsPlugin };`
  - `action_label`: `case InstanceAction::SaveAsPlugin: return "Save as Plugin";`
  - `Folder.hpp`: `void context_actions(std::vector<ContextAction>& out) const override;`
  - `Folder.cpp`:
    ```cpp
    void Folder::context_actions(std::vector<ContextAction>& out) const {
        DataModel::context_actions(out);
        out.push_back(ContextAction{InstanceAction::SaveAsPlugin, false});
    }
    ```
  - `IdeExplorer.cpp` `ActionIcon`: `case InstanceAction::SaveAsPlugin: return "Export.png";`. `IdeAssets.cpp` `ActionIcon`: add `case InstanceAction::SaveAsPlugin:` to the `break` group. Assets never offers it.
  - `IdeLayoutEditing.cpp` `run_action`: `case engine_core::InstanceAction::SaveAsPlugin: save_as_plugin(id); break;`

- [ ] **Step 4: Implement the folder, polling, and saving in `IdeLayout`**

Constructor, next to `layout_file_`: `plugins_dir_ = config / "plugins";`.

`load_plugins()` (called from `start()`), after the built-ins: `poll_plugins(true);`.

`flushFrame()`, after the `check_pending_` block: `poll_plugins(false);`. On refocus (where `check_pending_ = true` is set), also set `plugin_poll_at_ = 0;` so the next poll runs at once.

```cpp
void IdeLayout::poll_plugins(bool now) {
    if (plugins_dir_.empty()) {
        return;
    }
    const double clock = scene_ != nullptr ? scene_->timeSeconds() : 0;
    if (!now && clock < plugin_poll_at_) {
        return;
    }
    plugin_poll_at_ = clock + 1.0;
    std::vector<PluginStamp> stamps = scan_plugins(plugins_dir_);
    if (stamps == plugin_stamps_) {
        return;
    }
    plugin_stamps_ = stamps;
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    runner_.simulation().on_simulation([this, &scripts, stamps = std::move(stamps)](engine_core::DataModel& game) {
        plugins_.sync_user(game, scripts, stamps);
    });
}

void IdeLayout::save_as_plugin(std::uint32_t folder) {
    if (plugins_dir_.empty()) {
        show_toast("Plugins need a config folder");
        return;
    }
    std::vector<CopiedNode> copies;
    std::string name;
    run_now([&](engine_core::DataModel& game) {
        if (dynamic_cast<const engine_core::Folder*>(game.instance(folder)) == nullptr) {
            return;
        }
        name = game.name(folder);
        copies.push_back(engine_core::copy_tree(game, folder));
    });
    if (copies.empty()) {
        return;
    }
    const std::string file_name = plugin_file_name(name);
    const std::filesystem::path path = plugins_dir_ / (file_name + kPluginExtension);
    auto write = [this, path, file_name, copies = std::move(copies)] {
        std::error_code made;
        std::filesystem::create_directories(plugins_dir_, made);
        std::string error;
        if (!engine_core::save_instance_file(path, copies, error)) {
            show_toast("Could not save plugin \"" + file_name + "\": " + error);
            return;
        }
        show_toast("Saved plugin \"" + file_name + "\"");
        poll_plugins(true);
    };
    if (!std::filesystem::exists(path)) {
        write();
        return;
    }
    const jadefx::ButtonType replace("Replace", jadefx::ButtonType::Data::OkDone);
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Confirmation, "",
                                                 std::vector<jadefx::ButtonType>{replace, jadefx::ButtonType::Cancel()});
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText("Replace plugin \"" + file_name + "\"?");
    alert->setOnClosed([replace, write](const jadefx::ButtonType* choice) {
        if (choice != nullptr && *choice == replace) {
            write();
        }
    });
    alerts_.push_back(alert);
    if (scene_ != nullptr) {
        alert->show(*scene_);
    }
}
```

Copy `IdeLayoutProject.cpp:296-315` for how the alert list is pruned. Use the same `alerts_` cleanup there, so a finished alert is let go.

File menu: after the existing "Open…"-style items, before "Quit", add

```cpp
    AddItem(*file, "Open Plugins Folder", "AssetFolder.png", 0, 0)->setOnAction([this](jadefx::ActionEvent&) {
        if (plugins_dir_.empty()) {
            return;
        }
        std::error_code made;
        std::filesystem::create_directories(plugins_dir_, made);
        reveal_folder(plugins_dir_);
    });
```

Check `AddItem`'s signature at `IdeLayoutInternal.hpp:259`. If 0 is not "no accelerator", pass whatever the existing accelerator-less items pass.

- [ ] **Step 5: Run the tests.** Run `build\Debug\sandbox.exe "[PL15]"` and `build\Debug\studio-tests.exe`. Expected: PL15 passes, and no `FAIL` lines from PluginsTest. The pre-existing flakes listed above may appear.

- [ ] **Step 6: Commit.** `git commit -am "Plugins: Save as Plugin, Open Plugins Folder, and folder polling"`

---

## Sub-project 3: `plugin` global, toolbars, ribbon tabs

### Task 5: `PluginUi`, the `plugin` global, `plugin.Name`, `plugin.Unloading`

**Files:**
- Create: `src/engine_core/PluginUi.hpp`, `src/engine_core/PluginUi.cpp` (add to the `engine_core` library in CMake)
- Create: `src/engine_core/PluginBindings.cpp` (Lua side; add to the `engine_core` library)
- Modify: `src/engine_core/ScriptRuntime.hpp/.cpp`: own a `PluginUi`; `register_plugin` adds and `unregister_plugin` removes; `new_thread` sets `plugin`; implement `fire_plugin_unloading`
- Modify: `src/engine_core/ScriptBindings.hpp/.cpp`: `kSignalPlugin`, meta names, `signal_of`/`signal_cause`/connect/wait branches; `open_plugin_api(state)` declared and called from `open_host_libraries`
- Test: `sandbox/plugin_tests.cpp` (append)

**Interfaces:**
- Produces (namespace `engine_core`, `PluginUi.hpp`):
  ```cpp
  struct PluginButtonState {
      std::uint32_t id = 0;
      std::string key, tooltip, icon, text;
      bool active = false;
      bool enabled = true;
  };
  struct PluginToolbarState {
      std::uint32_t id = 0;
      std::string plugin;   // the plugin's name
      std::string name;     // the toolbar's
      std::vector<PluginButtonState> buttons;
  };
  // The plugins' toolbars, buttons, and signals, which plugin scripts make and
  // the studio draws. SimulationThread writes; the UI reads toolbars() under the
  // DataModel read lock when revision() has moved.
  class PluginUi {
  public:
      void attach(EventQueue& events);
      void detach();
      void add_plugin(std::uint32_t serial, std::string name, bool builtin);
      // Drops its toolbars and releases its signals. Its connections must already be gone.
      void remove_plugin(std::uint32_t serial);
      const std::string* plugin_name(std::uint32_t serial) const;
      // A key a SignalUd of kind kSignalPlugin carries.
      std::uint32_t unloading_key(std::uint32_t serial) const;
      void fire_unloading(std::uint32_t serial);
      std::uint32_t create_toolbar(std::uint32_t serial, std::string name);
      // 0, with error, for a toolbar that is gone, a key used in it already, or a bad icon path.
      std::uint32_t create_button(std::uint32_t toolbar, std::string key, std::string tooltip, std::string icon,
                                  std::string text, std::string& error);
      const PluginButtonState* button(std::uint32_t id) const;
      std::uint32_t click_key(std::uint32_t button) const;
      bool set_active(std::uint32_t button, bool active);
      bool set_enabled(std::uint32_t button, bool enabled);
      // Fires Click. False for a button that is gone or disabled.
      bool click(std::uint32_t button);
      Signal* signal(std::uint32_t key);
      std::uint64_t revision() const { return revision_; }
      // Built-ins' toolbars first, then the user plugins' by plugin name, each in the order made.
      std::vector<PluginToolbarState> toolbars() const;
  };
  // True for "icons/<name>" with no "..", "\\", or empty name.
  bool plugin_icon_path_ok(const std::string& path);
  ```
- Produces (`ScriptRuntime`): `PluginUi& plugin_ui();` and `const PluginUi& plugin_ui() const;`. `fire_plugin_unloading(root)` is now real.
- Lua: `plugin.Name`, `plugin.Unloading`, `plugin:CreateToolbar(name)` (Task 6), `plugin:CreateDockWidget` (Task 8). `tostring(plugin)` is `"Plugin"`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("PL16 plugin is one object per plugin, with its Name; Play and Console have none", "[PL16]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    engine_core::ModuleScript& lib = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(lib.id(), "Lib");
    lib.set_source("return function() return plugin end");
    rig.game.set_parent(lib.id(), folder);
    add_script(rig.game, folder, "A", "_G.pa = plugin print(plugin.Name, tostring(plugin))");
    add_script(rig.game, folder, "B",
               "print(plugin == _G.pa, require(script.Parent.Lib)() == plugin)\n"
               "task.spawn(function() print('spawned', plugin == _G.pa) end)");
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(folder, "ToolsFile"));
    REQUIRE(texts(rig.runtime.drain_output()) ==
            std::vector<std::string>{"ToolsFile\tPlugin\n", "true\ttrue\n", "spawned\ttrue\n"});

    rig.runtime.run_chunk("print(plugin)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"nil\n"});
    add_script(rig.game, "Play", "print('play', plugin)");
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"play\tnil\n"});
    rig.game.stop_simulation();
}

TEST_CASE("PL17 Unloading runs before the plugin's threads stop", "[PL17]") {
    ScriptRig rig;
    TempDir dir;
    write_plugin(dir.path, "Bye", {"plugin.Unloading:Connect(function() print('unloading', plugin.Name) end)"});
    ide::PluginLoader loader;
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    rig.runtime.drain_output();
    std::filesystem::remove(dir.path / "Bye.aeplugin");
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"unloading\tBye\n"});
}

TEST_CASE("PL18 icon paths stay inside icons/", "[PL18]") {
    REQUIRE(engine_core::plugin_icon_path_ok("icons/Brush.png"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("Brush.png"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("icons/../shaders/x.png"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("icons/"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("icons\\x.png"));
}
```

Check the start and stop names in `support.hpp` and `game_services_tests.cpp`, and use whatever the other play tests call (for example `rig.game.start_simulation()`).

- [ ] **Step 2: Run them to see them fail.** Expected: compile errors for the missing `PluginUi.hpp` and `plugin_icon_path_ok`.

- [ ] **Step 3: Implement `PluginUi`**

The state is `std::map<std::uint32_t, Plugin> plugins_` (`Plugin{std::string name; bool builtin; std::uint32_t unloading_key;}`), `std::vector<Toolbar> toolbars_` (`Toolbar{std::uint32_t id, serial; std::string name; std::vector<PluginButtonState> buttons; std::vector<std::uint32_t> click_keys;}`), and `std::map<std::uint32_t, std::unique_ptr<Signal>> signals_` (keyed so `Signal` addresses stay stable). `next_id_` starts at 1 and is shared by toolbar, button, and signal ids. `revision_` is bumped on every change. `EventQueue* events_`.
- `make_signal()`: `auto s = std::make_unique<Signal>(); if (events_) events_->host_signal(s.get());`. Store it, and return its key.
- `release(key)`: `events_->release_signal(*it->second)`, then erase it.
- `fire_unloading(serial)`: `events_->emit_args(signal->id(), 0, {})`.
- `click(button)`: check that the button exists and is enabled, then `emit_args` on its click signal.
- `create_button`: reject `!plugin_icon_path_ok(icon) && !icon.empty()` with `"icon must be a path under icons/, such as icons/Brush.png"`. An empty icon is allowed and makes a text-only button. Also reject a duplicate `key` within the toolbar with `"a button with id \"" + key + "\" is in this toolbar already"`.
- `toolbars()`: sort a copy by `(builtin ? 0 : 1, plugin name, toolbar id)`.

`plugin_icon_path_ok`: `path.rfind("icons/", 0) == 0 && path.size() > 6 && path.find("..") == npos && path.find('\\') == npos`.

- [ ] **Step 4: Hook it into `ScriptRuntime`**
  - Add the member `PluginUi plugin_ui_;`. In `attach`, call `plugin_ui_.attach(game.events());`. Wherever the host signals are released (ScriptRuntime.cpp:229-236), call `plugin_ui_.detach();`, which releases every remaining signal.
  - `register_plugin`: after `plugins_.push_back(plugin)`, call `plugin_ui_.add_plugin(plugin.serial, plugin.name, game_->core_holds(root) && dynamic_cast<Script*>(game_->instance(root)) != nullptr);`. A Script root in Core is a built-in; a Folder root is a user plugin.
  - `unregister_plugin` and the dead-root sweeps: after each `kill_owned(plugin_, 0, serial)`, call `plugin_ui_.remove_plugin(serial)`.
  - `fire_plugin_unloading(root)`: `const std::uint32_t serial = plugin_serial(root); if (serial == 0) return; plugin_ui_.fire_unloading(serial); game_->events().drain();`
  - `new_thread`: after `set_script_global(co, script);`, add `if (vm.kind == VmKind::Plugin) set_plugin_global(co, generation);`.
  - `set_plugin_global(co, serial)` pushes a `PluginUd{serial}` userdata with the `AE.Plugin` metatable, or nil when `plugin_ui_.plugin_name(serial)` is null, then calls `lua_setglobal(co, "plugin")`. Cache one userdata per serial in a registry table (`"AE.PluginCache"`, a weak-valued table like `kInstanceCache`), so `plugin == plugin` holds across threads.

- [ ] **Step 5: The Lua side, in `PluginBindings.cpp`**

In `ScriptBindings.hpp`, add:

```cpp
inline constexpr const char* kPluginMeta = "AE.Plugin";
inline constexpr const char* kPluginCache = "AE.PluginCache";
inline constexpr const char* kPluginToolbarMeta = "AE.PluginToolbar";
inline constexpr const char* kPluginButtonMeta = "AE.PluginToolbarButton";
struct PluginUd { std::uint32_t serial = 0; };
struct PluginObjectUd { std::uint32_t id = 0; };   // a toolbar or a button
// A PluginUi signal, found by its key in id.
constexpr int kSignalPlugin = 5;
void open_plugin_api(lua_State* state);
void push_plugin_signal(lua_State* state, std::uint32_t key, const char* cause);
```

Extend `SignalUd` handling:
- `signal_of`: `else if (ud.kind == kSignalPlugin) { signal = runtime.plugin_ui().signal(ud.id); }`.
- `signal_cause`: `if (ud.kind == kSignalPlugin) return ud.event_name != nullptr ? ud.event_name : "event";`.
- The three `kind == kSignalHost` argument branches in `signal_connect`/`signal_wait` (ScriptBindings.cpp:786, :852) become `kind == kSignalHost || kind == kSignalPlugin`.

`push_plugin_signal` makes a `SignalUd{kind = kSignalPlugin, id = key, event_name = cause}` with the `kSignalMeta` metatable. `cause` must be a string literal ("Unloading", "Click").

`plugin_index(state)`:

```cpp
int plugin_index(lua_State* state) {
    auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
    const char* key = luaL_checkstring(state, 2);
    ScriptRuntime* runtime = runtime_from(state);
    const std::string* name = runtime->plugin_ui().plugin_name(ud->serial);
    if (name == nullptr) {
        luaL_error(state, "the plugin has unloaded");
    }
    if (std::strcmp(key, "Name") == 0) {
        lua_pushstring(state, name->c_str());
        return 1;
    }
    if (std::strcmp(key, "Unloading") == 0) {
        push_plugin_signal(state, runtime->plugin_ui().unloading_key(ud->serial), "Unloading");
        return 1;
    }
    if (std::strcmp(key, "CreateToolbar") == 0) {
        lua_pushcfunction(state, plugin_create_toolbar, "CreateToolbar");
        return 1;
    }
    luaL_error(state, "%s is not a valid member of Plugin", key);
}
```

Writes are refused (`__newindex` raises "%s cannot be assigned to"), and `__tostring` returns "Plugin". `open_plugin_api` installs the three metatables and creates the weak cache table, the same way `RaycastBindings.cpp`'s `install` does.

- [ ] **Step 6: Run the tests.** Run `build\Debug\sandbox.exe "[PL16],[PL17],[PL18]"`, then the whole sandbox (`build\Debug\sandbox.exe`). Expected: all pass, apart from the known flakes.

- [ ] **Step 7: Commit.** `git commit -am "Plugins: the plugin global, Name, and Unloading"`

### Task 6: Toolbars and buttons from Lua

**Files:**
- Modify: `src/engine_core/PluginBindings.cpp`
- Test: `sandbox/plugin_tests.cpp` (append)

**Interfaces:**
- Consumes: `PluginUi::create_toolbar/create_button/set_active/set_enabled/click/click_key/button/toolbars/revision` from Task 5.
- Lua: `plugin:CreateToolbar(name) -> PluginToolbar`; `toolbar:CreateButton(id, tooltip, icon, text) -> PluginToolbarButton`; `button.Click` (signal); `button:SetActive(bool)`; `button.Enabled` (read and write); `button.Name` (its id, read-only).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("PL19 toolbars and buttons show in PluginUi, and a click fires Click", "[PL19]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main",
               "local tb = plugin:CreateToolbar('Terrain Tools')\n"
               "local b = tb:CreateButton('Smooth', 'Smooth it', 'icons/Brush.png', 'Smooth')\n"
               "b.Click:Connect(function() print('clicked', b.Name) b:SetActive(true) end)\n"
               "local ok, err = pcall(function() tb:CreateButton('Smooth', '', '', 'Again') end)\n"
               "print(ok, string.find(err, 'already') ~= nil)\n"
               "ok, err = pcall(function() tb:CreateButton('Bad', '', '../x.png', 'Bad') end)\n"
               "print(ok, string.find(err, 'icons/') ~= nil)\n"
               "local off = tb:CreateButton('Off', '', '', 'Off') off.Enabled = false\n"
               "off.Click:Connect(function() print('never') end)");
    rig.runtime.drain_output();
    const std::uint64_t before = rig.runtime.plugin_ui().revision();
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"false\ttrue\n", "false\ttrue\n"});
    REQUIRE(rig.runtime.plugin_ui().revision() != before);

    std::vector<engine_core::PluginToolbarState> bars = rig.runtime.plugin_ui().toolbars();
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].name == "Terrain Tools");
    REQUIRE(bars[0].plugin == "Tools");
    REQUIRE(bars[0].buttons.size() == 2);
    REQUIRE(bars[0].buttons[0].icon == "icons/Brush.png");
    REQUIRE_FALSE(bars[0].buttons[1].enabled);

    REQUIRE(rig.runtime.plugin_ui().click(bars[0].buttons[0].id));
    REQUIRE_FALSE(rig.runtime.plugin_ui().click(bars[0].buttons[1].id));
    rig.game.events().drain();
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"clicked\tSmooth\n"});
    REQUIRE(rig.runtime.plugin_ui().toolbars()[0].buttons[0].active);

    REQUIRE(rig.runtime.unregister_plugin(folder));
    REQUIRE(rig.runtime.plugin_ui().toolbars().empty());
}

TEST_CASE("PL20 two plugins with one toolbar name keep two groups, each going with its plugin", "[PL20]") {
    ScriptRig rig;
    const InstanceId a = add_folder(rig.game, "A", rig.game.core());
    add_script(rig.game, a, "M", "plugin:CreateToolbar('Tools'):CreateButton('x', '', '', 'X')");
    const InstanceId b = add_folder(rig.game, "B", rig.game.core());
    add_script(rig.game, b, "M", "plugin:CreateToolbar('Tools'):CreateButton('y', '', '', 'Y')");
    REQUIRE(rig.runtime.register_plugin(b, "B"));
    REQUIRE(rig.runtime.register_plugin(a, "A"));
    std::vector<engine_core::PluginToolbarState> bars = rig.runtime.plugin_ui().toolbars();
    REQUIRE(bars.size() == 2);
    REQUIRE(bars[0].plugin == "A");  // by plugin name, not registration order
    REQUIRE(rig.runtime.unregister_plugin(a));
    bars = rig.runtime.plugin_ui().toolbars();
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].plugin == "B");
}
```

- [ ] **Step 2: Run them to see them fail.** Expected: `CreateToolbar is not a valid member` errors in the output, so the assertions fail.

- [ ] **Step 3: Implement**

`plugin_create_toolbar(state)`: check the `PluginUd` at index 1 and the string name at index 2. Call `runtime->plugin_ui().create_toolbar(serial, name)` and push a `PluginObjectUd{id}` with `kPluginToolbarMeta`.

`toolbar_index`: `"CreateButton"` returns a cfunction. Anything else is an error.
`toolbar_create_button`: takes `(self, id, tooltip, icon, text)`. All four are strings; `text` defaults to `id` when it's nil. If `create_button` returns 0, call `luaL_error(state, "%s", error.c_str())`. Otherwise push a `PluginObjectUd` with `kPluginButtonMeta`.

`button_index`:
- `"Name"` returns the button's key.
- `"Enabled"` returns `button(id)->enabled`.
- `"Click"` returns `push_plugin_signal(state, click_key(id), "Click")`.
- `"SetActive"` returns a cfunction that reads `lua_toboolean(state, 2)` and calls `set_active`.
- A button that is gone gives the error "the button's plugin has unloaded".

`button_newindex`: `"Enabled"` takes a boolean (anything else is the error "Enabled must be true or false") and calls `set_enabled`. Any other key gives "%s cannot be assigned to".

- [ ] **Step 4: Run the tests.** Run `build\Debug\sandbox.exe "[PL19],[PL20],[PL16]"`. Expected: all pass.

- [ ] **Step 5: Commit.** `git commit -am "Plugins: toolbars and buttons"`

### Task 7: Tabbed ribbon with a Plugins tab

**Files:**
- Create: `src/ide/PluginRibbon.hpp`, `src/ide/PluginRibbon.cpp` (add to `STUDIO_SOURCES`)
- Modify: `src/ide/IdeLayout.cpp:68-100` (the ribbon block), `src/ide/IdeLayout.hpp` (members), `src/ide/IdeLayoutInternal.hpp` (`kStylesheet` rules; `RibbonButton` gains `setActive`)
- Modify: `src/ide/IdeLayout.cpp` `flushFrame`: refresh the toolbars when the revision moves
- Test: `tests/PluginsTest.cpp` (extend `RunPluginsTests`)

**Interfaces:**
- Produces (namespace `ide`):
  ```cpp
  // The ribbon's tab row (Home, Plugins) over one content row at a time. Home
  // is the row the shell builds; Plugins is built from the plugins' toolbars.
  class PluginRibbon : public jadefx::VBox {
  public:
      PluginRibbon(std::shared_ptr<jadefx::Node> home, std::function<void(std::uint32_t button)> click);
      void setToolbars(const std::vector<engine_core::PluginToolbarState>& toolbars);
      void showTab(int index);   // 0 Home, 1 Plugins
      int tab() const;
      // For tests: the Plugins row and a button's node by its PluginButtonState id.
      jadefx::Node* pluginsRow() const;
      jadefx::Node* buttonNode(std::uint32_t id) const;
  };
  ```
- `RibbonButton` gains `void setActive(bool on)`, which toggles the class `ide-ribbon-button-on` (the same look as the Grid toggle when it's lit; reuse that class if the Grid toggle already has one), and `void setEnabledLook(bool on)`, which sets the `disabled` look and ignores clicks.

- [ ] **Step 1: Write the failing test** (append inside `RunPluginsTests` before the layout leaves scope)

```cpp
        // A toolbar from a plugin shows on the Plugins tab, and its click reaches the script.
        layout.run_now([&](engine_core::DataModel& game) {
            engine_core::Folder& made = game.create<engine_core::Folder>();
            game.set_name(made.id(), "Ribbon Tool");
            game.set_parent(made.id(), game.scene_service("Workspace"));
            engine_core::Script& script = game.create<engine_core::Script>();
            script.set_source(
                "local b = plugin:CreateToolbar('Bar'):CreateButton('Go', 'Go now', 'icons/Play.png', 'Go')\n"
                "b.Click:Connect(function() _G.ribbonClicks = (_G.ribbonClicks or 0) + 1 b:SetActive(true) end)");
            game.set_parent(script.id(), made.id());
            folder = made.id();
        });
        layout.save_as_plugin(folder);
        frames(4);
        ide::PluginRibbon* ribbon = layout.plugin_ribbon_for_tests();
        Expect(ribbon != nullptr, "the studio has a tabbed ribbon");
        ribbon->showTab(1);
        frames(2);
        std::uint32_t buttonId = 0;
        layout.run_now([&](engine_core::DataModel&) {
            const auto bars = layout.simulation().scripts().plugin_ui().toolbars();
            if (!bars.empty() && !bars[0].buttons.empty()) {
                buttonId = bars[0].buttons[0].id;
            }
        });
        jadefx::Node* go = ribbon->buttonNode(buttonId);
        Expect(go != nullptr, "the plugin's button is on the Plugins tab");
        if (go != nullptr) {
            const double x = go->getAbsoluteX() + go->getWidth() / 2;
            const double y = go->getAbsoluteY() + go->getHeight() / 2;
            scene->noteMouseMove(x, y);
            scene->noteMouseButton(0, true, x, y);
            scene->noteMouseButton(0, false, x, y);
            frames(4);
            Expect(go->getClassList().contains("ide-ribbon-button-on"), "SetActive from the Click handler lights it");
        }
```

The mouse calls must match `Scene`'s injection API. Use whatever `tests/ExplorerRenameTest.cpp` uses to click.

- [ ] **Step 2: Run it to see it fail.** Expected: a compile error, since there's no `PluginRibbon` or `plugin_ribbon_for_tests`.

- [ ] **Step 3: Implement `PluginRibbon`**
  - The tab row is an `HBox` with class `ide-ribbon-tabs`. It holds two `Label`s, "Home" and "Plugins", each with class `ide-ribbon-tab`; the one showing adds `ide-ribbon-tab-on`. Clicking a label calls `showTab`.
  - Below it is the content: the `home` node or the plugins row. Swap them with `getChildren().set(1, ...)`.
  - The plugins row is an `HBox` with class `ide-ribbon` (the same look as Home), rebuilt by `setToolbars` only when the list differs from the last one. Compare the ids, active, enabled, text, and icon fields.
  - Each toolbar is a `VBox` group (class `ide-ribbon-group`). It holds an `HBox` of `RibbonButton`s and a caption `Label` (class `ide-ribbon-caption`) with the toolbar's name. A `Separator` (or a 1px `Pane` with class `ide-ribbon-separator`) follows each group.
  - Each button is `RibbonButton(text, icon_without_prefix, [click, id] { click(id); })`. `icon_without_prefix` strips `"icons/"`, because `icon_graphic` resolves a bare file name under `resources/icons`. If `icon_graphic` returns null for a non-empty icon, write one Console warning per path: `"Plugin icon " + icon + " was not found"`. `PluginRibbon` takes a `std::function<void(const std::string&)> warn` for this, so it doesn't reach into the engine itself.
  - `Tooltip::install` uses the tooltip text.
  - With no toolbars, the row shows one `Label` with class `ide-ribbon-empty`: "No plugins installed — right-click a Folder → Save as Plugin".

Add the CSS rules to `kStylesheet` next to `.ide-ribbon`. Use only `var(--ide-*)` variables already in use there (copy the hover and on colours from `.ide-ribbon-button` and the grid toggle). Keep the tab row 22px tall so the total height grows by that much.

- [ ] **Step 4: Wire it into `IdeLayout`**

In the constructor, keep building `ribbon` (the HBox) exactly as now. Then wrap it:

```cpp
    auto tabs = jadefx::make<PluginRibbon>(ribbon, [this](std::uint32_t button) {
        runner_.simulation().on_simulation([this, button](engine_core::DataModel&) {
            runner_.simulation().scripts().plugin_ui().click(button);
        });
    }, [this](const std::string& text) {
        runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Warning, text);
    });
    plugin_ribbon_ = tabs.get();
```

Wherever `ribbon` is added to `top`, add `tabs` in its place. Check `OutputKind` for the exact warning value. If there is none, use `Error`.

In `flushFrame`, add:

```cpp
    {
        engine_core::Engine& engine = runner_.simulation();
        engine_core::DataModelLock lock(engine.datamodel(), engine_core::DataModelLock::Read, std::chrono::milliseconds(1));
        if (lock.owns() && plugin_ribbon_ != nullptr) {
            const engine_core::PluginUi& ui = engine.scripts().plugin_ui();
            if (ui.revision() != plugin_ui_revision_) {
                plugin_ui_revision_ = ui.revision();
                plugin_ribbon_->setToolbars(ui.toolbars());
            }
        }
    }
```

Add the members `PluginRibbon* plugin_ribbon_ = nullptr; std::uint64_t plugin_ui_revision_ = 0;` and a public `PluginRibbon* plugin_ribbon_for_tests() { return plugin_ribbon_; }`.

The built-in SceneCamera and MoveTool create no toolbars, so the Plugins tab shows the empty hint until a user plugin adds one.

- [ ] **Step 5: Run the tests.** Run `build\Debug\studio-tests.exe`. Expected: no new `FAIL` lines, and the existing layout tests still find the menu bar, ribbon, and Grid toggle. If a test reaches the ribbon by child index, update that index and note it in the commit.

- [ ] **Step 6: Commit.** `git commit -am "Ribbon: Home and Plugins tabs, with plugin toolbars"`

---

## Sub-project 4: Dock widgets

### Task 8: The `DockWidget` instance and `plugin:CreateDockWidget`

**Files:**
- Modify: `src/engine_instances/Gui.hpp` (`GuiProperty::Title`, `GuiProperty::WidgetEnabled`; class `DockWidget`), `src/engine_instances/Gui.cpp` (specs, defaults, the class, Lua registration)
- Modify: `src/engine_core/PluginBindings.cpp` (`CreateDockWidget`)
- Test: `sandbox/plugin_tests.cpp` (append)

**Interfaces:**
- Produces (namespace `engine_core`):
  ```cpp
  enum class DockSide { Left, Right, Bottom, Float };
  // A plugin's dockable window, made by plugin:CreateDockWidget under the
  // plugin's root. Its GuiBase children draw in a studio pane as a ScreenGui's
  // draw over the view. Title and Enabled are its Lua properties; the rest is
  // fixed at creation. Never saved: it lives in Core.
  class DockWidget : public GuiBase {
  public:
      DockWidget(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
      const char* class_name() const override;  // "DockWidget"
      void set_origin(std::string plugin, std::string key);
      const std::string& plugin() const;
      const std::string& key() const;
      // "plugin:<plugin>/<key>"
      std::string pane_name() const;
      DockSide initial_dock = DockSide::Right;
      double width = 300, height = 400, min_width = 0, min_height = 0;
      const std::string& title() const { return text(GuiProperty::Title); }
      bool enabled() const { return flag(GuiProperty::WidgetEnabled); }
  };
  ```
- `GuiProperty` gains `Title` (`"Title"`, string, default `""`) and `WidgetEnabled` (registry name `"Enabled"`, boolean, default `false`), appended before `Count`, with matching rows in `kSpecs`.
- Lua: `plugin:CreateDockWidget(id, options?) -> DockWidget`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "Gui.hpp"

TEST_CASE("PL21 CreateDockWidget makes a DockWidget under the plugin, once per id", "[PL21]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main",
               "local w = plugin:CreateDockWidget('Panel', {Title = 'Terrain', InitialDock = 'Bottom', Enabled = true,"
               " Width = 250, Height = 120})\n"
               "print(w.ClassName, w.Parent == script.Parent, w.Title, w.Enabled)\n"
               "local d = plugin:CreateDockWidget('Plain')\n"
               "print(d.Title, d.Enabled)\n"
               "print(pcall(function() plugin:CreateDockWidget('Panel') end))\n"
               "print(pcall(function() plugin:CreateDockWidget('X', {InitialDock = 'Up'}) end))\n"
               "w.Enabled = false print(w.Enabled)");
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    const std::vector<std::string> out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 5);
    REQUIRE(out[0] == "DockWidget\ttrue\tTerrain\ttrue\n");
    REQUIRE(out[1] == "Plain\tfalse\n");
    REQUIRE(out[2].rfind("false\t", 0) == 0);
    REQUIRE(out[2].find("already") != std::string::npos);
    REQUIRE(out[3].find("InitialDock") != std::string::npos);
    REQUIRE(out[4] == "false\n");

    const InstanceId panel = rig.game.find_first_child(folder, "Panel");
    const auto* widget = dynamic_cast<const engine_core::DockWidget*>(rig.game.instance(panel));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->pane_name() == "plugin:Tools/Panel");
    REQUIRE(widget->initial_dock == engine_core::DockSide::Bottom);
    REQUIRE(widget->width == 250);
}

TEST_CASE("PL22 a BillboardGui inside a DockWidget is not drawn in the world", "[PL22]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main",
               "local w = plugin:CreateDockWidget('Panel')\n"
               "local b = Instance.new('BillboardGui') b.Name = 'Board' b.Parent = w");
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    const InstanceId board = rig.game.find_first_child(rig.game.find_first_child(folder, "Panel"), "Board");
    const auto* gui = dynamic_cast<const engine_core::BillboardGui*>(rig.game.instance(board));
    REQUIRE(gui != nullptr);
    REQUIRE_FALSE(gui->drawn());
}
```

- [ ] **Step 2: Run them to see them fail.** Expected: a compile error for `DockWidget`.

- [ ] **Step 3: Implement the class**
  - `kSpecs`: append `{"Title", "string", LuaSlot::Kind::String, 0, 0}` and `{"Enabled", "boolean", LuaSlot::Kind::Bool, 0, 0}`.
  - `default_value`: `case GuiProperty::Title: return string_slot("");` and `case GuiProperty::WidgetEnabled: return bool_slot(false);`.
  - The `DockWidget` constructor calls `reset_values()`. `class_name()` returns `"DockWidget"`. `pane_name()` returns `"plugin:" + plugin_ + "/" + key_`.
  - Registration in `register_gui_lua`:
    ```cpp
    const LuaField dock[] = {gui_field<GuiProperty::Title>("DockWidget"), gui_field<GuiProperty::WidgetEnabled>("DockWidget")};
    add_class("DockWidget", "GuiBase", dock);
    register_suited_parents("GuiBasePane", {"ScreenGui", "BillboardGui", "GuiBasePane", "DockWidget"});
    ```
    The existing `GuiBasePane` and controls `register_suited_parents` calls each add `"DockWidget"` to their lists, rather than being called twice. Don't call `register_lua_creatable`: `Instance.new("DockWidget")` must fail.

- [ ] **Step 4: `CreateDockWidget`**

In `plugin_index`, `"CreateDockWidget"` returns the cfunction `plugin_create_dock_widget`:

```cpp
int plugin_create_dock_widget(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
        const std::string key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        const std::string* name = runtime->plugin_ui().plugin_name(ud->serial);
        const InstanceId root = runtime->plugin_root(ud->serial);   // add: the registered root for a serial, or 0
        if (name == nullptr || root == 0) {
            luaL_error(state, "the plugin has unloaded");
        }
        DataModel& game = *runtime->game_;
        for (InstanceId child = game.first_child(root); child != 0; child = game.next_sibling(child)) {
            const auto* other = dynamic_cast<const DockWidget*>(game.instance(child));
            if (other != nullptr && other->key() == key) {
                luaL_error(state, "a dock widget with id \"%s\" exists already", key.c_str());
            }
        }
        DockWidget& widget = game.create<DockWidget>();
        widget.set_origin(*name, key);
        game.set_name(widget.id(), key);
        std::string title = key;
        bool enabled = false;
        if (lua_istable(state, 3)) {
            lua_getfield(state, 3, "Title");
            if (lua_isstring(state, -1)) title = lua_tostring(state, -1);
            lua_pop(state, 1);
            lua_getfield(state, 3, "Enabled");
            if (lua_isboolean(state, -1)) enabled = lua_toboolean(state, -1);
            lua_pop(state, 1);
            lua_getfield(state, 3, "InitialDock");
            if (!lua_isnil(state, -1)) {
                const std::string side = luaL_checkstring(state, -1);
                if (side == "Left") widget.initial_dock = DockSide::Left;
                else if (side == "Right") widget.initial_dock = DockSide::Right;
                else if (side == "Bottom") widget.initial_dock = DockSide::Bottom;
                else if (side == "Float") widget.initial_dock = DockSide::Float;
                else { game.destroy(widget.id()); luaL_error(state, "InitialDock must be Left, Right, Bottom, or Float"); }
            }
            lua_pop(state, 1);
            auto number = [&](const char* field, double& out) {
                lua_getfield(state, 3, field);
                if (lua_isnumber(state, -1)) out = std::max(0.0, lua_tonumber(state, -1));
                lua_pop(state, 1);
            };
            number("Width", widget.width);
            number("Height", widget.height);
            number("MinWidth", widget.min_width);
            number("MinHeight", widget.min_height);
        }
        widget.set_value(GuiProperty::Title, /* string slot */ ...);
        widget.set_value(GuiProperty::WidgetEnabled, /* bool slot */ ...);
        game.set_parent(widget.id(), root);
        runtime->push_instance(state, widget.id());
        return 1;
    });
}
```

Write the two `set_value` calls with the existing `LuaSlot` helpers: `string_slot` and `bool_slot` are file-local in `Gui.cpp`. Either add public `set_title(std::string)` and `set_enabled(bool)` on `DockWidget` that call `set_value` internally (preferred), or build the `LuaSlot` inline. Also add `InstanceId ScriptRuntime::plugin_root(std::uint32_t serial) const;`. Note that `luaL_error` inside `lua_guard` long-jumps, so destroy the half-made widget before raising, as shown.

- [ ] **Step 5: Run the tests.** Run `build\Debug\sandbox.exe "[PL21],[PL22]"` and `build\Debug\sandbox.exe "[gui]"` (the existing GUI tests; adding properties must not break them). Expected: all pass.

- [ ] **Step 6: Commit.** `git commit -am "Plugins: DockWidget and plugin:CreateDockWidget"`

### Task 9: `runner::GuiTree`, extracted from `GuiLayer`

**Files:**
- Create: `src/runner/GuiTree.hpp`, `src/runner/GuiTree.cpp` (add to `STUDIO_SOURCES` next to `GuiLayer.cpp`)
- Modify: `src/runner/GuiLayer.hpp`, `src/runner/GuiLayer.cpp`
- Test: the existing `tests/GuiStyleTest.cpp`, `tests/GuiImageTest.cpp`, and `tests/BillboardLayerTest.cpp` must pass unchanged; new `tests/GuiTreeTest.cpp` (studio-tests)

**Interfaces:**
- Produces (namespace `runner`):
  ```cpp
  // The JadeFX nodes for GUI instances, made once and updated in place: each
  // GuiBase a node of its class's element type, its CSS children its
  // stylesheet. Mouse events fire the instance's events on the simulation
  // thread; typing writes a TextField's Text. A pass reads under the
  // DataModel read lock: beginPass, build each root, endPass. updateImages
  // runs after, without the lock.
  class GuiTree {
  public:
      GuiTree(engine_core::Engine& engine, std::shared_ptr<GuiInput> input,
              std::function<std::filesystem::path()> resourcesRoot);
      ~GuiTree();
      void beginPass();
      std::shared_ptr<jadefx::Node> build(engine_core::InstanceId id, const engine_core::GuiValues& gui);
      void endPass();
      void updateImages();
      jadefx::Node* nodeFor(engine_core::InstanceId id) const;
      bool visible(engine_core::InstanceId id) const;
      bool mouseTransparent(engine_core::InstanceId id) const;
  };
  ```
- `makeNode` gains `"DockWidget"`, which makes a `GuiNode<jadefx::StackPane>("dockwidget", ...)` with pick-on-bounds on. `build`'s child loop accepts a `ScreenGui` child when the parent's class is `DockWidget`.

- [ ] **Step 1: Write the failing test** (`tests/GuiTreeTest.cpp`, registered as `int RunGuiTreeTests(ide::IdeLayout&, jadefx::Scene&);`)

```cpp
// GuiTree alone: a DockWidget's subtree as nodes, and the nodes following edits.
int RunGuiTreeTests(ide::IdeLayout& layout, jadefx::Scene&) {
    engine_core::Engine& engine = layout.simulation();
    engine_core::InstanceId widget = 0, pane = 0, label = 0;
    layout.run_now([&](engine_core::DataModel& game) {
        auto& w = game.create<engine_core::DockWidget>();
        w.set_origin("T", "W");
        game.set_parent(w.id(), game.core());
        auto& p = game.create<engine_core::Pane>();
        game.set_name(p.id(), "Body");
        game.set_parent(p.id(), w.id());
        auto& l = game.create<engine_core::Label>();
        game.set_parent(l.id(), p.id());
        widget = w.id(); pane = p.id(); label = l.id();
    });
    runner::GuiTree tree(engine, std::make_shared<runner::GuiInput>(), [] { return std::filesystem::path(); });
    auto pass = [&] {
        engine_core::DataModelLock lock(engine.datamodel(), engine_core::DataModelLock::Read);
        tree.beginPass();
        const auto* gui = dynamic_cast<const engine_core::GuiValues*>(engine.datamodel().instance(widget));
        std::shared_ptr<jadefx::Node> root = gui != nullptr ? tree.build(widget, *gui) : nullptr;
        tree.endPass();
        return root;
    };
    std::shared_ptr<jadefx::Node> root = pass();
    Expect(root != nullptr && std::string(root->getElementType()) == "dockwidget", "a DockWidget is a dockwidget node");
    Expect(tree.nodeFor(label) != nullptr, "the label inside is built");
    Expect(tree.nodeFor(pane) != nullptr && tree.nodeFor(pane)->getElementId() == "Body", "Name is the CSS id");

    // Reparenting the pane out of the widget drops its nodes on the next pass.
    layout.run_now([&](engine_core::DataModel& game) { game.set_parent(pane, game.core()); });
    pass();
    Expect(tree.nodeFor(pane) == nullptr && tree.nodeFor(label) == nullptr, "moved-out GUI leaves the tree");
    layout.run_now([&](engine_core::DataModel& game) { game.destroy(widget); game.destroy(pane); });
    return gFailures;
}
```

(Use the same `gFailures`/`Expect` preamble as `PluginsTest.cpp`.)

- [ ] **Step 2: Run it to see it fail.** Expected: a compile error, since there's no `GuiTree.hpp`.

- [ ] **Step 3: Move the code**

Move these from `GuiLayer.cpp` into `GuiTree.cpp`, keeping their bodies as they are:
- the anonymous-namespace helpers (`GuiNode`, `SplitClasses`, `AlignmentPos`, `NodeColor`, `IsPaneClass`, `kImageRecheck`, `loadFlipped`)
- `struct Entry`
- `makeNode`, `build`, `apply`, `fire`, `writeText`, `updateImages`, `loadImage`
- the `LoadedImage` cache members

Then make these changes:
- `beginPass()`: `++pass_; resourcesRoot_ = resourcesRootFn_();`. This replaces `syncTree`'s first two lines.
- `endPass()`: the "What is no longer drawn lets go of its children and goes" loop from the end of `syncTree`.
- `visible(id)` and `mouseTransparent(id)` read the entry. `placeBillboards` uses them instead of `entries_`.
- `GuiLayer` keeps `std::unique_ptr<GuiTree> tree_`, constructed with `input_` and `[this] { return game_.resources_root(); }`. `syncTree` calls `tree_->beginPass()`, `collectScreens`/`collectBillboards` call `tree_->build(...)`, and `syncTree` calls `tree_->endPass()` at the end. `sync()` calls `tree_->updateImages()`, and `nodeFor` forwards to the tree.
- `GuiNode`'s `input_` stays a `std::shared_ptr<GuiInput>`, and an empty `GuiInput` is fine because each call is guarded.

Add `DockWidget` to `makeNode`:

```cpp
    } else if (className == "DockWidget") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("dockwidget", input, false);
```

In `build`'s child loop, replace the ScreenGui exclusion with:

```cpp
            const bool screen = dynamic_cast<const engine_core::ScreenGui*>(object) != nullptr;
            const bool board = dynamic_cast<const engine_core::BillboardGui*>(object) != nullptr;
            // A ScreenGui or BillboardGui inside another GUI is drawn by neither,
            // except a ScreenGui directly in a DockWidget, which fills it.
            if (entry.container != nullptr && !board && (!screen || entry.className == "DockWidget")) {
```

- [ ] **Step 4: Run the tests.** Run `build\Debug\studio-tests.exe`. Expected: the existing GuiStyle, GuiImage, and BillboardLayer checks pass, the new GuiTree checks pass, and there are no new `FAIL` lines.

- [ ] **Step 5: Commit.** `git commit -am "GuiTree: the GUI node builder, out of GuiLayer"`

### Task 10: `PluginWidgetPane` and IDE integration

**Files:**
- Create: `src/ide/PluginWidgetPane.hpp`, `src/ide/PluginWidgetPane.cpp` (add to `STUDIO_SOURCES`)
- Modify: `src/ide/IdeLayout.hpp`, `src/ide/IdeLayout.cpp` (`flushFrame`: `sync_plugin_widgets()`), `src/ide/IdeLayoutDocking.cpp` (`layout_host`, `apply_layout`'s `dock_page`, the Window menu's Plugins submenu)
- Test: `tests/PluginsTest.cpp` (extend)

**Interfaces:**
- Produces (namespace `ide`):
  ```cpp
  // A plugin's DockWidget as a studio page: its GUI drawn by a GuiTree in the
  // studio's own scene, so the theme reaches it. Mouse and keys stay here.
  class PluginWidgetPane : public IdePane {
  public:
      PluginWidgetPane(engine_core::Engine& engine, engine_core::InstanceId widget, std::string paneName,
                       std::function<void()> closed);
      engine_core::InstanceId widget() const;
      // Reads the widget's subtree under a 1 ms read lock and updates the nodes.
      void sync();
      void onClose() override;   // calls closed
  protected:
      void layoutChildren() override;  // sync(), then StackPane::layoutChildren()
  };
  ```
- `IdeLayout` gains `void sync_plugin_widgets();`, `std::map<std::string, PluginWidget> plugin_widgets_;` (keyed by pane name, where `PluginWidget{engine_core::InstanceId id; WindowEntry* entry; std::string title; bool enabled;}`), `std::map<std::string, std::weak_ptr<IdeDock>> plugin_docks_;` (recorded docks), and `jadefx::Menu* plugins_menu_ = nullptr;`.

- [ ] **Step 1: Write the failing test** (extend `RunPluginsTests`)

```cpp
        // A dock widget opens as a page with the plugin's GUI, and closing its tab clears Enabled.
        layout.run_now([&](engine_core::DataModel& game) {
            engine_core::Folder& made = game.create<engine_core::Folder>();
            game.set_name(made.id(), "Widget Tool");
            game.set_parent(made.id(), game.scene_service("Workspace"));
            engine_core::Label& label = game.create<engine_core::Label>();
            game.set_name(label.id(), "Hello");
            game.set_parent(label.id(), made.id());
            engine_core::Script& script = game.create<engine_core::Script>();
            script.set_source(
                "local w = plugin:CreateDockWidget('Panel', {Title = 'Widget Tool', Enabled = true})\n"
                "script.Parent.Hello.Parent = w\n"
                "_G.widget = w");
            game.set_parent(script.id(), made.id());
            folder = made.id();
        });
        layout.save_as_plugin(folder);
        frames(6);
        ide::IdePane* page = layout.page_named_for_tests("plugin:Widget Tool/Panel");
        Expect(page != nullptr, "the widget is a studio page named plugin:<plugin>/<id>");
        Expect(page != nullptr && page->title() == "Widget Tool", "the page shows the widget's Title");
        Expect(page != nullptr && !page->getElementsByClassName("label").empty() /* or find by element id "Hello" */,
               "the plugin's GUI is drawn in the page");
        Expect(layout.page_open_for_tests(page), "Enabled = true opens it");

        layout.close_page_for_tests(page);
        frames(4);
        bool enabled = true;
        layout.run_now([&](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(game.core())) {
                if (game.name(id) == "Widget Tool") {
                    const auto* w = dynamic_cast<const engine_core::DockWidget*>(
                        game.instance(game.find_first_child(id, "Panel")));
                    enabled = w != nullptr && w->enabled();
                }
            }
        });
        Expect(!enabled, "closing the tab sets Enabled to false");

        // Reloading the plugin brings the page back.
        layout.run_now([&](engine_core::DataModel& game) {
            game.scene_service("Workspace");  // no-op; the file is rewritten below
        });
        layout.save_as_plugin(folder);  // the folder in Workspace no longer has Hello: it moved in the Core copy only
        frames(6);
        Expect(layout.page_named_for_tests("plugin:Widget Tool/Panel") != nullptr, "a reload makes the page again");
```

Find a page's label node with whichever lookup `GuiStyleTest.cpp` uses, such as `getElementById("Hello")` if JadeFX has it. Add the three small test hooks to `IdeLayout`:
- `page_named_for_tests(name)` returns `page_named(name).get()`.
- `page_open_for_tests(page)` returns `dockContaining(page) != nullptr`.
- `close_page_for_tests(page)` closes its tab the way a click on its close button does. Find how `toggle_window` closes a page.

- [ ] **Step 2: Run it to see it fail.** Expected: a compile error for the missing hooks and `PluginWidgetPane`.

- [ ] **Step 3: Implement `PluginWidgetPane`**

```cpp
PluginWidgetPane::PluginWidgetPane(engine_core::Engine& engine, engine_core::InstanceId widget, std::string paneName,
                                   std::function<void()> closed)
    : IdePane(std::move(paneName), true), engine_(engine), widget_(widget), closed_(std::move(closed)),
      tree_(std::make_unique<runner::GuiTree>(engine, std::make_shared<runner::GuiInput>(),
                                              [] { return find_resource("icons").parent_path(); })) {
    getClassList().add("plugin-widget");
    setIconFile("Plugin.png");  // falls back to no icon when resources/icons has none
}

void PluginWidgetPane::sync() {
    std::shared_ptr<jadefx::Node> root;
    {
        engine_core::DataModelLock lock(engine_.datamodel(), engine_core::DataModelLock::Read,
                                        std::chrono::milliseconds(1));
        if (!lock.owns()) {
            return;
        }
        tree_->beginPass();
        if (const auto* gui = dynamic_cast<const engine_core::GuiValues*>(engine_.datamodel().instance(widget_))) {
            root = tree_->build(widget_, *gui);
        }
        tree_->endPass();
    }
    tree_->updateImages();
    if (root.get() != shown_) {
        getChildren().clear();
        if (root) {
            Fill(*root);
            getChildren().add(root);
        }
        shown_ = root.get();
    }
}

void PluginWidgetPane::layoutChildren() {
    sync();
    IdePane::layoutChildren();
}

void PluginWidgetPane::onClose() {
    if (closed_) {
        closed_();
    }
}
```

`find_resource("icons")` gives `resources/icons`, and its parent is `resources/`, which is where a widget's ImagePane Texture paths resolve (spec §4.4). If an icon named `Plugin.png` doesn't exist, use `"Script.png"`.

- [ ] **Step 4: `IdeLayout::sync_plugin_widgets()`**, called from `flushFrame` after the ribbon refresh

1. Under a 1 ms read lock (skip the frame if the lock isn't free), walk `game.get_children(game.core())` and their children. Collect every `DockWidget` as `{pane_name, id, title, enabled, initial_dock, width, height}`.
2. For each pane name in `plugin_widgets_` that wasn't collected, the widget is gone:
   - If its page is docked, record the dock: `plugin_docks_[name] = <weak_ptr to dockContaining(page)>`. Find the `shared_ptr` in `docks_`.
   - Close its tab if it's open, without firing `closed`: clear the pane's callback first.
   - Erase its `WindowEntry` from `windows_`, then erase it from `plugin_widgets_`.
3. For each new widget, make a `PluginWidgetPane`. Its `closed` callback posts `on_simulation` to call `set_enabled(false)` on that DockWidget if it's still alive. Then add a `WindowEntry`:
   - `name` is the pane name.
   - `icon` is the pane's icon.
   - `pane` is the new pane.
   - `starts_closed` is `true`.
   - `home` builds the widget's initial dock:
     ```cpp
     switch (side) {
         case DockSide::Left:   return dock_beside(nullptr, DropSide::Left, width);
         case DockSide::Bottom: return dock_beside(nullptr, DropSide::Bottom, height);
         case DockSide::Float:  // docked, then floated by show_plugin_widget below
         case DockSide::Right:  return dock_beside(nullptr, DropSide::Right, width);
     }
     ```
   - Store it in `plugin_widgets_`.
4. For every widget, set the page title to its `Title` with `page->setTitle(title)`, falling back to the key when the title is empty. Then reconcile `enabled` with whether the page is docked:
   - **Enabled but not docked:** if `plugin_docks_[name]` still locks to a dock in `docks_`, call `dock->dock(page)`. Otherwise call `show_window(entry)`. For `Float`, then float the new tab with `floatTab`: find the tab with `TabShowing` and place it at the main window's centre. Erase the `plugin_docks_` entry once it's used.
   - **Docked but not enabled:** close its tab, without firing `closed`, since the plugin asked.
5. Rebuild `plugins_menu_`'s items when the set of names or titles changed. Each item is checkable, labelled with the title, and toggles `Enabled` through `on_simulation`. With no widgets, the submenu holds one disabled item, "No plugin windows".

`fill_window_menu`: after the built-in windows, add a separator and a submenu `jadefx::Menu("Plugins")`, and keep its pointer in `plugins_menu_`.

`page_named` already searches `windows_`, so a saved `layout.json` naming a plugin pane can dock it once the entry exists. For the restore before the plugin loads, update both `dock_page` lambdas, in `layout_host()` and in `apply_layout`. When `page_named(name)` is null and `name.rfind("plugin:", 0) == 0`, record `plugin_docks_[name] = <weak_ptr to dock>` and return false. Find the `shared_ptr` for `&dock` in `docks_`, or if the dock isn't adopted yet, keep a raw `IdeDock*` and resolve it against `docks_` when used. Using a recorded dock only when it's still in `docks_` keeps a dead pointer from being used.

- [ ] **Step 5: Run the tests.** Run `build\Debug\studio-tests.exe`. Expected: the new widget checks pass, with no new `FAIL` lines.

- [ ] **Step 6: Commit.** `git commit -am "Plugins: dock widgets as studio pages"`

### Task 11: Live check, Release build, package

**Files:** none new. This task is verification only.

- [ ] **Step 1: Build Release**

```powershell
cmake --build build --config Release --target AnarchyStudio AnarchyPlayer
```

If the link fails with LNK1104, the studio is running. Tell the user instead of killing it.

- [ ] **Step 2: Live check through the studio MCP.** Launch Release with its MCP port and token, as in the project's MCP notes. Then:
  1. Build a Folder `Live Plugin` in Workspace with:
     - a Script that makes a toolbar `Live` with a button `Toggle` (`icons/Play.png`), whose Click toggles a dock widget `Panel` (Title `Live Panel`, InitialDock `Right`);
     - a `VBox` holding a `Label` "Hello from a plugin" and a `Button` whose `Action` prints `button pressed`.

     The script parents the VBox into the widget.
  2. Right-click the Folder and choose **Save as Plugin**. Confirm `%APPDATA%\AnarchyEngine\plugins\Live Plugin.aeplugin` exists.
  3. Switch to the Plugins tab, click Toggle, and confirm the docked page shows the label and button in the IDE theme.
  4. Click the button and confirm Console shows `button pressed`.
  5. Edit the Script's source in the project. Save as Plugin again and choose **Replace**. Confirm the page comes back in the same dock and the new source runs.
  6. Delete the file from the plugins folder, wait 1 second, and confirm the toolbar and page are gone.

  Take at most two screenshots: the Plugins tab with the page open, and Console.

- [ ] **Step 3: Refresh the package.** Copy the Release `AnarchyStudio.exe` (renamed `AnarchyEngine-CPP.exe` in the package), `AnarchyPlayer.exe`, and `resources/` into `build/package/AnarchyEngine/`, then re-zip `build/AnarchyEngine-release.zip`.

- [ ] **Step 4: Run the full suites once.** Run `build\Debug\sandbox.exe` and `build\Debug\studio-tests.exe`, and report the results with any known flakes named.
