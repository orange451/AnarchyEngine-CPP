# Server and Client 2a: Engine Foundations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the engine what replication builds on, with no networking yet: a tree change sink, `Script.RunContext` and world roles, `RunService:IsServer/IsClient`, the `ServerStorage` scene service, the hidden `Connections` service with `Connection` instances and `LocalConnection`, and an Engine that runs without an audio device.

**Architecture:** All changes are in `engine_core`, `engine_instances`, and `engine_services`. Each piece is inert in Standalone (the default role) so single-player behaves exactly as today. Plan 2b (replication) consumes the sink, the role, and the Connection setters; plan 2c (executables) consumes the audio option.

**Tech Stack:** C++17, Luau bindings through the engine's Lua registry, Catch2 v3 sandbox tests, CMake, MSVC.

**Spec:** `docs/superpowers/specs/2026-10-13-server-client-design.md` (section 1 is this plan).

## Global Constraints

- Engine distances are "units"; never write "studs". Names are Anarchy's own: the spec's names (`RunContext`, `ServerStorage`, `Connections`, `Connection`, `LocalConnection`, `IsServer`, `IsClient`) are the user's choices.
- Comments in the engine's voice: a sentence above the thing saying what and why. Match the surrounding code's idiom.
- Standalone must cost nothing measurable: the sink is an empty `std::function` check; RunContext is one comparison per script start.
- `create<T>()` makes an instance unparented; callers parent it after. Stop's place restore and undo revival change the tree silently; that is accepted (see spec section 1).
- `src/network/Connection.hpp` exists (the transport's class) and its include directory reaches the sandbox, so the engine's Connection instance lives in `ConnectionInstance.hpp/.cpp` (class `engine_core::Connection`).
- Build and test (Git Bash, from the worktree `.claude/worktrees/network-transport`):
  - Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target sandbox > build/2a.log 2>&1; grep -E " error |error C|LNK" build/2a.log`
  - Run a tag: `./build/Release/sandbox.exe "[tag]"`. Never run sandbox.exe while building.
- Test names `SC<n> sentence`, tag `[server-client]` plus the area tag listed in each task.
- Commit messages `Area: what changed, as a sentence`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Explicit paths only; never add `.superpowers/`. Files are LF. Python is not on PATH.

## Review Focus

1. **Existing places and tests keep working:** an old project without `ServerStorage` loads; every test that counted services is updated to the new count, not deleted (SC24, plus the updated SS1/GS1/GS2/core tests).
2. **A Script's RunContext survives Stop and undo of a delete:** Script's custom `write_place` and the undo record otherwise drop it (SC10, SC11).
3. **The sink never fires for quiet physics writes, and fires exactly once per change:** set_parent must not report both Parented and a Parent property change (SC2, SC4).
4. **Connections never saves and never captures into the Play snapshot:** a project saved with Connections holding children writes nothing for them (SC31).
5. **A RunContext change during play starts or stops the script at once,** like toggling Enabled (SC16).

---

### Task 1: The tree change sink

**Files:**
- Create: `src/engine_core/TreeChange.hpp`, `sandbox/tree_change_tests.cpp`
- Modify: `src/engine_core/DataModelState.hpp` (end of `State`, ~line 282), `src/engine_core/DataModel.hpp` (public accessors near other state accessors), `src/engine_core/DataModel.cpp` (`destroy` ~752, `set_parent` ~1773, `set_name` ~2131, `emit_change` ~1606), `CMakeLists.txt` (sandbox list)

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  struct TreeChange {
      enum class Kind : std::uint8_t { Parented, Destroyed, Renamed, Property };
      Kind kind = Kind::Property;
      InstanceId id = 0;
      InstanceId previous_parent = 0;   // Parented: where it was (DataModel::kNoParent when nowhere)
      Field field = Field::Name;        // Property: the field (Events.hpp)
      std::uint64_t payload = 0;        // Property with Field::Reflected: lua_property_id(name)
  };
  using TreeChangeSink = std::function<void(const TreeChange&)>;
  // DataModel:
  void set_change_sink(TreeChangeSink sink);   // empty to clear
  bool has_change_sink() const;
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/tree_change_tests.cpp`:

```cpp
// The tree change sink replication reads
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Attachment.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Script.hpp"
#include "TreeChange.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using engine_core::DataModel;
using engine_core::Field;
using engine_core::InstanceId;
using engine_core::TreeChange;

namespace {

struct Recorder {
    std::vector<TreeChange> changes;
    void attach(DataModel& game) {
        game.set_change_sink([this](const TreeChange& change) { changes.push_back(change); });
    }
    std::size_t count(TreeChange::Kind kind, InstanceId id) const {
        std::size_t n = 0;
        for (const TreeChange& c : changes) n += (c.kind == kind && c.id == id) ? 1 : 0;
        return n;
    }
};

}  // namespace

TEST_CASE("SC1 parenting a new instance reports Parented from nowhere, once", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    Recorder recorder;
    recorder.attach(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(recorder.changes.empty());   // create alone reports nothing
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    REQUIRE(recorder.count(TreeChange::Kind::Parented, folder.id()) == 1);
    REQUIRE(recorder.changes.back().previous_parent == DataModel::kNoParent);
    // set_parent's own Parent field change is not reported a second time.
    for (const TreeChange& c : recorder.changes) {
        REQUIRE_FALSE((c.kind == TreeChange::Kind::Property && c.field == Field::Parent));
    }
    // Moving reports the old parent.
    game.set_parent(folder.id(), game.scene_service("Storage"));
    REQUIRE(recorder.changes.back().kind == TreeChange::Kind::Parented);
    REQUIRE(recorder.changes.back().previous_parent == game.scene_service("Workspace"));
    // Setting the same parent again reports nothing.
    const std::size_t before = recorder.changes.size();
    game.set_parent(folder.id(), game.scene_service("Storage"));
    REQUIRE(recorder.changes.size() == before);
}

TEST_CASE("SC2 rename, a built-in field, and a registry property each report once", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Script& script = add_script(game, "S", "-- nothing");
    engine_core::Attachment& attachment = game.create<engine_core::Attachment>();
    game.set_parent(attachment.id(), game.scene_service("Workspace"));
    Recorder recorder;
    recorder.attach(game);
    game.set_name(script.id(), "Renamed");
    REQUIRE(recorder.count(TreeChange::Kind::Renamed, script.id()) == 1);
    for (const TreeChange& c : recorder.changes) {
        REQUIRE_FALSE((c.kind == TreeChange::Kind::Property && c.field == Field::Name));
    }
    recorder.changes.clear();
    script.set_source("print(1)");
    REQUIRE(recorder.changes.size() == 1);
    REQUIRE(recorder.changes[0].kind == TreeChange::Kind::Property);
    REQUIRE(recorder.changes[0].field == Field::Source);
    recorder.changes.clear();
    REQUIRE_FALSE(attachment.set_offset_space(static_cast<int>(engine_core::TransformSpace::World)));
    bool saw_offset_space = false;
    for (const TreeChange& c : recorder.changes) {
        if (c.kind == TreeChange::Kind::Property && c.field == Field::Reflected &&
            c.payload == engine_core::lua_property_id("OffsetSpace")) {
            saw_offset_space = true;
        }
    }
    REQUIRE(saw_offset_space);
}

TEST_CASE("SC3 destroy reports Destroyed for the instance", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    const InstanceId id = folder.id();
    Recorder recorder;
    recorder.attach(game);
    game.destroy(id);
    REQUIRE(recorder.count(TreeChange::Kind::Destroyed, id) == 1);
}

TEST_CASE("SC4 a quiet simulated transform write reports nothing; a script transform write reports once", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    game.set_parent(object.id(), game.scene_service("Workspace"));
    Recorder recorder;
    recorder.attach(game);
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 5.0f;
    // Physics' quiet path (DataModel::write_simulated_transform, DataModel.cpp ~1221).
    game.write_simulated_transform(object.id(), moved);
    REQUIRE(recorder.changes.empty());
    moved.m[12] = 6.0f;
    game.apply_transform(object.id(), moved);   // the checked setter scripts reach
    REQUIRE(recorder.changes.size() == 1);
    REQUIRE(recorder.changes[0].field == Field::Transform);
}

TEST_CASE("SC5 with no sink set nothing is reported and clearing it stops reports", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE_FALSE(game.has_change_sink());
    Recorder recorder;
    recorder.attach(game);
    REQUIRE(game.has_change_sink());
    game.set_change_sink({});
    REQUIRE_FALSE(game.has_change_sink());
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    REQUIRE(recorder.changes.empty());
}
```

Before writing SC4, read `DataModel::write_simulated_transform` and `DataModel::apply_transform` signatures in `DataModel.hpp` and adjust the two calls to match them exactly (both take an id and a `Matrix4`; if either takes an origin or flag argument, pass the one physics and scripts pass). Add the file to the sandbox list in `CMakeLists.txt` after `sandbox/network_soak_tests.cpp`.

- [ ] **Step 2: Build to see the failure**

Run the build. Expected: `TreeChange.hpp` cannot be opened.

- [ ] **Step 3: Write `TreeChange.hpp`**

```cpp
#pragma once

#include "Events.hpp"
#include "types.hpp"

#include <cstdint>
#include <functional>

namespace engine_core {

// One change to the tree, as replication reads it. There is no "created":
// create<T>() makes an instance unparented, and its first set_parent reports
// Parented from nowhere. Quiet writes (physics, the pose blend) are never
// reported. The sink reads current values from the tree itself.
struct TreeChange {
    enum class Kind : std::uint8_t { Parented, Destroyed, Renamed, Property };
    Kind kind = Kind::Property;
    InstanceId id = 0;
    // Parented: where it was, DataModel::kNoParent when it was nowhere.
    InstanceId previous_parent = 0;
    // Property: the field, and for Field::Reflected the lua_property_id.
    Field field = Field::Name;
    std::uint64_t payload = 0;
};

using TreeChangeSink = std::function<void(const TreeChange&)>;

}  // namespace engine_core
```

- [ ] **Step 4: State, accessors, and the four funnels**

In `DataModelState.hpp`, include `TreeChange.hpp` and add at the end of `State`, after `PhysicsWorld* physics = nullptr;`:

```cpp
    // Replication's reader of the tree's changes. Empty in Standalone, so the
    // funnels pay one check.
    TreeChangeSink change_sink;
```

In `DataModel.hpp`, include `TreeChange.hpp` and add public methods beside the other state accessors:

```cpp
    // Every parent, destroy, rename, and checked property write calls it,
    // on the gameplay thread, after the change. Empty to stop.
    void set_change_sink(TreeChangeSink sink);
    bool has_change_sink() const;
```

In `DataModel.cpp`:

```cpp
void DataModel::set_change_sink(TreeChangeSink sink) { state_->change_sink = std::move(sink); }

bool DataModel::has_change_sink() const { return static_cast<bool>(state_->change_sink); }
```

`emit_change`, at its top (before `notify_watchers`):

```cpp
    // set_parent and set_name report their own kinds.
    if (state_->change_sink && field != Field::Parent && field != Field::Name) {
        TreeChange change;
        change.kind = TreeChange::Kind::Property;
        change.id = id;
        change.field = field;
        change.payload = payload;
        state_->change_sink(change);
    }
```

`set_parent`, immediately after `emit_change(id, Field::Parent, origin);`:

```cpp
    if (state_->change_sink) {
        TreeChange change;
        change.kind = TreeChange::Kind::Parented;
        change.id = id;
        change.previous_parent = old;
        state_->change_sink(change);
    }
```

`set_name`, immediately after `emit_change(id, Field::Name, current_origin());`:

```cpp
    if (state_->change_sink) {
        TreeChange change;
        change.kind = TreeChange::Kind::Renamed;
        change.id = id;
        state_->change_sink(change);
    }
```

`destroy`, after the service check (`contract_fail(destroy_error(id)...)`) and before anything is released, so the id still names a live instance when the sink runs:

```cpp
    if (state_->change_sink) {
        TreeChange change;
        change.kind = TreeChange::Kind::Destroyed;
        change.id = id;
        state_->change_sink(change);
    }
```

- [ ] **Step 5: Build and run**

Run `./build/Release/sandbox.exe "[tree-change]"`. Expected: SC1-SC5 pass. Then `"[project]"` and `"[history]"` once to confirm nothing changed for existing paths.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/TreeChange.hpp src/engine_core/DataModelState.hpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp sandbox/tree_change_tests.cpp CMakeLists.txt
git commit -m "Engine: a tree change sink reports parents, destroys, renames, and checked property writes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `Enum.RunContext` and `Script.RunContext`

**Files:**
- Modify: `src/engine_datatypes/Enum.hpp` (~line 46), `src/engine_datatypes/Enum.cpp` (entries ~197, EnumType ~250, `kTypes` ~303-314, accessor ~392), `src/engine_instances/Script.hpp`, `src/engine_instances/Script.cpp`, `src/engine_core/DataModelHistory.cpp` (`capture_record` ~273-285, `apply_record_fields` ~313-319)
- Create: `sandbox/run_context_tests.cpp`; add to the sandbox list

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  const EnumType& run_context_enum();          // "RunContext": Server 0, Client 1, Any 2
  enum class RunContext { Server = 0, Client = 1, Any = 2 };
  // Script:
  RunContext run_context() const;
  std::optional<std::string> set_run_context(int value);   // error text, nullopt on success
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/run_context_tests.cpp` (Task 3 adds more cases to this file):

```cpp
// Script.RunContext: where a script runs once worlds have roles
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Enum.hpp"
#include "Project.hpp"
#include "Script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using engine_core::RunContext;

TEST_CASE("SC8 RunContext is Server by default and scripts set it to Client or Any", "[server-client][run-context]") {
    ScriptRig rig;
    engine_core::Script& target = add_script(rig.game, "Target", "-- target");
    REQUIRE(target.run_context() == RunContext::Server);
    add_script(rig.game, "Setter",
               "local t = workspace:FindFirstChild('Target')\n"
               "_G.default_ok = t.RunContext == Enum.RunContext.Server\n"
               "t.RunContext = Enum.RunContext.Any\n"
               "_G.after = t.RunContext == Enum.RunContext.Any\n"
               "_G.bad = not pcall(function() t.RunContext = 3 end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"default_ok", "after", "bad"});
    REQUIRE(target.run_context() == RunContext::Any);
}

TEST_CASE("SC9 a project saves RunContext only when it is not Server and loads it back", "[server-client][run-context][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.game();
        engine_core::Script& client = add_script(game, "ClientOne", "-- c");
        REQUIRE_FALSE(client.set_run_context(static_cast<int>(RunContext::Client)));
        add_script(game, "ServerOne", "-- s");
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    engine_core::DataModel& game = loaded.game();
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    auto* client = dynamic_cast<engine_core::Script*>(game.instance(game.find_first_child(workspace, "ClientOne")));
    auto* server = dynamic_cast<engine_core::Script*>(game.instance(game.find_first_child(workspace, "ServerOne")));
    REQUIRE(client != nullptr);
    REQUIRE(server != nullptr);
    REQUIRE(client->run_context() == RunContext::Client);
    REQUIRE(server->run_context() == RunContext::Server);
}

TEST_CASE("SC10 Stop puts back the RunContext a script had when Play started", "[server-client][run-context]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "S", "-- s");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    rig.game.start_simulation();
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
    rig.game.stop_simulation();
    auto* restored = dynamic_cast<engine_core::Script*>(
        rig.game.instance(rig.game.find_first_child(rig.game.scene_service("Workspace"), "S")));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->run_context() == RunContext::Client);
}

TEST_CASE("SC11 undoing a script's delete keeps its RunContext", "[server-client][run-context][history]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Script& script = add_script(game, "S", "-- s");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
    const std::string guid = game.guid(script.id());
    begin_step(game, "Delete");
    game.destroy_tree(script.id());
    end_step(game);
    game.history().undo();
    auto* back = dynamic_cast<engine_core::Script*>(game.instance(game.find_guid(guid)));
    REQUIRE(back != nullptr);
    REQUIRE(back->run_context() == RunContext::Any);
}
```

Before running, check these helpers' exact names in `sandbox/support.hpp` and `Project.hpp` and adjust the calls to match: `Project::game()` (the project's DataModel accessor), `DataModel::find_first_child(parent, name)`, `DataModel::guid(id)`, `DataModel::find_guid(guid)`, `begin_step`/`end_step` (used by `sandbox/attachment_tests.cpp` ATT10), `require_globals`, `TempDir`. Keep each assertion's meaning unchanged.

- [ ] **Step 2: Build to see the failure**

Expected: `run_context` is not a member of `Script`.

- [ ] **Step 3: The enum**

`Enum.hpp`, after `TransformSpace`:

```cpp
// Server 0, Client 1, Any 2. Script.RunContext: where a script runs.
const EnumType& run_context_enum();
// run_context_enum's items, by value.
enum class RunContext { Server = 0, Client = 1, Any = 2 };
```

`Enum.cpp`: entries beside `kTransformSpaces`:

```cpp
// Where a Script runs: on the server, on each client, or on both. In
// single-player every script runs, whatever it says.
const EnumEntry kRunContexts[] = {
    {"Server", 0},
    {"Client", 1},
    {"Any", 2},
};
```

beside `kTransformSpaceType`:

```cpp
const EnumType kRunContextType{"RunContext", kRunContexts, count_of(kRunContexts)};
```

append `&kRunContextType` to the end of `kTypes` (after `&kLodKindType`), and beside `transform_space_enum()`:

```cpp
const EnumType& run_context_enum() { return kRunContextType; }
```

- [ ] **Step 4: The property on Script**

`Script.hpp`: include `Enum.hpp` and `<optional>`; add public:

```cpp
    // Where the script runs once worlds have roles: Server, Client, or Any.
    // Standalone runs every script. A change during play starts or stops it
    // as toggling Enabled does.
    RunContext run_context() const { return run_context_; }
    std::optional<std::string> set_run_context(int value);
```

and private `RunContext run_context_ = RunContext::Server;`.

`Script.cpp`: add, following `Attachment::set_offset_space`:

```cpp
namespace {

LuaSlot run_context_slot(RunContext context) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &run_context_enum();
    slot.number = static_cast<int>(context);
    return slot;
}

}  // namespace

std::optional<std::string> Script::set_run_context(int value) {
    if (!on_gameplay_thread()) {
        contract_fail("set_run_context runs on SimulationThread");
    }
    if (enum_item_name(run_context_enum(), value) == nullptr) {
        return std::string("RunContext must be an Enum.RunContext");
    }
    const RunContext next = static_cast<RunContext>(value);
    if (next == run_context_) {
        return std::nullopt;
    }
    const RunContext previous = run_context_;
    run_context_ = next;
    note_property_change("RunContext", run_context_slot(previous), run_context_slot(next));
    if (ScriptHost* host = script_host()) {
        host->on_script_enabled(*this, enabled_);
    }
    return std::nullopt;
}
```

(The anonymous namespace must sit before `set_run_context`; merge it with the file's existing anonymous namespace if that is cleaner, keeping registry functions where they are.) Reset in `on_release` and `on_reuse`: `run_context_ = RunContext::Server;`.

Registry read/write beside `read_lua_enabled`:

```cpp
bool read_run_context(DataModel&, DataModel& object, LuaSlot& out) {
    auto* script = dynamic_cast<Script*>(&object);
    if (script == nullptr) {
        return false;
    }
    out = run_context_slot(script->run_context());
    return true;
}

bool write_run_context(DataModel&, DataModel& object, LuaSlot& in) {
    auto* script = dynamic_cast<Script*>(&object);
    if (script == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &run_context_enum()) {
        in.error = "RunContext must be an Enum.RunContext";
        return false;
    }
    if (const std::optional<std::string> error = script->set_run_context(static_cast<int>(in.number))) {
        in.error = *error;
        return false;
    }
    return true;
}
```

and the registry entry (count 2):

```cpp
    const LuaField fields[] = {
        lua_property("Enabled", "boolean", true, read_lua_enabled, write_lua_enabled),
        lua_saved_enum("RunContext", run_context_enum(), read_run_context, write_run_context, "\"Server\""),
    };
    register_lua_class("Script", "LuaSource", fields, static_cast<int>(std::size(fields)));
```

- [ ] **Step 5: Stop restores it — compose the base bytes in `write_place`/`read_place`**

`Script::write_place` writes Enabled then the source and never calls `DataModel::write_place`, so registry-saved properties are lost at Stop. Following `Terrain.cpp` (~556-582), prefix the base bytes with their length:

```cpp
void Script::write_place(std::vector<std::byte>& out) const {
    // The base's saved registry properties, such as RunContext, length-prefixed.
    std::vector<std::byte> base;
    DataModel::write_place(base);
    const std::uint32_t length = static_cast<std::uint32_t>(base.size());
    const auto* l = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), l, l + sizeof(length));
    out.insert(out.end(), base.begin(), base.end());
    out.push_back(std::byte{enabled_ ? std::uint8_t{1} : std::uint8_t{0}});
    LuaSource::write_place(out);
}

void Script::read_place(const std::byte* data, std::size_t size) {
    start_generation_ = 0;
    std::uint32_t length = 0;
    if (data == nullptr || size < sizeof(length)) {
        DataModel::read_place(nullptr, 0);
        enabled_ = true;
        LuaSource::read_place(nullptr, 0);
        return;
    }
    std::memcpy(&length, data, sizeof(length));
    const std::size_t base_end = sizeof(length) + length;
    if (base_end > size) {
        DataModel::read_place(nullptr, 0);
        enabled_ = true;
        LuaSource::read_place(nullptr, 0);
        return;
    }
    DataModel::read_place(data + sizeof(length), length);
    if (base_end >= size) {
        enabled_ = true;
        LuaSource::read_place(nullptr, 0);
        return;
    }
    enabled_ = static_cast<unsigned char>(data[base_end]) != 0;
    LuaSource::read_place(data + base_end + 1, size - base_end - 1);
}
```

Add `#include <cstring>`. Check how `DataModel::read_place(nullptr, 0)` behaves (it should reset registry fields to defaults; confirm by reading it, and if it does not, call it only with real bytes and reset `run_context_` by hand in the empty branches). Update the header comment on `write_place` to "The base's registry bytes, length-prefixed; Enabled as one byte; then the source."

- [ ] **Step 6: Undo of a delete keeps it**

In `DataModelHistory.cpp` `capture_record`, a Script records only source and Enabled. Change the extra-bytes branch so a LuaSource (never spatial) also records its base registry bytes:

```cpp
    if (record.spatial || record.has_source) {
        // The base class's saved registry properties: a SpatialObject's Prefab,
        // a Script's RunContext.
        object->DataModel::write_place(record.extra);
    } else {
        object->write_place(record.extra);
    }
```

and in `apply_record_fields`, inside `if (record.has_source) { ... }`, after setting Enabled:

```cpp
        if (DataModel* object = instance(record.id)) {
            const std::byte* bytes = record.extra.empty() ? nullptr : record.extra.data();
            object->DataModel::read_place(bytes, record.extra.size());
        }
```

- [ ] **Step 7: Build and run**

Run `"[run-context]"`, then `"[history]"`, `"[project]"`, and `"[SS7]"` (scripts must still start). Expected: SC8-SC11 pass, nothing else regresses.

- [ ] **Step 8: Commit**

```bash
git add src/engine_datatypes/Enum.hpp src/engine_datatypes/Enum.cpp src/engine_instances/Script.hpp src/engine_instances/Script.cpp src/engine_core/DataModelHistory.cpp sandbox/run_context_tests.cpp CMakeLists.txt
git commit -m "Script: RunContext says whether a script runs on the server, the client, or both

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: World roles, script gating, and `RunService:IsServer/IsClient`

**Files:**
- Modify: `src/engine_core/types.hpp` (beside `ThreadRole` ~96), `src/engine_core/DataModelState.hpp`, `src/engine_core/DataModel.hpp/.cpp`, `src/engine_core/ScriptRuntime.hpp` (~412) / `.cpp` (`runs_here` ~424 and its callers ~416, ~1089, and `launch_one`), `src/engine_core/ScriptBindings.hpp` (~446) / `.cpp` (~1489, ~1720-1723), `src/engine_core/LuaApi.cpp` (hover docs ~1873), `sandbox/run_context_tests.cpp`

**Interfaces:**
- Consumes: `Script::run_context()`, `RunContext` (Task 2).
- Produces:
  ```cpp
  namespace engine_core {
  enum class WorldRole : std::uint8_t { Standalone, Server, Client };
  bool context_runs(RunContext context, WorldRole role);   // Standalone: always; Server: Server/Any; Client: Client/Any
  // DataModel:
  WorldRole world_role() const;
  void set_world_role(WorldRole role);    // gameplay thread; set before start_simulation
  // ScriptRuntime:
  bool runs_here(const Script& script) const;   // replaces runs_here(InstanceId)
  }
  ```

- [ ] **Step 1: Write the failing tests** (append to `sandbox/run_context_tests.cpp`)

```cpp
namespace {

void add_three(engine_core::DataModel& game) {
    engine_core::Script& server = add_script(game, "Server", "_G.server_ran = true");
    engine_core::Script& client = add_script(game, "Client", "_G.client_ran = true");
    engine_core::Script& any = add_script(game, "Any", "_G.any_ran = true");
    REQUIRE_FALSE(server.set_run_context(static_cast<int>(RunContext::Server)));
    REQUIRE_FALSE(client.set_run_context(static_cast<int>(RunContext::Client)));
    REQUIRE_FALSE(any.set_run_context(static_cast<int>(RunContext::Any)));
}

bool ran(ScriptRig& rig, const char* name) {
    bool value = false;
    return rig.runtime.global_boolean(name, value) && value;
}

}  // namespace

TEST_CASE("SC12 Standalone runs every script whatever its RunContext", "[server-client][run-context]") {
    ScriptRig rig;
    REQUIRE(rig.game.world_role() == engine_core::WorldRole::Standalone);
    add_three(rig.game);
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(ran(rig, "server_ran"));
    REQUIRE(ran(rig, "client_ran"));
    REQUIRE(ran(rig, "any_ran"));
}

TEST_CASE("SC13 a Server world runs Server and Any scripts only", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Server);
    add_three(rig.game);
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(ran(rig, "server_ran"));
    REQUIRE_FALSE(ran(rig, "client_ran"));
    REQUIRE(ran(rig, "any_ran"));
}

TEST_CASE("SC14 a Client world runs Client and Any scripts only", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Client);
    add_three(rig.game);
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE_FALSE(ran(rig, "server_ran"));
    REQUIRE(ran(rig, "client_ran"));
    REQUIRE(ran(rig, "any_ran"));
}

TEST_CASE("SC15 RunService.IsServer and IsClient follow the role; both are true in Standalone", "[server-client][run-context]") {
    const char* source =
        "local run = game:GetService('RunService')\n"
        "_G.is_server = run:IsServer()\n"
        "_G.is_client = run:IsClient()";
    struct Case { engine_core::WorldRole role; bool server; bool client; };
    for (const Case c : {Case{engine_core::WorldRole::Standalone, true, true},
                         Case{engine_core::WorldRole::Server, true, false},
                         Case{engine_core::WorldRole::Client, false, true}}) {
        ScriptRig rig;
        rig.game.set_world_role(c.role);
        engine_core::Script& script = add_script(rig.game, "Probe", source);
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Any)));
        rig.game.start_simulation();
        rig.frames(1);
        bool value = false;
        REQUIRE(rig.runtime.global_boolean("is_server", value));
        REQUIRE(value == c.server);
        REQUIRE(rig.runtime.global_boolean("is_client", value));
        REQUIRE(value == c.client);
    }
}

TEST_CASE("SC16 changing RunContext during play starts or stops the script at once", "[server-client][run-context]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Server);
    engine_core::Script& script = add_script(rig.game, "Ticker",
        "_G.ticks = 0\n"
        "game:GetService('RunService').Heartbeat:Connect(function() _G.ticks = _G.ticks + 1 end)");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    rig.game.start_simulation();
    rig.frames(2);
    double ticks = -1;
    REQUIRE_FALSE(rig.runtime.global_number("ticks", ticks));   // never started on a server
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Server)));
    rig.frames(3);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks >= 1);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(RunContext::Client)));
    const double stopped_at = ticks;
    rig.frames(3);
    REQUIRE(rig.runtime.global_number("ticks", ticks));
    REQUIRE(ticks == stopped_at);   // its Heartbeat connection died with it
}
```

If `ScriptRuntime` has no `global_number`, add one beside `global_boolean` (same shape, reading a number) as part of this task.

- [ ] **Step 2: Build to see the failure**

Expected: `WorldRole` undeclared.

- [ ] **Step 3: The role**

`types.hpp`, beside `ThreadRole`:

```cpp
// What a world is in a networked game. Standalone is single-player, Studio's
// Play included: every script runs. A Server runs Server and Any scripts; a
// Client runs Client and Any scripts.
enum class WorldRole : std::uint8_t { Standalone, Server, Client };
```

Declare `bool context_runs(RunContext context, WorldRole role);` in `Script.hpp` (it needs both enums) and define it in `Script.cpp`:

```cpp
bool context_runs(RunContext context, WorldRole role) {
    switch (role) {
        case WorldRole::Standalone: return true;
        case WorldRole::Server: return context != RunContext::Client;
        case WorldRole::Client: return context != RunContext::Server;
    }
    return true;
}
```

`DataModelState.hpp`: `WorldRole world_role = WorldRole::Standalone;` in `State`. `DataModel.hpp/.cpp`:

```cpp
    // Standalone until a server or client says otherwise, before Play starts.
    WorldRole world_role() const;
    void set_world_role(WorldRole role);
```

```cpp
WorldRole DataModel::world_role() const { return state_->world_role; }

void DataModel::set_world_role(WorldRole role) {
    if (!mutation_thread()) {
        contract_fail("set_world_role runs on SimulationThread");
    }
    state_->world_role = role;
}
```

- [ ] **Step 4: Gate in `ScriptRuntime`**

Change `runs_here(InstanceId)` to `runs_here(const Script& script)`; its body walks from `script.id()` exactly as now and, after the service-name test passes, returns `context_runs(script.run_context(), game_->world_role())`. Update the three callers (`on_moved` loop: `runs_here(*script)`; `enqueue_start`: `runs_here(script)`; `launch_one`: look up the Script it already holds and pass it). Update the header comment: "Whether a script is under Workspace, Scripts, or Gui, where scripts run, and its RunContext runs in this world's role."

- [ ] **Step 5: `IsServer` and `IsClient`**

`ScriptBindings.hpp`: `static int run_is_server(lua_State* state); static int run_is_client(lua_State* state);`. `ScriptBindings.cpp`, beside `run_is_running`:

```cpp
int ScriptBindings::run_is_server(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        ScriptRuntime* runtime = runtime_from(state);
        const bool server = runtime == nullptr || runtime->game_ == nullptr ||
                            runtime->game_->world_role() != WorldRole::Client;
        lua_pushboolean(state, server ? 1 : 0);
        return 1;
    });
}

int ScriptBindings::run_is_client(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        ScriptRuntime* runtime = runtime_from(state);
        const bool client = runtime == nullptr || runtime->game_ == nullptr ||
                            runtime->game_->world_role() != WorldRole::Server;
        lua_pushboolean(state, client ? 1 : 0);
        return 1;
    });
}
```

Registration:

```cpp
    const LuaField run[] = {
        lua_method("IsRunning", "boolean", reinterpret_cast<void*>(&ScriptBindings::run_is_running)),
        lua_method("IsServer", "boolean", reinterpret_cast<void*>(&ScriptBindings::run_is_server)),
        lua_method("IsClient", "boolean", reinterpret_cast<void*>(&ScriptBindings::run_is_client)),
    };
    register_lua_class("RunService", nullptr, run, static_cast<int>(std::size(run)));
```

Hover docs in `LuaApi.cpp` beside IsRunning:

```cpp
    add("RunService", "IsServer", "True on a server and in single-player.", "boolean", false, {});
    add("RunService", "IsClient", "True on a connected player and in single-player.", "boolean", false, {});
```

- [ ] **Step 6: Build and run**

Run `"[run-context]"`, `"[SS7]"`, and `"[analysis]"` (Luau declarations come from the registry; confirm no analysis test lists RunService members exhaustively — if one does, add the two methods there). Expected: SC12-SC16 pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/types.hpp src/engine_core/DataModelState.hpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_instances/Script.hpp src/engine_instances/Script.cpp src/engine_core/ScriptRuntime.hpp src/engine_core/ScriptRuntime.cpp src/engine_core/ScriptBindings.hpp src/engine_core/ScriptBindings.cpp src/engine_core/LuaApi.cpp sandbox/run_context_tests.cpp
git commit -m "Engine: a world's role decides which scripts run, and RunService says whether it is a server or a client

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The `ServerStorage` scene service

**Files:**
- Modify: `src/engine_core/Containment.hpp` (`kServices` ~22), `src/engine_services/SceneService.hpp` (class + `kSceneServiceClasses` ~33), `src/engine_services/SceneService.cpp` (class_name, Lua registration ~222), `src/engine_services/Game.cpp` (constructor, after `add_service<Gui>`), `src/engine_core/Project.cpp` (`static_assert` ~133 and the `existing_service<N>` list), `sandbox/scene_services_tests.cpp` (SS1), `sandbox/game_services_tests.cpp` (GS1, GS2), `sandbox/core_tests.cpp` (~50-60), plus any other test the build or `[project]` run shows counting services
- Create: `sandbox/server_storage_tests.cpp`; add to the sandbox list

**Interfaces:**
- Produces: scene service class `ServerStorage` (GUID `serverstorage`), after `Gui` in `kServices` and `kSceneServiceClasses`; `game:GetService("ServerStorage")`; scripts under it never run.

- [ ] **Step 1: Write the failing tests**

`sandbox/server_storage_tests.cpp`:

```cpp
// ServerStorage: what never leaves the server
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Folder.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST_CASE("SC20 game holds ServerStorage after Gui, and GetService finds it", "[server-client][server-storage]") {
    ScriptRig rig;
    const engine_core::InstanceId storage = rig.game.scene_service("ServerStorage");
    REQUIRE(storage != 0);
    REQUIRE(rig.game.guid(storage) == "serverstorage");
    REQUIRE(rig.game.next_sibling(rig.game.scene_service("Gui")) == storage);
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "_G.same = game:GetService('ServerStorage') == game.ServerStorage");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"same"});
}

TEST_CASE("SC21 a script in ServerStorage never runs, in any role", "[server-client][server-storage]") {
    for (const engine_core::WorldRole role :
         {engine_core::WorldRole::Standalone, engine_core::WorldRole::Server, engine_core::WorldRole::Client}) {
        ScriptRig rig;
        rig.game.set_world_role(role);
        engine_core::Script& script =
            add_script(rig.game, rig.game.scene_service("ServerStorage"), "Hidden", "_G.hidden_ran = true");
        REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Any)));
        rig.game.start_simulation();
        rig.frames(1);
        bool value = false;
        REQUIRE_FALSE(rig.runtime.global_boolean("hidden_ran", value));
    }
}

TEST_CASE("SC22 ServerStorage cannot be moved, renamed, or destroyed", "[server-client][server-storage]") {
    ScriptRig rig;
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "local s = game.ServerStorage\n"
               "_G.no_move = not pcall(function() s.Parent = workspace end)\n"
               "_G.no_rename = not pcall(function() s.Name = 'X' end)\n"
               "_G.no_destroy = not pcall(function() s:Destroy() end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"no_move", "no_rename", "no_destroy"});
}

TEST_CASE("SC23 ServerStorage and what it holds save and load", "[server-client][server-storage][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.game();
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), "Secrets");
        game.set_parent(folder.id(), game.scene_service("ServerStorage"));
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    engine_core::DataModel& game = loaded.game();
    REQUIRE(game.find_first_child(game.scene_service("ServerStorage"), "Secrets") != 0);
}

TEST_CASE("SC24 a project saved before ServerStorage existed loads and gains it", "[server-client][server-storage][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    { engine_core::Project made = engine_core::Project::create(root); }
    // Remove ServerStorage's file, as a project from before it would lack it.
    for (const auto& entry : fs::recursive_directory_iterator(root / "src")) {
        if (entry.is_regular_file() && entry.path().string().find("serverstorage") != std::string::npos) {
            fs::remove(entry.path());
            break;
        }
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    REQUIRE(loaded.game().scene_service("ServerStorage") != 0);
}
```

Before running, look at how `sandbox/scene_services_tests.cpp` SS10 removes a service's file from a saved project and copy its method into SC24 if it differs (the file name may be the service's GUID or class). Adjust helper names as in Task 2's note.

- [ ] **Step 2: Build to see the failure**

Expected: SC20 fails (`scene_service("ServerStorage")` is 0).

- [ ] **Step 3: The service**

`SceneService.hpp`, after the `Gui` class:

```cpp
// What never leaves the server: a networked game's secret logic, tools, and
// data. Nothing in it replicates to clients and no script in it runs. In
// single-player it is a place to keep things, like Storage.
class ServerStorage : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};
```

and `kSceneServiceClasses` becomes `{"Workspace", "Lighting", "Storage", "Scripts", "Gui", "ServerStorage"}`.

`SceneService.cpp`: `const char* ServerStorage::class_name() const { return "ServerStorage"; }` beside the others, and `register_lua_class("ServerStorage", "SceneService", nullptr, 0);` after Gui's (the `kSceneServiceClasses` loop registers it as a service).

`Containment.hpp` `kServices`: insert `{"ServerStorage", nullptr}` after `{"Gui", nullptr}`.

`Game.cpp`: `add_service<ServerStorage>(*this);` immediately after `add_service<Gui>(*this);`.

`Project.cpp`: the `static_assert` becomes 13, and the `existing_service<N>` list gains one entry and renumbers so index N matches `kServices[N]` (ServerStorage is index 5; the Assets entries shift to 6-12). Read the list and its comment before editing; keep its shape.

- [ ] **Step 4: Update the tests that count services**

Update, keeping each test's intent: SS1 (game's children grow by one, ServerStorage after Gui), GS1 (`kServices` size 13; indexes of Assets and its categories shift by one), GS2 (child class list gains ServerStorage; it is a scene service, not a game service), `core_tests.cpp` (Core still last). Run `"[SS1]" "[GS1]" "[GS2]"` and the core tests' tag, and fix any other failure the build or `"[project]"` run reveals the same way.

- [ ] **Step 5: Build and run**

Run `"[server-storage]"`, `"[project]"`, `"[SS7]"`, `"[SS10]"`. Expected: SC20-SC24 pass; existing tests pass after their updates.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/Containment.hpp src/engine_services/SceneService.hpp src/engine_services/SceneService.cpp src/engine_services/Game.cpp src/engine_core/Project.cpp sandbox/server_storage_tests.cpp sandbox/scene_services_tests.cpp sandbox/game_services_tests.cpp sandbox/core_tests.cpp CMakeLists.txt
git commit -m "Engine: a ServerStorage scene service keeps what a server never sends

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

(Add any other test file you updated.)

---

### Task 5: The `Connections` service, `Connection`, and `LocalConnection`

**Files:**
- Create: `src/engine_services/Connections.hpp`, `src/engine_services/Connections.cpp`, `src/engine_instances/ConnectionInstance.hpp`, `src/engine_instances/ConnectionInstance.cpp`, `sandbox/connections_tests.cpp`
- Modify: `src/engine_core/Containment.hpp` (a `kConnectionsClass` constant beside `kCoreClass`), `src/engine_core/Containment.cpp` (`placement_error` Game branch ~102, a Connections holder branch, a Connection child-side rule), `src/engine_core/DataModel.cpp` (`parent_error` ~2040 service branch), `src/engine_core/DataModel.hpp/.cpp` (a `connections()` accessor like `core()`), `src/engine_core/DataModelPlace.cpp` (`authored_tree` ~438 and `capture_place_unlocked` skips), `src/engine_services/Game.cpp` (make it after Core), `CMakeLists.txt` (engine_instances and engine_services source lists, sandbox list)

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  inline constexpr const char* kConnectionsClass = "Connections";
  // DataModel:
  InstanceId connections() const;              // the service's id; always present in a Game
  class Connections : public Service {         // hidden while it has no children
  public:
      InstanceId local_connection() const;
      void set_local_connection(InstanceId id);   // engine only (replication); nil with 0
  };
  class Connection : public Instance {         // ConnectionInstance.hpp
  public:
      const std::string& connection_id() const;
      void set_connection_id(std::string uuid);  // engine only; read-only to scripts
  };
  std::string make_connection_uuid();          // random, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", lowercase hex
  }
  ```
  Scripts: `game:GetService("Connections")`, `game.Connections`, `Connection.Id` (read-only string), `Connections.LocalConnection` (read-only `Connection?`). `Instance.new("Connection")` is refused.

- [ ] **Step 1: Write the failing tests**

`sandbox/connections_tests.cpp`:

```cpp
// The Connections service: who is connected, by UUID, and which one is me
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Connections.hpp"
#include "ConnectionInstance.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using engine_core::Connection;
using engine_core::Connections;

namespace {

bool is_uuid(const std::string& text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : !(std::isxdigit(static_cast<unsigned char>(text[i])) && !std::isupper(static_cast<unsigned char>(text[i])))) {
            return false;
        }
    }
    return true;
}

Connection& add_connection(engine_core::DataModel& game) {
    Connection& connection = game.create<Connection>();
    connection.set_connection_id(engine_core::make_connection_uuid());
    game.set_parent(connection.id(), game.connections());
    return connection;
}

}  // namespace

TEST_CASE("SC28 game holds Connections, hidden while empty and shown once it has a Connection", "[server-client][connections]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId id = game.connections();
    REQUIRE(id != 0);
    REQUIRE(game.parent(id) == 0);
    REQUIRE(game.instance(id)->hidden_in_explorer());
    add_connection(game);
    REQUIRE_FALSE(game.instance(id)->hidden_in_explorer());
}

TEST_CASE("SC29 UUIDs are well formed and never repeat", "[server-client][connections]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string uuid = engine_core::make_connection_uuid();
        REQUIRE(is_uuid(uuid));
        REQUIRE(seen.insert(uuid).second);
    }
}

TEST_CASE("SC30 scripts read Id and LocalConnection but cannot set them or make a Connection", "[server-client][connections]") {
    ScriptRig rig;
    Connection& mine = add_connection(rig.game);
    static_cast<Connections*>(rig.game.instance(rig.game.connections()))->set_local_connection(mine.id());
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "local c = game:GetService('Connections')\n"
               "_G.same_service = c == game.Connections\n"
               "_G.local_ok = c.LocalConnection ~= nil and #c.LocalConnection.Id == 36\n"
               "_G.no_set_id = not pcall(function() c.LocalConnection.Id = 'x' end)\n"
               "_G.no_set_local = not pcall(function() c.LocalConnection = nil end)\n"
               "_G.no_new = not pcall(function() Instance.new('Connection') end)\n"
               "_G.no_destroy = not pcall(function() c:Destroy() end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"same_service", "local_ok", "no_set_id", "no_set_local", "no_new", "no_destroy"});
}

TEST_CASE("SC31 Connections and its Connections never save", "[server-client][connections][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        add_connection(project.game());
        project.save();
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(text.find("\"Connection") == std::string::npos);
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    REQUIRE(loaded.game().first_child(loaded.game().connections()) == 0);
}

TEST_CASE("SC32 only a Connection goes in Connections, and a Connection goes only there", "[server-client][connections]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::placement_error("Connections", "Folder", "F").has_value());
    REQUIRE_FALSE(engine_core::placement_error("Connections", "Connection", "C").has_value());
    REQUIRE(engine_core::placement_error("Workspace", "Connection", "C").has_value());
    REQUIRE_FALSE(engine_core::placement_error("Game", "Connections", "Connections").has_value());
}

TEST_CASE("SC33 LocalConnection reads nil when its Connection is destroyed", "[server-client][connections]") {
    SimRole role;
    engine_core::Game game;
    Connection& mine = add_connection(game);
    auto* service = static_cast<Connections*>(game.instance(game.connections()));
    service->set_local_connection(mine.id());
    game.destroy(mine.id());
    REQUIRE(game.instance(service->local_connection()) == nullptr);
}
```

Add `#include <fstream>` and `<iterator>`. `SC31` must also confirm the Play snapshot ignores Connections: append to it, or as `SC34`, a `ScriptRig` that adds a Connection, calls `start_simulation()` then `stop_simulation()`, and checks the Connection is still there and unchanged (Stop must not retire it or restore over it).

- [ ] **Step 2: Build to see the failure**

Expected: `Connections.hpp` cannot be opened.

- [ ] **Step 3: The `Connection` instance**

`src/engine_instances/ConnectionInstance.hpp`:

```cpp
#pragma once

#include "Instance.hpp"

#include <string>

namespace engine_core {

// One client connected to a server. The server makes it under Connections
// when the client joins and destroys it when the client leaves; scripts read
// it and never make one. Id is a random UUID the server chose. Names,
// characters, and accounts are the game's to keep, keyed by Id.
class Connection : public Instance {
public:
    using Instance::Instance;
    const char* class_name() const override { return "Connection"; }

    const std::string& connection_id() const { return connection_id_; }
    // The engine's: replication sets it once, before parenting.
    void set_connection_id(std::string uuid);

protected:
    void on_release() override;
    void on_reuse() override;

private:
    std::string connection_id_;
};

// A random version-4 style UUID, lowercase hex with dashes.
std::string make_connection_uuid();

}  // namespace engine_core
```

Read `Instance.hpp`, `Folder.hpp`/`.cpp` (the simplest Instance subclass) and copy their constructor form if `using Instance::Instance;` is not how subclasses are declared; match how other classes register a factory (`register_lua_creatable`) and Lua fields.

`src/engine_instances/ConnectionInstance.cpp`:

```cpp
#include "ConnectionInstance.hpp"

#include "LuaApi.hpp"

#include <cstdio>
#include <random>

namespace engine_core {

void Connection::set_connection_id(std::string uuid) {
    connection_id_ = std::move(uuid);
    emit_property("Id");
}

void Connection::on_release() {
    connection_id_.clear();
    Instance::on_release();
}

void Connection::on_reuse() {
    connection_id_.clear();
    Instance::on_reuse();
}

std::string make_connection_uuid() {
    // One generator per thread, seeded from the device; ids never repeat in practice.
    thread_local std::mt19937_64 engine{std::random_device{}() ^ (static_cast<std::uint64_t>(std::random_device{}()) << 32)};
    const std::uint64_t high = engine();
    const std::uint64_t low = engine();
    char text[37];
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(high >> 32), static_cast<unsigned>((high >> 16) & 0xffff),
                  static_cast<unsigned>((high & 0x0fff) | 0x4000), static_cast<unsigned>(((low >> 48) & 0x3fff) | 0x8000),
                  static_cast<unsigned long long>(low & 0xffffffffffffull));
    return std::string(text, 36);
}

namespace {

bool read_id(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* connection = dynamic_cast<const Connection*>(&object);
    if (connection == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = connection->connection_id();
    return true;
}

DataModel& create_connection(DataModel& world) { return world.create<Connection>(); }

ANARCHY_LUA_REGISTER(register_connection_lua) {
    const LuaField fields[] = {
        lua_property("Id", "string", false, read_id, nullptr),
    };
    register_lua_class("Connection", "Instance", fields, static_cast<int>(std::size(fields)));
    // Made by replication (lua_create_instance), never by Instance.new.
    register_lua_creatable("Connection", create_connection, false);
}

}  // namespace

}  // namespace engine_core
```

Check `LuaSlot`'s string member name (`text` above) and `Instance::on_release`/`on_reuse` existence against `LuaApi.hpp` and `Instance.hpp`, and fix to match. If `on_release`/`on_reuse` are not virtuals on `Instance`, drop those overrides and clear the id where other classes reset state.

- [ ] **Step 4: The `Connections` service**

`src/engine_services/Connections.hpp`:

```cpp
#pragma once

#include "SceneService.hpp"

namespace engine_core {

// Who is connected to a networked game: one Connection per client, made and
// destroyed by the engine. A child of game outside the place, like Core: never
// saved, untouched by Play and Stop. The explorer shows it only while it holds
// a Connection. LocalConnection is a client's own Connection, nil on a server
// and in single-player.
class Connections : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
    bool hidden_in_explorer() const override;

    InstanceId local_connection() const;
    // The engine's: replication sets it when a client's world arrives.
    void set_local_connection(InstanceId id);

private:
    InstanceId local_connection_ = 0;
};

}  // namespace engine_core
```

`src/engine_services/Connections.cpp`:

```cpp
#include "Connections.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Connections::class_name() const { return kConnectionsClass; }

bool Connections::hidden_in_explorer() const { return first_child(id()) == 0; }

InstanceId Connections::local_connection() const {
    // A destroyed Connection reads nil: its id no longer names a live instance.
    return instance(local_connection_) != nullptr ? local_connection_ : 0;
}

void Connections::set_local_connection(InstanceId id) {
    local_connection_ = id;
    emit_property("LocalConnection");
}

namespace {

bool read_local_connection(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* service = dynamic_cast<const Connections*>(&object);
    if (service == nullptr) {
        return false;
    }
    out.id = service->local_connection();
    out.kind = out.id != 0 ? LuaSlot::Kind::Instance : LuaSlot::Kind::Nil;
    return true;
}

ANARCHY_LUA_REGISTER(register_connections_lua) {
    const LuaField fields[] = {
        lua_property("LocalConnection", "Connection?", false, read_local_connection, nullptr),
    };
    register_lua_class(kConnectionsClass, "Service", fields, static_cast<int>(std::size(fields)));
    register_lua_service(kConnectionsClass);
}

}  // namespace

}  // namespace engine_core
```

`first_child` and `instance` are DataModel methods reachable from a DataModel subclass; if a service instance must ask its world (the way other services do), use that form instead. Confirm `hidden_in_explorer()` is called per snapshot (the context says it is) so the "shown once it has a Connection" behavior needs no Explorer change.

- [ ] **Step 5: Containment and the place's boundaries**

`Containment.hpp`, beside `kCoreClass`:

```cpp
// Who is connected: a child of game outside the place, like Core, so never in
// kServices, never saved, and left as it is by Play and Stop.
inline constexpr const char* kConnectionsClass = "Connections";
```

`Containment.cpp` `placement_error`:
- the Game branch accepts `child_class == kConnectionsClass` beside `kCoreClass`;
- a holder branch: `if (holder_class == kConnectionsClass) { return child_class == "Connection" ? std::nullopt : std::optional<std::string>("Connections holds only Connection"); }`;
- a child-side rule, beside the asset-home rule: `if (child_class == "Connection" && holder_class != kConnectionsClass) return std::string("A Connection must be in Connections");`.

`DataModel.cpp` `parent_error`: in the service branch's `else if (... == kCoreClass)`, also accept `kConnectionsClass` with `home = 0`.

`DataModel.hpp/.cpp`: `InstanceId connections() const;` found the way `core()` is (read `core()` and copy it).

`DataModelPlace.cpp`: `authored_tree` skips `child == connections()` beside `child == core()`; `capture_place_unlocked` (and anything else that excludes Core by id — grep `core()` in DataModelPlace.cpp and DataModelHistory.cpp) excludes Connections the same way, so Play's snapshot neither captures nor restores it.

`Game.cpp`, after Core is made:

```cpp
    // Who is connected: outside the place, like Core.
    Connections& connections = create<Connections>();
    set_guid(connections.id(), service_guid(kConnectionsClass));
    set_parent(connections.id(), 0);
```

`CMakeLists.txt`: `src/engine_instances/ConnectionInstance.cpp` in the engine_instances list, `src/engine_services/Connections.cpp` in the engine_services list, `sandbox/connections_tests.cpp` in the sandbox list.

- [ ] **Step 6: Build and run**

Run `"[connections]"`, then `"[project]"`, `"[SS1]"`, `"[GS2]"`, the core tests' tag, and `"[history]"`. If a test that walks game's children (SS1, GS2, core tests) now sees Connections, update it to expect Connections last after Core, keeping its intent. Expected: SC28-SC34 pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine_services/Connections.hpp src/engine_services/Connections.cpp src/engine_instances/ConnectionInstance.hpp src/engine_instances/ConnectionInstance.cpp src/engine_core/Containment.hpp src/engine_core/Containment.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelPlace.cpp src/engine_services/Game.cpp sandbox/connections_tests.cpp CMakeLists.txt
git commit -m "Engine: a hidden Connections service holds a Connection per client, by UUID, and the client's LocalConnection

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

(Add any test file you updated.)

---

### Task 6: An Engine with no audio device

**Files:**
- Modify: `src/engine_core/Engine.hpp` (~40, ~146), `src/engine_core/Engine.cpp` (~56)
- Create: `sandbox/engine_options_tests.cpp`; add to the sandbox list

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  struct EngineOptions {
      bool audio_device = true;   // false: SoundEmitters mix offline, nothing is heard, no device opens
  };
  // Engine:
  Engine();                                  // unchanged: EngineOptions{}
  explicit Engine(const EngineOptions& options);
  bool has_audio_device() const;
  }
  ```

- [ ] **Step 1: Write the failing test**

`sandbox/engine_options_tests.cpp`:

```cpp
// Engine options a headless server needs
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Engine.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("SC38 an Engine made without an audio device still steps a playing place", "[server-client][engine-options]") {
    SimRole role;
    engine_core::EngineOptions options;
    options.audio_device = false;
    engine_core::Engine engine(options);
    REQUIRE_FALSE(engine.has_audio_device());
    engine_core::Engine standard;
    REQUIRE(standard.has_audio_device());
    engine_core::DataModel& game = engine.datamodel();
    add_script(game, "Probe", "_G.stepped = true");
    game.start_simulation();
    engine.resume();
    for (int i = 0; i < 3; ++i) engine.run_frame(1.0 / 60.0);
    REQUIRE(game.simulation_running());
}
```

(If `run_frame` is not public or takes another signature, use what `sandbox/particle_world_tests.cpp` PW10 uses.)

- [ ] **Step 2: Build to see the failure**

Expected: `EngineOptions` undeclared.

- [ ] **Step 3: Implement**

`Engine.hpp`, before `class Engine`:

```cpp
// How an Engine is built. A headless server turns the audio device off.
struct EngineOptions {
    // False: SoundEmitters still step and mix offline, but no device opens and
    // nothing is heard.
    bool audio_device = true;
};
```

In the class: `Engine();` stays, add `explicit Engine(const EngineOptions& options);` and `bool has_audio_device() const { return audio_device_; }`, and a member `bool audio_device_ = true;` declared before `audio_` (initialization order). `Engine.cpp`:

```cpp
Engine::Engine() : Engine(EngineOptions{}) {}

Engine::Engine(const EngineOptions& options) : audio_device_(options.audio_device), audio_(options.audio_device) {
    // ...the existing constructor body, unchanged...
}
```

Move the existing `Engine::Engine()` body into the new constructor. Check the member initializer order against declaration order (MSVC warns on mismatch; there must be no warning).

- [ ] **Step 4: Build and run**

Run `"[engine-options]"`, then `"[particle-world]"` and `"[audio]"`. Expected: SC38 passes; others unchanged.

- [ ] **Step 5: Run the full sandbox once**

`./build/Release/sandbox.exe > build/2a-full.log 2>&1; tail -5 build/2a-full.log`. Expected: all pass (known unrelated flakes: re-run that test once and report it).

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/Engine.hpp src/engine_core/Engine.cpp sandbox/engine_options_tests.cpp CMakeLists.txt
git commit -m "Engine: an option to run with no audio device, for a headless server

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
