# Server and Client 2b: Tree Replication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A server world's tree reaches every connected client and stays in step: a join snapshot, then one change message per tick, applied before client scripts run; each client gets a `Connection` with a UUID and its `LocalConnection`.

**Architecture:** A new library `engine_network` (`src/engine_network/`) links `engine_core` and `anarchy_network`. `TreeCodec` turns operations into bytes and back. `Replicator` (server) journals the tree change sink each tick, coalesces, filters, and encodes once. `ReplicaApplier` (client) applies a snapshot and changes through the ordinary setters. `ServerSession` and `ClientSession` own a `network::Endpoint`, create and destroy Connections, queue messages per peer, and implement `engine_core::INetworkPump`, which `Engine` calls at the start and end of each step.

**Property values reuse the save path.** An instance's state travels as the same JSON values a project save writes (`DataModel::save_properties` for a Create, a new `read_property_json` for one changed property) and is applied with `DataModel::load_property`, encoded in a compact binary JSON form. This carries every saved registry property, `Transform`, `Enabled`, `Simulated`, `VisualOnly`, enum names, curves, and references (by GUID, resolved late through `InstanceRef` exactly as a project load does) with exact values, and it rides on code that project save/load tests already prove. `Source` travels separately.

**Tech Stack:** C++17, zstd, the project's Catch2 v3 sandbox, CMake, MSVC.

**Spec:** `docs/superpowers/specs/2026-10-13-server-client-design.md` (section 2 is this plan; section 1 landed in plan 2a).

## Global Constraints

- Engine distances are "units"; never write "studs". Names are Anarchy's own.
- Comments in the engine's voice: a sentence above the thing saying what and why. Match the surrounding code's idiom.
- `engine_network` depends on `engine_core` (and through it the engine packages) and `anarchy_network`; nothing in `engine_core`/`engine_instances`/`engine_services` includes `engine_network`. `Engine` reaches a session only through `engine_core::INetworkPump`.
- Network identity: a NetId is the server's `InstanceId` raw value. The root `game` is NetId 0. Services and Connections exist in every `Game`: they are bound by GUID (`service_guid(class)` / `"connections"`), never created on a client.
- Replicated = alive, not under Core, not `ServerStorage` or under it. Everything else (Connections included) replicates. `Server`-context Scripts replicate with `Source` empty and their source changes are never sent.
- Values exact: JSON numbers travel as 64-bit doubles. No quantization in tree replication.
- Decoding never trusts input: every length is checked against the bytes left; a bad message fails as a whole and never crashes. The applier pre-checks with `parent_error`, `rename_error`, `destroy_error`, `room_left`, `valid_guid`, `lua_creatable_known` and counts a refused operation; it never relies on catching a contract failure.
- A client starts play while its world is still empty (`set_world_role(Client)`, `capture_place()`, `start_simulation()`), then builds the world from the snapshot during play. A client world never steps physics.
- Build and test (Git Bash, from the worktree `.claude/worktrees/server-client-2b`):
  - Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target sandbox > build/2b.log 2>&1; grep -E " error |error C|LNK" build/2b.log`
  - Run a tag: `./build/Release/sandbox.exe "[tag]"`. Never run sandbox.exe while building.
  - Known failures elsewhere, ignore them: `Q6` in `[project]` (fails on main), `S38` and `A34` (load-sensitive timing tests).
- Test names `RP<n> sentence`, tags `[replication]` plus the area tag each task lists.
- Commit messages `Area: what changed, as a sentence`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Explicit paths only; never add `.superpowers/`. Files are LF. Python is not on PATH.

## Review Focus

1. **A client that joins while the server is busy:** changes made in the tick a client connects, and every tick while its snapshot is still arriving, must reach it exactly once, after the snapshot (RP41).
2. **Moves across the filter boundary:** an instance moved into `ServerStorage` disappears for clients; moved back out, it reappears whole, with its current properties and children (RP33).
3. **Destroys:** `folder:Destroy()` orphans its children (they leave the tree, and nothing leaks on the client); `destroy_tree` removes a subtree; a create and destroy in the same tick sends nothing (RP31, RP32).
4. **References that arrive before their target** (an `ObjectValue` pointing at an instance created later, or at one in `ServerStorage`) read nil until the target exists, then resolve (RP24).
5. **Hostile or corrupt bytes** from the server fail the message and disconnect the client with "Bad data from the server", never a crash (RP14, RP26, RP47).

---

### Task 1: The network pump hook, no physics on a client, and two worlds on one thread

**Files:**
- Create: `src/engine_core/INetworkPump.hpp`, `sandbox/replication_engine_tests.cpp`
- Modify: `src/engine_core/Engine.hpp` (beside `set_renderer`/`set_clock` ~56, members beside `clock_` ~165), `src/engine_core/Engine.cpp` (`run_step` ~468-586, `step_physics` ~621), `CMakeLists.txt` (sandbox list, after `sandbox/tree_change_tests.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  // INetworkPump.hpp
  class INetworkPump {
  public:
      virtual ~INetworkPump() = default;
      virtual void receive(double now) = 0;   // start of each step, before anything else, under the frame lock
      virtual void send(double now) = 0;      // end of each step, after audio, under the frame lock
  };
  // Engine:
  void set_network(INetworkPump* pump);       // null to stop; set before start()
  }
  ```
  A Client-role world never steps physics (`Engine::step_physics` returns at once).

- [ ] **Step 1: Write the failing tests**

`sandbox/replication_engine_tests.cpp`:

```cpp
// What tree replication needs from the engine: a network pump at each step's
// ends, a client world that never steps physics, and two worlds on one thread
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "support.hpp"

#include "Engine.hpp"
#include "GameObject.hpp"
#include "INetworkPump.hpp"
#include "PhysicsObject.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using engine_core::InstanceId;

namespace {

// Writes 1 into First when the step starts, and records at the step's end
// whether a Heartbeat script had copied it into Second by then.
struct OrderPump : engine_core::INetworkPump {
    engine_core::DataModel* game = nullptr;
    InstanceId first = 0;
    InstanceId second = 0;
    std::vector<double> receives;
    std::vector<double> seen_at_send;   // Second's value at each send
    void receive(double now) override {
        receives.push_back(now);
        static_cast<engine_core::NumberValue*>(game->instance(first))->set_value(1.0);
    }
    void send(double) override {
        seen_at_send.push_back(static_cast<engine_core::NumberValue*>(game->instance(second))->value());
    }
};

}  // namespace

TEST_CASE("RP1 the engine pumps the network before a step's scripts and after them", "[replication][engine]") {
    SimRole role;
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::NumberValue& first = game.create<engine_core::NumberValue>();
    game.set_name(first.id(), "First");
    game.set_parent(first.id(), game.scene_service("Workspace"));
    engine_core::NumberValue& second = game.create<engine_core::NumberValue>();
    game.set_name(second.id(), "Second");
    game.set_parent(second.id(), game.scene_service("Workspace"));
    add_script(game, "Copy",
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "    workspace.Second.Value = workspace.First.Value * 2\n"
               "end)");
    OrderPump pump;
    pump.game = &game;
    pump.first = first.id();
    pump.second = second.id();
    engine.set_network(&pump);
    game.start_simulation();
    engine.resume();
    for (int i = 0; i < 4; ++i) {
        engine.run_frame(1.0 / 60.0);
    }
    engine.set_network(nullptr);
    REQUIRE(pump.receives.size() == 4);
    REQUIRE(pump.seen_at_send.size() == 4);
    for (std::size_t i = 1; i < pump.receives.size(); ++i) {
        REQUIRE(pump.receives[i] >= pump.receives[i - 1]);
    }
    // The script ran between receive and send: by the last send it had seen First == 1.
    REQUIRE(pump.seen_at_send.back() == 2.0);
}

TEST_CASE("RP2 a client world never steps physics; a server world does", "[replication][engine]") {
    for (const engine_core::WorldRole world : {engine_core::WorldRole::Client, engine_core::WorldRole::Server}) {
        SimRole role;
        engine_core::Engine engine;
        engine_core::DataModel& game = engine.datamodel();
        game.set_world_role(world);
        engine_core::GameObject& object = game.create<engine_core::GameObject>();
        engine_core::Matrix4 up = engine_core::matrix4_identity();
        up.m[13] = 10.0f;
        object.set_transform(up);
        game.set_parent(object.id(), game.scene_service("Workspace"));
        engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
        game.set_parent(body.id(), object.id());
        game.start_simulation();
        engine.resume();
        for (int i = 0; i < 30; ++i) {
            engine.run_frame(1.0 / 60.0);
        }
        const float y = object.transform().m[13];
        if (world == engine_core::WorldRole::Client) {
            REQUIRE(y == 10.0f);
        } else {
            REQUIRE(y < 10.0f);
        }
    }
}

TEST_CASE("RP3 two worlds on one thread run their own scripts", "[replication][engine]") {
    ScriptRig server;
    ScriptRig client;
    server.game.set_world_role(engine_core::WorldRole::Server);
    client.game.set_world_role(engine_core::WorldRole::Client);
    engine_core::Script& on_server = add_script(server.game, "Mark", "_G.where = 'server'");
    engine_core::Script& on_client = add_script(client.game, "Mark", "_G.where = 'client'");
    REQUIRE_FALSE(on_server.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    REQUIRE_FALSE(on_client.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    server.game.start_simulation();
    client.game.start_simulation();
    for (int i = 0; i < 3; ++i) {
        server.frames(1);
        client.frames(1);
    }
    std::string where;
    REQUIRE(server.runtime.global_string("where", where));
    REQUIRE(where == "server");
    REQUIRE(client.runtime.global_string("where", where));
    REQUIRE(where == "client");
}
```

Before building, check against the real headers and adjust calls without changing what each test proves: `NumberValue`'s setter and getter names (`ValueBase.hpp`), `Engine::run_frame`'s signature (sandbox/particle_world_tests.cpp PW10 calls it), `ScriptRuntime::global_string` (if it does not exist, add it beside `global_boolean`/`global_number`, same shape), and that a `PhysicsObject` parented under a `GameObject` moves it (sandbox/physics_game_object_tests.cpp). Add the file to the sandbox list.

- [ ] **Step 2: Build to see the failure**

Expected: `INetworkPump.hpp` cannot be opened.

- [ ] **Step 3: The interface**

`src/engine_core/INetworkPump.hpp`:

```cpp
#pragma once

namespace engine_core {

// A networked game's connection to the engine loop: a server or a client
// session. The engine calls receive at the start of each step, before
// anything else runs, and send at its end, after audio, both on the engine
// thread under the frame's write lock. now is seconds on a steady clock.
class INetworkPump {
public:
    virtual ~INetworkPump() = default;
    virtual void receive(double now) = 0;
    virtual void send(double now) = 0;
};

}  // namespace engine_core
```

- [ ] **Step 4: The engine**

`Engine.hpp`: include `INetworkPump.hpp`; beside `set_clock`:

```cpp
    // A server or client session, pumped at each step's ends. Null to stop.
    // Set before start(); the engine does not own it.
    void set_network(INetworkPump* pump);
```

and a member beside `clock_`: `INetworkPump* network_ = nullptr;`.

`Engine.cpp`: `void Engine::set_network(INetworkPump* pump) { network_ = pump; }` beside `set_clock`. A file-local helper:

```cpp
// Seconds on the steady clock, for the network's timers.
double network_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
```

At the very top of `run_step`, before the Commands block:

```cpp
    if (network_ != nullptr) {
        // What arrived since the last step lands before anything else runs.
        PROFILE_SCOPE("Network", profiler::Group::Engine);
        network_->receive(network_now());
    }
```

and at its very end, after `audio_.step` and the `last_*` bookkeeping:

```cpp
    if (network_ != nullptr) {
        PROFILE_SCOPE("Network", profiler::Group::Engine);
        network_->send(network_now());
    }
```

(`PROFILE_SCOPE` declares a scoped variable; the braces keep the two scopes apart. If the end of `run_step` already has a `PROFILE_SCOPE("Audio", ...)` at function scope, put the send block in its own braces after it.)

`step_physics`, first lines:

```cpp
    // A client shows the server's world: its bodies move only as replication
    // says, never by its own simulation.
    if (game_.world_role() == WorldRole::Client) {
        return;
    }
```

- [ ] **Step 5: Build and run**

Run `"[replication]"`, then `"[physics]"` and `"[particle-world]"` once. Expected: RP1-RP3 pass, nothing else changes.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/INetworkPump.hpp src/engine_core/Engine.hpp src/engine_core/Engine.cpp sandbox/replication_engine_tests.cpp CMakeLists.txt
git commit -m "Engine: a network pump at each step's ends, and a client world never steps physics

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

(Add `ScriptRuntime.hpp/.cpp` if you added `global_string`.)

---

### Task 2: One property's current value, as a save writes it

**Files:**
- Modify: `src/engine_core/DataModel.hpp` (beside `save_properties` ~391), `src/engine_core/DataModel.cpp` (beside `save_properties` ~2552; `saved_value` is file-local there), `src/engine_instances/SpatialObject.hpp/.cpp` (beside its `save_properties`/`load_property`), `src/engine_instances/Script.hpp/.cpp` (same)
- Create: `sandbox/replication_property_tests.cpp`; add to the sandbox list

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  // DataModel:
  // The current value of one property as a project save writes it, whether or
  // not it is at its default: a saved registry property, Simulated, or
  // VisualOnly; a subclass adds what its save_properties adds (a
  // SpatialObject's Transform, a Script's Enabled). False for any other name.
  virtual bool read_property_json(std::string_view name, JsonValue& out) const;
  }
  ```
  `load_property(name, value)` with the value it returns puts the same state on another instance.

- [ ] **Step 1: Write the failing tests**

`sandbox/replication_property_tests.cpp`:

```cpp
// read_property_json: one property's value as a save writes it, which
// replication sends and load_property applies
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "support.hpp"

#include "GameObject.hpp"
#include "Light.hpp"
#include "Script.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

using engine_core::JsonValue;

namespace {

// Every key save_properties writes, read back one at a time, is the same value.
void require_matches_save(const engine_core::DataModel& object) {
    engine_core::PropertyBag saved;
    object.save_properties(saved);
    for (const auto& [key, value] : saved) {
        INFO(object.class_name() << "." << key);
        JsonValue read;
        REQUIRE(object.read_property_json(key, read));
        REQUIRE(read == value);
    }
}

}  // namespace

TEST_CASE("RP5 read_property_json gives what a save writes, at a default or not", "[replication][property]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId workspace = game.scene_service("Workspace");

    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 3.5f;
    object.set_transform(moved);
    game.set_simulated(object.id(), true);
    game.set_parent(object.id(), workspace);
    require_matches_save(object);

    engine_core::PointLight& light = game.create<engine_core::PointLight>();
    game.set_parent(light.id(), workspace);
    require_matches_save(light);
    JsonValue brightness;
    REQUIRE(light.read_property_json("Brightness", brightness));   // at its default, still read
    REQUIRE(brightness.is_number());

    engine_core::Script& script = add_script(game, "S", "-- s");
    script.set_enabled(false);
    require_matches_save(script);
    JsonValue enabled;
    REQUIRE(script.read_property_json("Enabled", enabled));
    REQUIRE(enabled == JsonValue::boolean(false));

    engine_core::ObjectValue& pointer = game.create<engine_core::ObjectValue>();
    game.set_parent(pointer.id(), workspace);
    pointer.set_value_object(object.id());
    require_matches_save(pointer);

    JsonValue none;
    REQUIRE_FALSE(object.read_property_json("Name", none));
    REQUIRE_FALSE(object.read_property_json("NoSuchProperty", none));
    REQUIRE_FALSE(script.read_property_json("Source", none));   // Source is not a saved property
}

TEST_CASE("RP6 load_property with read_property_json's value copies the state", "[replication][property]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    engine_core::PointLight& from = game.create<engine_core::PointLight>();
    game.set_parent(from.id(), workspace);
    engine_core::PointLight& to = game.create<engine_core::PointLight>();
    game.set_parent(to.id(), workspace);
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[14] = -2.0f;
    from.set_transform(moved);
    engine_core::PropertyBag saved;
    from.save_properties(saved);
    for (const auto& [key, value] : saved) {
        JsonValue read;
        REQUIRE(from.read_property_json(key, read));
        std::string error;
        REQUIRE(to.load_property(key, read, error));
        REQUIRE(error.empty());
    }
    engine_core::PropertyBag copied;
    to.save_properties(copied);
    REQUIRE(copied == saved);
}
```

Check the real API before building and adjust calls without changing meaning: the `ObjectValue` C++ setter (`ValueBase.hpp`; if there is none, write it through the registry: `lua_class_find("ObjectValue","Value")->write(game, pointer, slot)` with an Instance slot), `PointLight`'s header name, and `PropertyBag` equality (a `std::vector<std::pair<std::string, JsonValue>>`; if `JsonValue` has no `operator==`, compare with the existing helper the project tests use).

- [ ] **Step 2: Build to see the failure**

Expected: `read_property_json` is not a member.

- [ ] **Step 3: Implement**

`DataModel.hpp`, beside `save_properties`, with the comment from Interfaces:

```cpp
    virtual bool read_property_json(std::string_view name, JsonValue& out) const;
```

`DataModel.cpp`, beside `save_properties`:

```cpp
bool DataModel::read_property_json(std::string_view name, JsonValue& out) const {
    const std::shared_ptr<const std::vector<LuaField>> fields = lua_saved_field_list(class_name());
    for (const LuaField& field : *fields) {
        if (name == field.name) {
            return saved_value(*state_->root, *this, field, out);
        }
    }
    if (name == "Simulated") {
        out = JsonValue::boolean(simulated(id_));
        return true;
    }
    if (name == "VisualOnly") {
        out = JsonValue::boolean(visual_only(id_));
        return true;
    }
    return false;
}
```

Read `save_properties` and the `Simulated`/`VisualOnly` branch of `load_property` (~2600) first, and match how `default_properties` names and writes those two keys; use the same accessor names (`simulated(id)`, `visual_only(id)` or whatever they are).

`SpatialObject`: override, declared beside its `save_properties`:

```cpp
bool SpatialObject::read_property_json(std::string_view name, JsonValue& out) const {
    if (DataModel::read_property_json(name, out)) {
        return true;
    }
    if (name == "Transform") {
        const Matrix4 value = transform();
        out = json_floats(value.m, 16);
        return true;
    }
    return false;
}
```

(The base runs first so a class with its own saved `Transform` row, such as a PhysicsBase, answers from that row.)

`Script`: override beside its `save_properties`:

```cpp
bool Script::read_property_json(std::string_view name, JsonValue& out) const {
    if (name == "Enabled") {
        out = JsonValue::boolean(enabled_);
        return true;
    }
    return LuaSource::read_property_json(name, out);
}
```

If `LuaSource` does not override it, `LuaSource::read_property_json` resolves to `DataModel`'s; that is correct.

- [ ] **Step 4: Build and run**

Run `"[replication]"`, `"[project]"` (Q6 known), `"[history]"`. Expected: RP5-RP6 pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_instances/SpatialObject.hpp src/engine_instances/SpatialObject.cpp src/engine_instances/Script.hpp src/engine_instances/Script.cpp sandbox/replication_property_tests.cpp CMakeLists.txt
git commit -m "Engine: read one property's current value as a save writes it

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The `engine_network` library and the tree codec

**Files:**
- Create: `src/engine_network/ByteBuffer.hpp`, `src/engine_network/ByteBuffer.cpp`, `src/engine_network/JsonWire.hpp`, `src/engine_network/JsonWire.cpp`, `src/engine_network/TreeCodec.hpp`, `src/engine_network/TreeCodec.cpp`, `src/engine_network/README.md`, `sandbox/replication_codec_tests.cpp`
- Modify: `CMakeLists.txt` (a new library after `anarchy_network` ~596-614; the `anarchy_warnings` foreach ~1240; `ANARCHY_OWN_LIBRARIES` ~1337; the sandbox list and its link line ~1221)

**Interfaces:**
- Produces (namespace `engine_network`):
  ```cpp
  class ByteWriter {                              // grows as needed
  public:
      void u8(std::uint8_t v);
      void u32(std::uint32_t v);                  // little-endian, 4 bytes
      void varuint(std::uint64_t v);              // LEB128
      void f64(double v);                         // IEEE 754, little-endian, 8 bytes
      void string(std::string_view text);         // varuint length, then bytes
      void bytes(const std::uint8_t* data, std::size_t count);
      const std::vector<std::uint8_t>& data() const;
      std::vector<std::uint8_t>& data();
      void clear();
  };
  class ByteReader {                              // never reads past the end; a failed read sets failed() and returns 0/""
  public:
      ByteReader(const std::uint8_t* data, std::size_t size);
      std::uint8_t u8(); std::uint32_t u32(); std::uint64_t varuint(); double f64();
      std::string string();                       // refuses a length longer than what is left
      bool failed() const; std::size_t left() const; bool done() const;   // done: nothing left and not failed
  };
  void write_json(ByteWriter& out, const engine_core::JsonValue& value);
  bool read_json(ByteReader& in, engine_core::JsonValue& out);   // false on bad data; depth at most 32

  constexpr std::uint8_t kCodecVersion = 1;
  enum class MessageType : std::uint8_t { SnapshotPart = 1, Changes = 2 };
  struct Op {
      enum class Kind : std::uint8_t { Define = 1, Bind, Create, Destroy, Parent, Rename, Set, Source };
      Kind kind = Kind::Define;
      std::uint32_t netid = 0;
      std::uint32_t parent = 0;            // Create, Parent
      std::string text;                    // Define: the string; Bind/Create: guid; Rename: name; Source: source
      std::string class_name;              // Create
      std::string name;                    // Create
      engine_core::PropertyBag properties; // Create: name -> value; Set: exactly one
      bool has_source = false;             // Create
      std::string source;                  // Create
  };
  // Class and property names: the server keeps one table for every client, and
  // each message defines the strings it is first to use.
  class StringTable {
  public:
      // The string's index; *added is true when this call added it.
      std::uint32_t intern(std::string_view text, bool* added = nullptr);
      const std::string* at(std::uint32_t index) const;
      std::size_t size() const;
      void clear();
  };
  // Builds an op stream. Strings it interns for the first time are defined in
  // the stream before their first use.
  class OpWriter {
  public:
      OpWriter(ByteWriter& out, StringTable& strings);
      void define_all();                    // every string in the table, in index order (a snapshot's start)
      void bind(std::uint32_t netid, std::string_view guid);
      void create(std::uint32_t netid, std::uint32_t parent, std::string_view class_name, std::string_view guid,
                  std::string_view name, const engine_core::PropertyBag& properties, const std::string* source);
      void destroy(std::uint32_t netid);
      void parent(std::uint32_t netid, std::uint32_t parent);
      void rename(std::uint32_t netid, std::string_view name);
      void set(std::uint32_t netid, std::string_view property, const engine_core::JsonValue& value);
      void source(std::uint32_t netid, std::string_view source);
      std::size_t ops() const;
  };
  // Reads ops until the bytes run out. Define ops update the client's table
  // and are not returned. False, with nothing guaranteed in out, on bad data.
  bool read_ops(ByteReader& in, StringTable& strings, std::vector<Op>& out);
  }
  ```

The wire, after a message's header (Task 5 and 6 write the headers): a sequence of ops, each `u8 kind` then:
- `Define`: `varuint index` (must equal the table's size), `string text`
- `Bind`: `u32 netid`, `string guid`
- `Create`: `u32 netid`, `u32 parent`, `varuint class`, `string guid`, `string name`, `varuint count`, `count × (varuint property, json value)`, `u8 has_source`, `[string source]`
- `Destroy`: `u32 netid`; `Parent`: `u32 netid`, `u32 parent`; `Rename`: `u32 netid`, `string name`
- `Set`: `u32 netid`, `varuint property`, `json value`; `Source`: `u32 netid`, `string source`

A JSON value is `u8 tag` then: 0 null; 1 false; 2 true; 3 `f64`; 4 `string`; 5 array `varuint count` then items; 6 object `varuint count` then `count × (string key, value)`.

- [ ] **Step 1: The CMake target**

After `anarchy_network`'s block:

```cmake
# engine_network replicates a server's instance tree to its clients over
# anarchy_network (docs/superpowers/specs/2026-10-13-server-client-design.md).
# Nothing in the engine packages includes it: Engine reaches a session only
# through engine_core::INetworkPump.
add_library(engine_network STATIC
    src/engine_network/ByteBuffer.cpp
    src/engine_network/JsonWire.cpp
    src/engine_network/TreeCodec.cpp
)
target_include_directories(engine_network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/engine_network")
target_compile_features(engine_network PUBLIC cxx_std_17)
target_link_libraries(engine_network PUBLIC engine_core anarchy_network)
target_link_libraries(engine_network PRIVATE zstd)
```

If `engine_core` is declared after this point in the file, put the block after the engine packages' declarations (after `target_link_libraries(engine_core PRIVATE zstd)`) instead. Add `engine_network` to the `anarchy_warnings` foreach and to `ANARCHY_OWN_LIBRARIES` beside `anarchy_network`, and to the sandbox link line: `target_link_libraries(sandbox PRIVATE studio_core anarchy_network engine_network Catch2::Catch2WithMain)`.

- [ ] **Step 2: Write the failing tests**

`sandbox/replication_codec_tests.cpp`:

```cpp
// The tree codec: bytes, JSON values, and operations, and that no input
// crashes it (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "ByteBuffer.hpp"
#include "JsonWire.hpp"
#include "TreeCodec.hpp"

#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

using engine_core::JsonValue;
using engine_network::ByteReader;
using engine_network::ByteWriter;
using engine_network::Op;

namespace {

JsonValue round_trip(const JsonValue& value) {
    ByteWriter out;
    engine_network::write_json(out, value);
    ByteReader in(out.data().data(), out.data().size());
    JsonValue back;
    REQUIRE(engine_network::read_json(in, back));
    REQUIRE(in.done());
    return back;
}

}  // namespace

TEST_CASE("RP10 bytes, varints, doubles and strings round-trip", "[replication][codec]") {
    ByteWriter out;
    out.u8(7);
    out.u32(0xDEADBEEFu);
    for (const std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 0xFFFFFFFFull, ~0ull}) out.varuint(v);
    out.f64(0.1);
    out.f64(-std::numeric_limits<double>::infinity());
    out.string("");
    out.string("Läuft ✓");
    ByteReader in(out.data().data(), out.data().size());
    REQUIRE(in.u8() == 7);
    REQUIRE(in.u32() == 0xDEADBEEFu);
    for (const std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 0xFFFFFFFFull, ~0ull}) REQUIRE(in.varuint() == v);
    REQUIRE(in.f64() == 0.1);
    REQUIRE(in.f64() == -std::numeric_limits<double>::infinity());
    REQUIRE(in.string().empty());
    REQUIRE(in.string() == "Läuft ✓");
    REQUIRE(in.done());
    REQUIRE(in.u8() == 0);   // past the end
    REQUIRE(in.failed());
}

TEST_CASE("RP11 a string longer than what is left fails without allocating it", "[replication][codec]") {
    ByteWriter out;
    out.varuint(1ull << 40);
    out.u8('x');
    ByteReader in(out.data().data(), out.data().size());
    REQUIRE(in.string().empty());
    REQUIRE(in.failed());
}

TEST_CASE("RP12 JSON values round-trip exactly", "[replication][codec]") {
    JsonValue object = JsonValue::object();
    object.set("a", JsonValue::number(1.0 / 3.0));
    object.set("b", JsonValue::array({JsonValue::boolean(true), JsonValue(), JsonValue::string("x")}));
    const float f = 0.1f;
    const JsonValue values[] = {JsonValue(), JsonValue::boolean(false), JsonValue::boolean(true), JsonValue::number(0.0),
                                JsonValue::number(-1e300), JsonValue::number_from_float(f), JsonValue::string("guid-1"),
                                JsonValue::array(), object};
    for (const JsonValue& value : values) {
        REQUIRE(round_trip(value) == value);
    }
    REQUIRE(static_cast<float>(round_trip(JsonValue::number_from_float(f)).as_number()) == f);
}

TEST_CASE("RP13 operations round-trip and strings are defined once, before first use", "[replication][codec]") {
    engine_network::StringTable server_strings;
    ByteWriter out;
    engine_network::OpWriter writer(out, server_strings);
    engine_core::PropertyBag props;
    engine_core::bag_set(props, "Brightness", JsonValue::number(2.5));
    engine_core::bag_set(props, "Enabled", JsonValue::boolean(false));
    const std::string source = "print('hi')";
    writer.bind(1, "workspace");
    writer.create(10, 1, "PointLight", "abc123", "Lamp", props, nullptr);
    writer.create(11, 1, "Script", "def456", "Main", {}, &source);
    writer.set(10, "Brightness", JsonValue::number(3.0));
    writer.rename(10, "Lamp2");
    writer.parent(11, 10);
    writer.source(11, "print(2)");
    writer.destroy(10);
    REQUIRE(writer.ops() == 8);
    REQUIRE(server_strings.size() == 4);   // PointLight, Brightness, Enabled, Script

    engine_network::StringTable client_strings;
    ByteReader in(out.data().data(), out.data().size());
    std::vector<Op> ops;
    REQUIRE(engine_network::read_ops(in, client_strings, ops));
    REQUIRE(client_strings.size() == 4);
    REQUIRE(ops.size() == 8);
    REQUIRE(ops[0].kind == Op::Kind::Bind);
    REQUIRE(ops[0].netid == 1);
    REQUIRE(ops[0].text == "workspace");
    REQUIRE(ops[1].kind == Op::Kind::Create);
    REQUIRE(ops[1].class_name == "PointLight");
    REQUIRE(ops[1].text == "abc123");
    REQUIRE(ops[1].name == "Lamp");
    REQUIRE(ops[1].parent == 1);
    REQUIRE(ops[1].properties == props);
    REQUIRE_FALSE(ops[1].has_source);
    REQUIRE(ops[2].has_source);
    REQUIRE(ops[2].source == source);
    REQUIRE(ops[3].kind == Op::Kind::Set);
    REQUIRE(ops[3].properties.size() == 1);
    REQUIRE(ops[3].properties[0].first == "Brightness");
    REQUIRE(ops[4].kind == Op::Kind::Rename);
    REQUIRE(ops[4].text == "Lamp2");
    REQUIRE(ops[5].kind == Op::Kind::Parent);
    REQUIRE(ops[5].parent == 10);
    REQUIRE(ops[6].kind == Op::Kind::Source);
    REQUIRE(ops[6].text == "print(2)");
    REQUIRE(ops[7].kind == Op::Kind::Destroy);

    // A second message defines only what is new; define_all starts a snapshot with everything.
    ByteWriter again;
    engine_network::OpWriter second(again, server_strings);
    second.set(10, "Brightness", JsonValue::number(4.0));
    std::vector<Op> more;
    ByteReader in2(again.data().data(), again.data().size());
    REQUIRE(engine_network::read_ops(in2, client_strings, more));
    REQUIRE(more.size() == 1);
    ByteWriter snapshot;
    engine_network::OpWriter fresh(snapshot, server_strings);
    fresh.define_all();
    engine_network::StringTable joiner;
    std::vector<Op> none;
    ByteReader in3(snapshot.data().data(), snapshot.data().size());
    REQUIRE(engine_network::read_ops(in3, joiner, none));
    REQUIRE(none.empty());
    REQUIRE(joiner.size() == server_strings.size());
}

TEST_CASE("RP14 truncated, garbled and hostile input fails without crashing", "[replication][codec]") {
    engine_network::StringTable strings;
    ByteWriter out;
    engine_network::OpWriter writer(out, strings);
    engine_core::PropertyBag props;
    engine_core::bag_set(props, "Brightness", JsonValue::number(2.5));
    writer.create(10, 0, "PointLight", "abc123", "Lamp", props, nullptr);
    writer.set(10, "Brightness", JsonValue::array({JsonValue::number(1), JsonValue::string("s")}));
    const std::vector<std::uint8_t> good = out.data();
    // Every truncation either fails or reads fewer ops; none crashes.
    for (std::size_t length = 0; length < good.size(); ++length) {
        engine_network::StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(good.data(), length);
        if (engine_network::read_ops(in, fresh, ops)) {
            REQUIRE(ops.size() < 2);
        }
    }
    // Every single-byte change: no crash (the result may be ok or not).
    for (std::size_t at = 0; at < good.size(); ++at) {
        std::vector<std::uint8_t> bad = good;
        bad[at] ^= 0xFF;
        engine_network::StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(bad.data(), bad.size());
        (void)engine_network::read_ops(in, fresh, ops);
    }
    // An unknown op kind, a Define out of order, a reference to an undefined string.
    const std::vector<std::vector<std::uint8_t>> hostile = {
        {0x7F},
        {1, 5, 1, 'x'},
        {7, 1, 0, 0, 0, 9, 0},
    };
    for (const auto& bytes : hostile) {
        engine_network::StringTable fresh;
        std::vector<Op> ops;
        ByteReader in(bytes.data(), bytes.size());
        REQUIRE_FALSE(engine_network::read_ops(in, fresh, ops));
    }
    // JSON nested deeper than 32 fails.
    ByteWriter deep;
    for (int i = 0; i < 40; ++i) {
        deep.u8(5);
        deep.varuint(1);
    }
    deep.u8(0);
    ByteReader in(deep.data().data(), deep.data().size());
    JsonValue value;
    REQUIRE_FALSE(engine_network::read_json(in, value));
}
```

Add the file to the sandbox list. `PropertyBag` equality is `std::vector<std::pair<std::string, JsonValue>>` equality; if `JsonValue` lacks `operator==`, add one in `PropertyBag.hpp/.cpp` (deep, by kind: numbers compare with `==` on the doubles) as part of this task, since RP12/RP13 need it.

- [ ] **Step 3: Build to see the failure**

Expected: `ByteBuffer.hpp` cannot be opened (CMake fails first on the missing sources; create empty `.cpp` files so it configures and the build fails on the headers instead).

- [ ] **Step 4: `ByteBuffer`**

`ByteBuffer.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine_network {

// Appends values to a growing buffer, little-endian.
class ByteWriter {
public:
    void u8(std::uint8_t value) { data_.push_back(value); }
    void u32(std::uint32_t value);
    // Seven bits a byte, low first, the high bit set on every byte but the last.
    void varuint(std::uint64_t value);
    void f64(double value);
    void string(std::string_view text);
    void bytes(const std::uint8_t* data, std::size_t count);
    const std::vector<std::uint8_t>& data() const { return data_; }
    std::vector<std::uint8_t>& data() { return data_; }
    void clear() { data_.clear(); }

private:
    std::vector<std::uint8_t> data_;
};

// Reads what ByteWriter wrote. It never reads past the end: a read that would
// sets failed() and returns 0 or "", and every later read does the same.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t varuint();
    double f64();
    std::string string();
    bool failed() const { return failed_; }
    std::size_t left() const { return failed_ ? 0 : size_ - at_; }
    bool done() const { return !failed_ && at_ == size_; }

private:
    bool take(std::size_t count);
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

}  // namespace engine_network
```

`ByteBuffer.cpp`:

```cpp
#include "ByteBuffer.hpp"

#include <cstring>

namespace engine_network {

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        data_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void ByteWriter::varuint(std::uint64_t value) {
    while (value >= 0x80) {
        data_.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    data_.push_back(static_cast<std::uint8_t>(value));
}

void ByteWriter::f64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 0; shift < 64; shift += 8) {
        data_.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

void ByteWriter::string(std::string_view text) {
    varuint(text.size());
    bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void ByteWriter::bytes(const std::uint8_t* data, std::size_t count) {
    data_.insert(data_.end(), data, data + count);
}

bool ByteReader::take(std::size_t count) {
    if (failed_ || count > size_ - at_) {
        failed_ = true;
        return false;
    }
    return true;
}

std::uint8_t ByteReader::u8() {
    if (!take(1)) {
        return 0;
    }
    return data_[at_++];
}

std::uint32_t ByteReader::u32() {
    if (!take(4)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data_[at_++]) << (8 * i);
    }
    return value;
}

std::uint64_t ByteReader::varuint() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const std::uint8_t byte = u8();
        if (failed_) {
            return 0;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    failed_ = true;   // more than ten bytes: not a varint this writer makes
    return 0;
}

double ByteReader::f64() {
    if (!take(8)) {
        return 0.0;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits |= static_cast<std::uint64_t>(data_[at_++]) << (8 * i);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string ByteReader::string() {
    const std::uint64_t length = varuint();
    if (failed_ || length > size_ - at_) {
        failed_ = true;
        return std::string();
    }
    std::string text(reinterpret_cast<const char*>(data_ + at_), static_cast<std::size_t>(length));
    at_ += static_cast<std::size_t>(length);
    return text;
}

}  // namespace engine_network
```

(The `~0ull` varint in RP10 needs ten bytes; the loop above allows shifts 0..63, which is ten bytes. Confirm the test passes and keep it.)

- [ ] **Step 5: `JsonWire`**

`JsonWire.hpp`:

```cpp
#pragma once

#include "ByteBuffer.hpp"

#include "PropertyBag.hpp"

namespace engine_network {

// A JSON value as a tag byte and its contents: what a property's value is on
// the wire, exactly as a project save would write it.
void write_json(ByteWriter& out, const engine_core::JsonValue& value);
// False on bad data, or nesting deeper than 32.
bool read_json(ByteReader& in, engine_core::JsonValue& out);

}  // namespace engine_network
```

`JsonWire.cpp`:

```cpp
#include "JsonWire.hpp"

namespace engine_network {

namespace {

enum Tag : std::uint8_t { Null = 0, False = 1, True = 2, Number = 3, String = 4, Array = 5, Object = 6 };
constexpr int kMaxDepth = 32;

bool read_value(ByteReader& in, engine_core::JsonValue& out, int depth) {
    using engine_core::JsonValue;
    if (depth > kMaxDepth) {
        return false;
    }
    switch (in.u8()) {
        case Null: out = JsonValue(); break;
        case False: out = JsonValue::boolean(false); break;
        case True: out = JsonValue::boolean(true); break;
        case Number: out = JsonValue::number(in.f64()); break;
        case String: out = JsonValue::string(in.string()); break;
        case Array: {
            const std::uint64_t count = in.varuint();
            // Each item takes at least one byte, so a count beyond what is left is a lie.
            if (in.failed() || count > in.left()) {
                return false;
            }
            std::vector<JsonValue> items(static_cast<std::size_t>(count));
            for (JsonValue& item : items) {
                if (!read_value(in, item, depth + 1)) {
                    return false;
                }
            }
            out = JsonValue::array(std::move(items));
            break;
        }
        case Object: {
            const std::uint64_t count = in.varuint();
            if (in.failed() || count > in.left()) {
                return false;
            }
            out = JsonValue::object();
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string key = in.string();
                JsonValue value;
                if (in.failed() || !read_value(in, value, depth + 1)) {
                    return false;
                }
                out.set(std::move(key), std::move(value));
            }
            break;
        }
        default:
            return false;
    }
    return !in.failed();
}

}  // namespace

void write_json(ByteWriter& out, const engine_core::JsonValue& value) {
    using Kind = engine_core::JsonValue::Kind;
    switch (value.kind()) {
        case Kind::Null: out.u8(Null); break;
        case Kind::Bool: out.u8(value.as_bool() ? True : False); break;
        case Kind::Number:
            out.u8(Number);
            out.f64(value.as_number());
            break;
        case Kind::String:
            out.u8(String);
            out.string(value.as_string());
            break;
        case Kind::Array:
            out.u8(Array);
            out.varuint(value.items().size());
            for (const engine_core::JsonValue& item : value.items()) {
                write_json(out, item);
            }
            break;
        case Kind::Object:
            out.u8(Object);
            out.varuint(value.members().size());
            for (const auto& [key, member] : value.members()) {
                out.string(key);
                write_json(out, member);
            }
            break;
    }
}

bool read_json(ByteReader& in, engine_core::JsonValue& out) { return read_value(in, out, 0); }

}  // namespace engine_network
```

- [ ] **Step 6: `TreeCodec`**

`TreeCodec.hpp`: the declarations from Interfaces, with comments in the engine's voice (the wire layout above goes in a comment at the top of the file).

`TreeCodec.cpp`:

```cpp
#include "TreeCodec.hpp"

#include "JsonWire.hpp"

#include <unordered_map>

namespace engine_network {

// StringTable keeps the strings in index order and a map back.
std::uint32_t StringTable::intern(std::string_view text, bool* added) {
    const auto found = index_.find(std::string(text));
    if (found != index_.end()) {
        if (added != nullptr) *added = false;
        return found->second;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(strings_.size());
    strings_.emplace_back(text);
    index_.emplace(strings_.back(), index);
    if (added != nullptr) *added = true;
    return index;
}

const std::string* StringTable::at(std::uint32_t index) const {
    return index < strings_.size() ? &strings_[index] : nullptr;
}

std::size_t StringTable::size() const { return strings_.size(); }

void StringTable::clear() {
    strings_.clear();
    index_.clear();
}

OpWriter::OpWriter(ByteWriter& out, StringTable& strings) : out_(out), strings_(strings) {}

void OpWriter::define(std::uint32_t index) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Define));
    out_.varuint(index);
    out_.string(*strings_.at(index));
}

std::uint32_t OpWriter::ref(std::string_view text) {
    bool added = false;
    const std::uint32_t index = strings_.intern(text, &added);
    if (added) {
        define(index);
    }
    return index;
}

void OpWriter::define_all() {
    for (std::uint32_t index = 0; index < strings_.size(); ++index) {
        define(index);
    }
}

void OpWriter::bind(std::uint32_t netid, std::string_view guid) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Bind));
    out_.u32(netid);
    out_.string(guid);
    ++ops_;
}

void OpWriter::create(std::uint32_t netid, std::uint32_t parent, std::string_view class_name, std::string_view guid,
                      std::string_view name, const engine_core::PropertyBag& properties, const std::string* source) {
    // Strings first: their Define ops must come before the op that uses them.
    const std::uint32_t klass = ref(class_name);
    std::vector<std::uint32_t> keys;
    keys.reserve(properties.size());
    for (const auto& member : properties) {
        keys.push_back(ref(member.first));
    }
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Create));
    out_.u32(netid);
    out_.u32(parent);
    out_.varuint(klass);
    out_.string(guid);
    out_.string(name);
    out_.varuint(properties.size());
    for (std::size_t i = 0; i < properties.size(); ++i) {
        out_.varuint(keys[i]);
        write_json(out_, properties[i].second);
    }
    out_.u8(source != nullptr ? 1 : 0);
    if (source != nullptr) {
        out_.string(*source);
    }
    ++ops_;
}

void OpWriter::destroy(std::uint32_t netid) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Destroy));
    out_.u32(netid);
    ++ops_;
}

void OpWriter::parent(std::uint32_t netid, std::uint32_t parent) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Parent));
    out_.u32(netid);
    out_.u32(parent);
    ++ops_;
}

void OpWriter::rename(std::uint32_t netid, std::string_view name) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Rename));
    out_.u32(netid);
    out_.string(name);
    ++ops_;
}

void OpWriter::set(std::uint32_t netid, std::string_view property, const engine_core::JsonValue& value) {
    const std::uint32_t key = ref(property);
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Set));
    out_.u32(netid);
    out_.varuint(key);
    write_json(out_, value);
    ++ops_;
}

void OpWriter::source(std::uint32_t netid, std::string_view source) {
    out_.u8(static_cast<std::uint8_t>(Op::Kind::Source));
    out_.u32(netid);
    out_.string(source);
    ++ops_;
}

namespace {

bool lookup(const StringTable& strings, std::uint64_t index, std::string& out) {
    const std::string* text = index <= 0xFFFFFFFFull ? strings.at(static_cast<std::uint32_t>(index)) : nullptr;
    if (text == nullptr) {
        return false;
    }
    out = *text;
    return true;
}

}  // namespace

bool read_ops(ByteReader& in, StringTable& strings, std::vector<Op>& out) {
    while (in.left() > 0) {
        Op op;
        op.kind = static_cast<Op::Kind>(in.u8());
        switch (op.kind) {
            case Op::Kind::Define: {
                const std::uint64_t index = in.varuint();
                std::string text = in.string();
                if (in.failed() || index != strings.size()) {
                    return false;
                }
                strings.intern(text);
                continue;   // a Define is for the table, not the caller
            }
            case Op::Kind::Bind:
                op.netid = in.u32();
                op.text = in.string();
                break;
            case Op::Kind::Create: {
                op.netid = in.u32();
                op.parent = in.u32();
                if (!lookup(strings, in.varuint(), op.class_name)) {
                    return false;
                }
                op.text = in.string();
                op.name = in.string();
                const std::uint64_t count = in.varuint();
                if (in.failed() || count > in.left()) {
                    return false;
                }
                for (std::uint64_t i = 0; i < count; ++i) {
                    std::string key;
                    engine_core::JsonValue value;
                    if (!lookup(strings, in.varuint(), key) || !read_json(in, value)) {
                        return false;
                    }
                    op.properties.emplace_back(std::move(key), std::move(value));
                }
                const std::uint8_t has_source = in.u8();
                if (has_source > 1) {
                    return false;
                }
                op.has_source = has_source == 1;
                if (op.has_source) {
                    op.source = in.string();
                }
                break;
            }
            case Op::Kind::Destroy:
                op.netid = in.u32();
                break;
            case Op::Kind::Parent:
                op.netid = in.u32();
                op.parent = in.u32();
                break;
            case Op::Kind::Rename:
                op.netid = in.u32();
                op.text = in.string();
                break;
            case Op::Kind::Set: {
                op.netid = in.u32();
                std::string key;
                engine_core::JsonValue value;
                if (!lookup(strings, in.varuint(), key) || !read_json(in, value)) {
                    return false;
                }
                op.properties.emplace_back(std::move(key), std::move(value));
                break;
            }
            case Op::Kind::Source:
                op.netid = in.u32();
                op.text = in.string();
                break;
            default:
                return false;
        }
        if (in.failed()) {
            return false;
        }
        out.push_back(std::move(op));
    }
    return !in.failed();
}

}  // namespace engine_network
```

`OpWriter` holds `ByteWriter& out_; StringTable& strings_; std::size_t ops_ = 0;` and private `define(std::uint32_t)`, `ref(std::string_view)`; `StringTable` holds `std::vector<std::string> strings_; std::unordered_map<std::string, std::uint32_t> index_;`. Note `create` properties are written in the bag's order (a `PropertyBag` is kept sorted by `bag_set`), which RP13's equality relies on.

`src/engine_network/README.md`: a short description in the style of `src/network/README.md`: what the library is, its files bottom up (ByteBuffer, JsonWire, TreeCodec, then Replicator, ReplicaApplier, Sessions as later tasks add them), the spec's path, and that it reaches the engine only through `INetworkPump`.

- [ ] **Step 7: Build and run**

Run `"[replication]"`. Expected: RP10-RP14 pass, with RP1-RP6 still passing.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/engine_network sandbox/replication_codec_tests.cpp
git commit -m "Network: engine_network's tree codec, with property values as a save writes them

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

(Add `PropertyBag.hpp/.cpp` if you added `operator==`.)

---

### Task 4: The join snapshot: Replicator builds it, ReplicaApplier applies it

**Files:**
- Create: `src/engine_network/Replicator.hpp`, `src/engine_network/Replicator.cpp`, `src/engine_network/ReplicaApplier.hpp`, `src/engine_network/ReplicaApplier.cpp`, `sandbox/replication_support.hpp`, `sandbox/replication_snapshot_tests.cpp`
- Modify: `CMakeLists.txt` (the two `.cpp` in `engine_network`; the test file in the sandbox list), `src/engine_network/README.md`

**Interfaces:**
- Consumes: `OpWriter`, `read_ops`, `StringTable`, `ByteWriter`/`ByteReader`, `MessageType`, `kCodecVersion` (Task 3); `DataModel::read_property_json` (Task 2); `DataModel::save_properties`/`load_property`, `ConnectionInstance::set_connection_id`, `Connections::set_local_connection`, `lua_create_instance`, `lua_creatable_known`, `valid_guid`, `set_guid`, `find_guid`, `parent_error`, `rename_error`, `destroy_error`, `room_left`.
- Produces (namespace `engine_network`):
  ```cpp
  struct ReplicatorOptions {
      std::size_t snapshot_part_bytes = 1u << 20;   // a snapshot message carries at most this much compressed data
  };
  class Replicator {
  public:
      explicit Replicator(engine_core::DataModel& game, ReplicatorOptions options = {});   // sets the game's change sink
      ~Replicator();                                                                        // clears it
      // Clients see it: alive, the root, or under the root and neither Core nor ServerStorage nor under them.
      bool replicated(engine_core::InstanceId id) const;
      // A join snapshot for the client whose Connection is local_connection, as messages ready to send in order.
      void snapshot_messages(engine_core::InstanceId local_connection, std::vector<std::vector<std::uint8_t>>& out);
      // Task 5: bool end_tick(std::vector<std::uint8_t>& message);
      std::uint64_t tick() const;
  };
  enum class ApplyResult { Ok, SnapshotApplied, Bad };
  class ReplicaApplier {
  public:
      explicit ReplicaApplier(engine_core::DataModel& game);
      // One message from the server: a snapshot part (applied when the last one
      // arrives) or a tick's changes (only after the snapshot). Bad: the bytes
      // were malformed or out of order; the caller ends the connection.
      ApplyResult receive_message(const std::uint8_t* data, std::size_t size);
      engine_core::InstanceId local(std::uint32_t netid) const;   // DataModel::kNoParent when not known
      bool joined() const;               // the snapshot has been applied
      std::size_t skipped() const;       // operations refused or naming an instance this client no longer has
      std::uint64_t server_tick() const;
  };
  }
  ```

Message layouts (the codec's op stream follows each header):
- A snapshot is one op stream, its body: `u8 kCodecVersion`, `varuint server tick`, `u32 local connection netid`, then ops (`define_all` first). The body is zstd-compressed and split into parts; each part message: `u8 MessageType::SnapshotPart`, `u32 index`, `u32 count`, `varuint raw body size`, then that part's compressed bytes.
- A tick's changes (Task 5): `u8 MessageType::Changes`, `u8 kCodecVersion`, `varuint server tick`, then ops.

**One string table for every client.** The server keeps one `StringTable`. A Changes message is encoded once for all clients, so it begins by defining every string added since the last Changes message (Task 5 tracks how far the table was broadcast), and a snapshot begins with `define_all`. A string can therefore reach a client twice; `read_ops` accepts a `Define` whose index is already in the table when its text is the same (Step 4 below changes Task 3's rule).

- [ ] **Step 1: Write the shared test support**

`sandbox/replication_support.hpp`:

```cpp
#pragma once

// Helpers the replication tests share: a server world and a client world on
// one thread, and a check that a client's tree is the server's, filtered.

#include "support.hpp"

#include "ConnectionInstance.hpp"
#include "LuaSource.hpp"
#include "ReplicaApplier.hpp"
#include "Replicator.hpp"
#include "Script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace replication {

// A client world as a ClientSession leaves it before the snapshot: Client role, playing, empty.
inline void start_client(ScriptRig& client) {
    client.game.set_world_role(engine_core::WorldRole::Client);
    client.game.capture_place();
    client.game.start_simulation();
}

inline void start_server(ScriptRig& server) {
    server.game.set_world_role(engine_core::WorldRole::Server);
    server.game.start_simulation();
}

// Hands every message to the applier in order; the last result.
inline engine_network::ApplyResult deliver(engine_network::ReplicaApplier& applier,
                                           const std::vector<std::vector<std::uint8_t>>& messages) {
    engine_network::ApplyResult result = engine_network::ApplyResult::Ok;
    for (const auto& message : messages) {
        result = applier.receive_message(message.data(), message.size());
        if (result == engine_network::ApplyResult::Bad) {
            break;
        }
    }
    return result;
}

// The Source a client should hold: a Server script's is empty.
inline std::string expected_source(const engine_core::DataModel& object) {
    const auto* lua = dynamic_cast<const engine_core::LuaSource*>(&object);
    if (lua == nullptr) {
        return std::string();
    }
    const auto* script = dynamic_cast<const engine_core::Script*>(&object);
    if (script != nullptr && script->run_context() == engine_core::RunContext::Server) {
        return std::string();
    }
    return lua->source();
}

// The client's tree under client_id matches the server's under server_id: the
// same replicated children in order, each with the same class, GUID, name,
// saved properties, Source, and Connection Id.
inline void require_same_tree(const engine_network::Replicator& replicator, const engine_core::DataModel& server,
                              engine_core::InstanceId server_id, const engine_core::DataModel& client,
                              engine_core::InstanceId client_id) {
    std::vector<engine_core::InstanceId> want;
    for (engine_core::InstanceId c = server.first_child(server_id); c != 0; c = server.next_sibling(c)) {
        if (replicator.replicated(c)) {
            want.push_back(c);
        }
    }
    std::vector<engine_core::InstanceId> have;
    for (engine_core::InstanceId c = client.first_child(client_id); c != 0; c = client.next_sibling(c)) {
        if (c != client.core()) {
            have.push_back(c);
        }
    }
    INFO("under " << server.name(server_id));
    REQUIRE(have.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const engine_core::DataModel& s = *server.instance(want[i]);
        const engine_core::DataModel& c = *client.instance(have[i]);
        INFO(server.name(want[i]));
        REQUIRE(std::string(c.class_name()) == s.class_name());
        REQUIRE(client.guid(have[i]) == server.guid(want[i]));
        REQUIRE(client.name(have[i]) == server.name(want[i]));
        engine_core::PropertyBag sp;
        engine_core::PropertyBag cp;
        s.save_properties(sp);
        c.save_properties(cp);
        REQUIRE(cp == sp);
        if (const auto* lua = dynamic_cast<const engine_core::LuaSource*>(&c)) {
            REQUIRE(lua->source() == expected_source(s));
        }
        if (const auto* sc = dynamic_cast<const engine_core::ConnectionInstance*>(&s)) {
            REQUIRE(static_cast<const engine_core::ConnectionInstance&>(c).connection_id() == sc->connection_id());
        }
        require_same_tree(replicator, server, want[i], client, have[i]);
    }
}

inline void require_same_world(const engine_network::Replicator& replicator, const engine_core::DataModel& server,
                               const engine_core::DataModel& client) {
    require_same_tree(replicator, server, 0, client, 0);
}

}  // namespace replication
```

`require_same_tree` skips the client's Core: the client's own Core is not replicated and the server's Core is filtered. Services appear on both sides and compare like any instance (their saved properties included); the server's `ServerStorage` is filtered, so the client's `ServerStorage` (which every `Game` has) would show up in `have` but not `want`: also skip the client's `ServerStorage` in `have` (by `client.service("ServerStorage")`). Adjust accessor names (`first_child`, `next_sibling`, `core`, `service`, `guid`, `name`) to the real `DataModel` API.

- [ ] **Step 2: Write the failing tests**

`sandbox/replication_snapshot_tests.cpp`:

```cpp
// The join snapshot: the server's place, filtered, built on an empty client
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "replication_support.hpp"

#include "Connections.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

using engine_core::InstanceId;
using replication::require_same_world;

namespace {

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

engine_core::ConnectionInstance& add_connection(engine_core::DataModel& game) {
    engine_core::ConnectionInstance& c = game.create<engine_core::ConnectionInstance>();
    c.set_connection_id(engine_core::make_connection_uuid());
    game.set_parent(c.id(), game.connections());
    return c;
}

// A place with something of most kinds: nested folders, a moved part, a light,
// scripts of each RunContext, and things in both Storage services.
void build_place(engine_core::DataModel& game) {
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& level = add_folder(game, workspace, "Level");
    add_folder(game, level.id(), "Inner");
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 4.0f;
    part.set_transform(moved);
    game.set_name(part.id(), "Part");
    game.set_parent(part.id(), level.id());
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), workspace);
    engine_core::Script& on_server = add_script(game, "ServerLogic", "print('secret')");
    engine_core::Script& on_client = add_script(game, "ClientLogic", "_G.client_ran = true");
    engine_core::Script& anywhere = add_script(game, "Shared", "_G.shared_ran = true");
    REQUIRE_FALSE(on_server.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    REQUIRE_FALSE(on_client.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    REQUIRE_FALSE(anywhere.set_run_context(static_cast<int>(engine_core::RunContext::Any)));
    add_folder(game, game.service("ServerStorage"), "Secrets");
    add_folder(game, game.service("ClientStorage"), "Effects");
}

}  // namespace

TEST_CASE("RP20 a snapshot builds the server's place on an empty client", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    build_place(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    REQUIRE(applier.joined());
    REQUIRE(applier.skipped() == 0);
    require_same_world(replicator, server.game, client.game);
}

TEST_CASE("RP21 ServerStorage and Core never arrive; ClientStorage does; a Server script is a shell", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    build_place(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    REQUIRE_FALSE(replicator.replicated(server.game.service("ServerStorage")));
    REQUIRE_FALSE(replicator.replicated(server.game.core()));
    REQUIRE(replicator.replicated(server.game.service("ClientStorage")));
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    REQUIRE(client.game.find_first_child(client.game.service("ServerStorage"), "Secrets") == 0);
    REQUIRE(client.game.find_first_child(client.game.service("ClientStorage"), "Effects") != 0);
    const InstanceId shell = client.game.find_first_child(client.game.scene_service("Workspace"), "ServerLogic");
    REQUIRE(shell != 0);
    REQUIRE(static_cast<engine_core::Script*>(client.game.instance(shell))->source().empty());
}

TEST_CASE("RP22 a service's changed properties arrive", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    engine_core::DataModel& lighting = *server.game.instance(server.game.scene_service("Lighting"));
    const engine_core::LuaField* brightness = engine_core::lua_class_find("Lighting", "Brightness");
    REQUIRE(brightness != nullptr);
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = 3.25;
    REQUIRE(brightness->write(server.game, lighting, slot));
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    engine_core::JsonValue value;
    REQUIRE(client.game.instance(client.game.scene_service("Lighting"))->read_property_json("Brightness", value));
    REQUIRE(value.as_number() == 3.25);
}

TEST_CASE("RP23 Connections arrive with their Ids, and LocalConnection is the client's own", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    engine_core::ConnectionInstance& first = add_connection(server.game);
    engine_core::ConnectionInstance& mine = add_connection(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(mine.id(), messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    require_same_world(replicator, server.game, client.game);
    auto* connections = static_cast<engine_core::Connections*>(client.game.instance(client.game.connections()));
    const InstanceId local = connections->local_connection();
    REQUIRE(local != 0);
    REQUIRE(static_cast<engine_core::ConnectionInstance*>(client.game.instance(local))->connection_id() ==
            mine.connection_id());
    REQUIRE(applier.local(first.id()) != engine_core::DataModel::kNoParent);
    REQUIRE(static_cast<engine_core::Connections*>(server.game.instance(server.game.connections()))->local_connection() == 0);
}

TEST_CASE("RP24 a reference to a later instance resolves; one into ServerStorage reads nil", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    const InstanceId workspace = server.game.scene_service("Workspace");
    engine_core::ObjectValue& early = server.game.create<engine_core::ObjectValue>();
    server.game.set_name(early.id(), "Early");
    server.game.set_parent(early.id(), workspace);
    engine_core::ObjectValue& hidden = server.game.create<engine_core::ObjectValue>();
    server.game.set_name(hidden.id(), "Hidden");
    server.game.set_parent(hidden.id(), workspace);
    engine_core::Folder& later = add_folder(server.game, workspace, "Later");
    engine_core::Folder& secret = add_folder(server.game, server.game.service("ServerStorage"), "Secret");
    early.set_value_object(later.id());
    hidden.set_value_object(secret.id());
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    const InstanceId cw = client.game.scene_service("Workspace");
    engine_core::LuaSlot slot;
    const engine_core::LuaField* value = engine_core::lua_class_find("ObjectValue", "Value");
    REQUIRE(value->read(client.game, *client.game.instance(client.game.find_first_child(cw, "Early")), slot));
    REQUIRE(slot.kind == engine_core::LuaSlot::Kind::Instance);
    REQUIRE(slot.id == client.game.find_first_child(cw, "Later"));
    REQUIRE(value->read(client.game, *client.game.instance(client.game.find_first_child(cw, "Hidden")), slot));
    REQUIRE(slot.id == 0);
}

TEST_CASE("RP25 client scripts in the snapshot run on the client; a Server shell does not", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    build_place(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    client.frames(2);
    bool ran = false;
    REQUIRE(client.runtime.global_boolean("client_ran", ran));
    REQUIRE(ran);
    REQUIRE(client.runtime.global_boolean("shared_ran", ran));
    REQUIRE(ran);
}

TEST_CASE("RP26 a large snapshot splits into parts; a missing or corrupt part is bad data", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    const InstanceId workspace = server.game.scene_service("Workspace");
    for (int i = 0; i < 2000; ++i) {
        add_folder(server.game, workspace, ("F" + std::to_string(i)).c_str());
    }
    replication::start_server(server);
    replication::start_client(client);
    engine_network::ReplicatorOptions options;
    options.snapshot_part_bytes = 4096;
    engine_network::Replicator replicator(server.game, options);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    REQUIRE(messages.size() > 1);
    {
        engine_network::ReplicaApplier applier(client.game);
        std::vector<std::vector<std::uint8_t>> skipped = messages;
        skipped.erase(skipped.begin() + 1);
        REQUIRE(replication::deliver(applier, skipped) == engine_network::ApplyResult::Bad);
    }
    {
        ScriptRig other;
        replication::start_client(other);
        engine_network::ReplicaApplier applier(other.game);
        std::vector<std::vector<std::uint8_t>> corrupt = messages;
        corrupt.back().back() ^= 0xFF;
        REQUIRE(replication::deliver(applier, corrupt) == engine_network::ApplyResult::Bad);
    }
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    require_same_world(replicator, server.game, client.game);
}
```

In RP24, use the real `ObjectValue` setter (see Task 2's note). A Changes message before the snapshot must be `Bad` too: add that assertion to RP26 once Task 5 exists (Task 5 adds it).

- [ ] **Step 3: Build to see the failure**

Expected: `Replicator.hpp` cannot be opened (create empty `.cpp` files first so CMake configures).

- [ ] **Step 4: Relax `read_ops`' Define rule (Task 3)**

In `TreeCodec.cpp` `read_ops`, the `Define` case becomes:

```cpp
            case Op::Kind::Define: {
                const std::uint64_t index = in.varuint();
                std::string text = in.string();
                if (in.failed()) {
                    return false;
                }
                // One table serves every client, so a client can be told a
                // string twice: again is fine when it says the same thing.
                if (index < strings.size()) {
                    if (*strings.at(static_cast<std::uint32_t>(index)) != text) {
                        return false;
                    }
                    continue;
                }
                if (index != strings.size()) {
                    return false;
                }
                strings.intern(text);
                continue;
            }
```

and `OpWriter` gains `void define_range(std::uint32_t from, std::uint32_t to);` (defines indices `[from, to)`; `define_all()` is `define_range(0, size)`). Add to RP14: a stream defining index 0 as "a" twice reads fine; defining index 0 as "a" then as "b" fails.

- [ ] **Step 5: `Replicator` (snapshot half)**

`Replicator.hpp` declares the Interfaces above plus, for Task 5, the private journal members it will use (declare them now so Task 5 only adds code):

```cpp
private:
    void on_change(const engine_core::TreeChange& change);
    // Source a client may hold: empty for a Server script, otherwise its source.
    static std::string client_source(const engine_core::DataModel& object);
    void write_record(OpWriter& writer, engine_core::InstanceId id) const;
    void write_subtree(OpWriter& writer, engine_core::InstanceId id);
    void forget_subtree(engine_core::InstanceId id);

    engine_core::DataModel& game_;
    ReplicatorOptions options_;
    engine_core::InstanceId core_ = 0;
    engine_core::InstanceId server_storage_ = 0;
    StringTable strings_;
    std::uint32_t broadcast_strings_ = 0;   // strings every joined client has been told (Task 5)
    std::uint64_t tick_ = 0;
    std::unordered_set<engine_core::InstanceId> known_;   // what clients hold, the root aside
    std::vector<engine_core::TreeChange> journal_;        // this tick's changes (Task 5)
```

`Replicator.cpp`:

```cpp
#include "Replicator.hpp"

#include "ByteBuffer.hpp"
#include "ConnectionInstance.hpp"
#include "LuaSource.hpp"
#include "Script.hpp"

#pragma warning(push, 0)
#include <zstd.h>
#pragma warning(pop)

#include <algorithm>
#include <stdexcept>

namespace engine_network {

using engine_core::DataModel;
using engine_core::InstanceId;

Replicator::Replicator(DataModel& game, ReplicatorOptions options)
    : game_(game), options_(options), core_(game.core()), server_storage_(game.service("ServerStorage")) {
    // Every replicated instance is one a client will hold once its snapshot lands.
    std::vector<InstanceId> pending{0};
    while (!pending.empty()) {
        const InstanceId id = pending.back();
        pending.pop_back();
        for (InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
            if (replicated(child)) {
                known_.insert(child);
                pending.push_back(child);
            }
        }
    }
    game_.set_change_sink([this](const engine_core::TreeChange& change) { on_change(change); });
}

Replicator::~Replicator() { game_.set_change_sink({}); }

bool Replicator::replicated(InstanceId id) const {
    if (id == 0) {
        return true;
    }
    if (!game_.alive(id)) {
        return false;
    }
    for (InstanceId at = id; at != 0; at = game_.parent(at)) {
        if (at == DataModel::kNoParent || at == core_ || at == server_storage_) {
            return false;
        }
    }
    return true;
}

std::uint64_t Replicator::tick() const { return tick_; }

void Replicator::on_change(const engine_core::TreeChange& change) { journal_.push_back(change); }

std::string Replicator::client_source(const DataModel& object) {
    const auto* lua = dynamic_cast<const engine_core::LuaSource*>(&object);
    if (lua == nullptr) {
        return std::string();
    }
    const auto* script = dynamic_cast<const engine_core::Script*>(&object);
    if (script != nullptr && script->run_context() == engine_core::RunContext::Server) {
        return std::string();
    }
    return lua->source();
}

void Replicator::write_record(OpWriter& writer, InstanceId id) const {
    const DataModel& object = *game_.instance(id);
    const InstanceId parent = game_.parent(id);
    if (object.is_service()) {
        // Every Game has its services: bind them by GUID, then what differs from their defaults.
        writer.bind(id, game_.guid(id));
        engine_core::PropertyBag properties;
        object.save_properties(properties);
        for (const auto& [key, value] : properties) {
            writer.set(id, key, value);
        }
        return;
    }
    engine_core::PropertyBag properties;
    object.save_properties(properties);
    if (const auto* connection = dynamic_cast<const engine_core::ConnectionInstance*>(&object)) {
        engine_core::bag_set(properties, "Id", engine_core::JsonValue::string(connection->connection_id()));
    }
    const bool has_source = dynamic_cast<const engine_core::LuaSource*>(&object) != nullptr;
    const std::string source = has_source ? client_source(object) : std::string();
    writer.create(id, parent, object.class_name(), game_.guid(id), game_.name(id), properties,
                  has_source ? &source : nullptr);
}

void Replicator::snapshot_messages(InstanceId local_connection, std::vector<std::vector<std::uint8_t>>& out) {
    ByteWriter body;
    body.u8(kCodecVersion);
    body.varuint(tick_);
    body.u32(local_connection);
    OpWriter writer(body, strings_);
    writer.define_all();
    // Parents before children, siblings in order.
    std::vector<InstanceId> pending;
    for (InstanceId child = game_.first_child(0); child != 0; child = game_.next_sibling(child)) {
        pending.push_back(child);
    }
    std::reverse(pending.begin(), pending.end());
    while (!pending.empty()) {
        const InstanceId id = pending.back();
        pending.pop_back();
        if (!replicated(id)) {
            continue;
        }
        write_record(writer, id);
        const std::size_t mark = pending.size();
        for (InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
            pending.push_back(child);
        }
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(mark), pending.end());
    }
    // Compress the body whole, then cut it into parts.
    const std::vector<std::uint8_t>& raw = body.data();
    std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (cctx == nullptr) {
        throw std::runtime_error("zstd: failed to create a compression context");
    }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
    const std::size_t written = ZSTD_compress2(cctx, packed.data(), packed.size(), raw.data(), raw.size());
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(written)) {
        throw std::runtime_error("zstd: compressing a snapshot failed");
    }
    packed.resize(written);
    const std::size_t part = std::max<std::size_t>(1, options_.snapshot_part_bytes);
    const std::uint32_t count = static_cast<std::uint32_t>((packed.size() + part - 1) / part);
    for (std::uint32_t index = 0; index < count; ++index) {
        ByteWriter message;
        message.u8(static_cast<std::uint8_t>(MessageType::SnapshotPart));
        message.u32(index);
        message.u32(count);
        message.varuint(raw.size());
        const std::size_t from = static_cast<std::size_t>(index) * part;
        const std::size_t size = std::min(part, packed.size() - from);
        message.bytes(packed.data() + from, size);
        out.push_back(std::move(message.data()));
    }
}

}  // namespace engine_network
```

(`count` is at least 1: zstd's output is never empty. `write_subtree`/`forget_subtree` are Task 5's; leave them declared but defined in Task 5.)

- [ ] **Step 6: `ReplicaApplier`**

`ReplicaApplier.hpp`: the Interfaces above, and private:

```cpp
private:
    ApplyResult snapshot_part(ByteReader& in);
    ApplyResult changes(ByteReader& in);
    // False only for bad data; a refused operation counts as skipped.
    bool apply(const Op& op);
    bool apply_create(const Op& op);

    engine_core::DataModel& game_;
    StringTable strings_;
    std::unordered_map<std::uint32_t, engine_core::InstanceId> local_;   // netid -> this world's id
    std::vector<std::uint8_t> parts_;   // compressed snapshot bytes so far
    std::uint32_t parts_seen_ = 0;
    std::uint32_t parts_count_ = 0;
    std::uint64_t raw_size_ = 0;
    bool joined_ = false;
    std::size_t skipped_ = 0;
    std::uint64_t server_tick_ = 0;
```

`ReplicaApplier.cpp`:

```cpp
#include "ReplicaApplier.hpp"

#include "ConnectionInstance.hpp"
#include "Connections.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"

#pragma warning(push, 0)
#include <zstd.h>
#pragma warning(pop)

namespace engine_network {

using engine_core::DataModel;
using engine_core::InstanceId;

namespace {
// A snapshot bigger than this is not one any server here would send.
constexpr std::uint64_t kMaxSnapshotBytes = 256ull << 20;
}  // namespace

ReplicaApplier::ReplicaApplier(DataModel& game) : game_(game) { local_.emplace(0, 0); }

InstanceId ReplicaApplier::local(std::uint32_t netid) const {
    const auto found = local_.find(netid);
    return found != local_.end() ? found->second : DataModel::kNoParent;
}

bool ReplicaApplier::joined() const { return joined_; }
std::size_t ReplicaApplier::skipped() const { return skipped_; }
std::uint64_t ReplicaApplier::server_tick() const { return server_tick_; }

ApplyResult ReplicaApplier::receive_message(const std::uint8_t* data, std::size_t size) {
    ByteReader in(data, size);
    switch (static_cast<MessageType>(in.u8())) {
        case MessageType::SnapshotPart: return snapshot_part(in);
        case MessageType::Changes: return changes(in);
        default: return ApplyResult::Bad;
    }
}

ApplyResult ReplicaApplier::snapshot_part(ByteReader& in) {
    const std::uint32_t index = in.u32();
    const std::uint32_t count = in.u32();
    const std::uint64_t raw_size = in.varuint();
    if (in.failed() || joined_ || count == 0 || index != parts_seen_ || raw_size > kMaxSnapshotBytes) {
        return ApplyResult::Bad;
    }
    if (index == 0) {
        parts_count_ = count;
        raw_size_ = raw_size;
    } else if (count != parts_count_ || raw_size != raw_size_) {
        return ApplyResult::Bad;
    }
    // The rest of the message is this part's compressed bytes.
    const std::size_t left = in.left();
    std::vector<std::uint8_t> bytes(left);
    for (std::size_t i = 0; i < left; ++i) bytes[i] = in.u8();
    parts_.insert(parts_.end(), bytes.begin(), bytes.end());
    ++parts_seen_;
    if (parts_seen_ < parts_count_) {
        return ApplyResult::Ok;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(raw_size_));
    const std::size_t written = ZSTD_decompress(raw.data(), raw.size(), parts_.data(), parts_.size());
    parts_.clear();
    parts_.shrink_to_fit();
    if (ZSTD_isError(written) || written != raw.size()) {
        return ApplyResult::Bad;
    }
    ByteReader body(raw.data(), raw.size());
    const std::uint8_t version = body.u8();
    server_tick_ = body.varuint();
    const std::uint32_t local_connection = body.u32();
    if (body.failed() || version != kCodecVersion) {
        return ApplyResult::Bad;
    }
    std::vector<Op> ops;
    if (!read_ops(body, strings_, ops)) {
        return ApplyResult::Bad;
    }
    for (const Op& op : ops) {
        if (!apply(op)) {
            return ApplyResult::Bad;
        }
    }
    joined_ = true;
    if (local_connection != 0) {
        const InstanceId mine = local(local_connection);
        auto* connections = dynamic_cast<engine_core::Connections*>(game_.instance(game_.connections()));
        if (connections != nullptr && mine != DataModel::kNoParent && game_.alive(mine)) {
            connections->set_local_connection(mine);
        }
    }
    return ApplyResult::SnapshotApplied;
}

ApplyResult ReplicaApplier::changes(ByteReader& in) {
    const std::uint8_t version = in.u8();
    const std::uint64_t tick = in.varuint();
    if (in.failed() || !joined_ || version != kCodecVersion) {
        return ApplyResult::Bad;
    }
    std::vector<Op> ops;
    if (!read_ops(in, strings_, ops)) {
        return ApplyResult::Bad;
    }
    server_tick_ = tick;
    for (const Op& op : ops) {
        if (!apply(op)) {
            return ApplyResult::Bad;
        }
    }
    return ApplyResult::Ok;
}

bool ReplicaApplier::apply(const Op& op) {
    const auto alive_local = [&](std::uint32_t netid) -> InstanceId {
        const InstanceId id = local(netid);
        return id != DataModel::kNoParent && (id == 0 || game_.alive(id)) ? id : DataModel::kNoParent;
    };
    switch (op.kind) {
        case Op::Kind::Bind: {
            // Only a service, or Connections, is bound: never something a client could make.
            const std::optional<InstanceId> found = engine_core::valid_guid(op.text) ? game_.find_guid(op.text) : std::nullopt;
            const DataModel* object = found ? game_.instance(*found) : nullptr;
            if (object == nullptr || !object->is_service()) {
                return false;
            }
            local_[op.netid] = *found;
            return true;
        }
        case Op::Kind::Create:
            return apply_create(op);
        case Op::Kind::Destroy: {
            const InstanceId id = alive_local(op.netid);
            local_.erase(op.netid);
            if (id == DataModel::kNoParent || id == 0 || game_.destroy_error(id)) {
                ++skipped_;
                return true;
            }
            game_.destroy_tree(id);
            return true;
        }
        case Op::Kind::Parent: {
            const InstanceId id = alive_local(op.netid);
            const InstanceId parent = alive_local(op.parent);
            if (id == DataModel::kNoParent || id == 0 || parent == DataModel::kNoParent || game_.parent_error(id, parent)) {
                ++skipped_;
                return true;
            }
            game_.set_parent(id, parent);
            return true;
        }
        case Op::Kind::Rename: {
            const InstanceId id = alive_local(op.netid);
            if (id == DataModel::kNoParent || id == 0 || game_.rename_error(id, op.text)) {
                ++skipped_;
                return true;
            }
            game_.set_name(id, op.text);
            return true;
        }
        case Op::Kind::Set: {
            const InstanceId id = alive_local(op.netid);
            DataModel* object = id == DataModel::kNoParent ? nullptr : (id == 0 ? &game_ : game_.instance(id));
            std::string error;
            if (object == nullptr || op.properties.size() != 1 ||
                !object->load_property(op.properties[0].first, op.properties[0].second, error) || !error.empty()) {
                ++skipped_;
            }
            return true;
        }
        case Op::Kind::Source: {
            const InstanceId id = alive_local(op.netid);
            auto* lua = id == DataModel::kNoParent || id == 0 ? nullptr : dynamic_cast<engine_core::LuaSource*>(game_.instance(id));
            if (lua == nullptr) {
                ++skipped_;
                return true;
            }
            lua->set_source(op.text);
            return true;
        }
        default:
            return false;
    }
}

bool ReplicaApplier::apply_create(const Op& op) {
    if (local_.count(op.netid) != 0 || op.netid == 0 || !engine_core::valid_guid(op.text)) {
        return false;   // a second create of one instance, or a GUID no server makes: bad data
    }
    const auto parent_it = local_.find(op.parent);
    const InstanceId parent = parent_it != local_.end() ? parent_it->second : DataModel::kNoParent;
    if (parent == DataModel::kNoParent || (parent != 0 && !game_.alive(parent)) ||
        !engine_core::lua_creatable_known(op.class_name.c_str()) || game_.room_left() == 0 || game_.find_guid(op.text)) {
        ++skipped_;
        return true;
    }
    DataModel* made = engine_core::lua_create_instance(game_, op.class_name.c_str());
    if (made == nullptr) {
        ++skipped_;
        return true;
    }
    const InstanceId id = made->id();
    game_.set_guid(id, op.text);
    if (!game_.rename_error(id, op.name)) {
        game_.set_name(id, op.name);
    }
    for (const auto& [key, value] : op.properties) {
        if (key == "Id") {
            if (auto* connection = dynamic_cast<engine_core::ConnectionInstance*>(made); connection != nullptr && value.is_string()) {
                connection->set_connection_id(value.as_string());
                continue;
            }
        }
        std::string error;
        if (!made->load_property(key, value, error) || !error.empty()) {
            ++skipped_;
        }
    }
    if (op.has_source) {
        if (auto* lua = dynamic_cast<engine_core::LuaSource*>(made)) {
            lua->set_source(op.source);
        }
    }
    // Parent last, so a Script starts with its Source and RunContext in place.
    if (game_.parent_error(id, parent)) {
        game_.destroy_tree(id);
        ++skipped_;
        return true;
    }
    game_.set_parent(id, parent);
    local_[op.netid] = id;
    return true;
}

}  // namespace engine_network
```

Check against the real API and adjust without changing behavior: `find_guid` returns `std::optional<InstanceId>`; `parent_error`/`rename_error`/`destroy_error` return `std::optional<std::string>` (truthy when refused); `valid_guid` is a free function in `engine_core`; a `Bind` naming the root (`game`'s GUID) is not needed because netid 0 is pre-bound. Read `apply` once more for `Set` on netid 0 (the root): `&game_` is the root `DataModel`; the server never sends that today, but it must not crash.

- [ ] **Step 7: Build and run**

Run `"[replication]"`. Expected: RP20-RP26 pass; RP1-RP14 still pass.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/engine_network sandbox/replication_support.hpp sandbox/replication_snapshot_tests.cpp sandbox/replication_codec_tests.cpp
git commit -m "Network: a join snapshot carries the server's filtered place to an empty client

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Each tick's changes: journal, coalesce, encode once, apply

**Files:**
- Modify: `src/engine_network/Replicator.hpp/.cpp` (`end_tick`, `write_subtree`, `forget_subtree`), `sandbox/replication_snapshot_tests.cpp` (RP26 gains one assertion)
- Create: `sandbox/replication_changes_tests.cpp`; add to the sandbox list

**Interfaces:**
- Consumes: Task 4's `Replicator` members and `ReplicaApplier`, which already applies every op kind.
- Produces:
  ```cpp
  // Replicator:
  // Everything that changed since the last call, as one Changes message for
  // every joined client. False, with message left empty, when nothing did.
  bool end_tick(std::vector<std::uint8_t>& message);
  ```

The rules, applied to the tick's journal (in this order in the message):
1. **Creates and moves, shallowest first.** For each instance reported `Parented` this tick that is still alive, sorted by depth: if it was orphaned by a destroy this tick and is still nowhere, forget it and what it holds without an op (the client's `destroy_tree` of the destroyed parent takes it). If it is replicated and clients do not know it, `Create` it and its replicated subtree (children a client already knows are skipped: they moved here this tick and get their own `Parent` op). If clients know it and it was not created this tick, `Parent` it. If it is no longer replicated (moved into `ServerStorage`, under Core, or nowhere), it leaves in step 2.
2. **Destroys.** First what left the filter (`Destroy`, and forget its known subtree), then what was destroyed and known (`Destroy`). Moves come before destroys so an instance moved out of something destroyed this tick is safe.
3. **Renames** of known instances not created this tick, with the current name.
4. **Property writes** of known instances not created this tick, one `Set` per property with its current value (`read_property_json`; a name it does not answer, such as a view-local or derived property, is skipped). `Source` is a `Source` op with what a client may hold; a Server script's source edits are never sent, except when its `RunContext` changed this tick (then its source, or empty, is sent).

Property names from a change: `Field::Transform` → `Transform`, `Source` → `Source`, `Enabled` → `Enabled`, `Simulated` → `Simulated`, `VisualOnly` → `VisualOnly`, `Reflected` → `lua_property_name(payload)`; `LinearVelocity` is never sent. A `RunContext` change also marks `Source`.

- [ ] **Step 1: Write the failing tests**

`sandbox/replication_changes_tests.cpp`:

```cpp
// Each tick's changes reach a joined client and leave it with the server's
// tree (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "replication_support.hpp"

#include "Folder.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

using engine_core::InstanceId;
using replication::require_same_world;

namespace {

// A server and one joined client, stepped by hand.
struct Pair {
    ScriptRig server;
    ScriptRig client;
    std::unique_ptr<engine_network::Replicator> replicator;
    std::unique_ptr<engine_network::ReplicaApplier> applier;

    Pair() {
        replication::start_server(server);
        replication::start_client(client);
        replicator = std::make_unique<engine_network::Replicator>(server.game);
        applier = std::make_unique<engine_network::ReplicaApplier>(client.game);
        std::vector<std::vector<std::uint8_t>> messages;
        replicator->snapshot_messages(0, messages);
        REQUIRE(replication::deliver(*applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    }
    // Ends the server's tick and applies what it sends. False when it sent nothing.
    bool tick() {
        std::vector<std::uint8_t> message;
        if (!replicator->end_tick(message)) {
            REQUIRE(message.empty());
            return false;
        }
        REQUIRE(applier->receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
        return true;
    }
    InstanceId client_id(InstanceId server_id) const { return applier->local(server_id); }
    void same() { require_same_world(*replicator, server.game, client.game); }
};

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

// Every live instance in a world sits somewhere: nothing leaked unparented.
void require_no_orphans(engine_core::DataModel& game) {
    game.for_each_instance([&](engine_core::DataModel& object) {
        INFO(object.class_name());
        REQUIRE(game.parent(object.id()) != engine_core::DataModel::kNoParent);
    });
}

}  // namespace

TEST_CASE("RP30 creates, property writes, renames and source edits reach the client", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& folder = add_folder(game, workspace, "Box");
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), folder.id());
    REQUIRE(pair.tick());
    pair.same();
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[13] = 7.0f;
    lamp.set_transform(moved);
    game.set_name(folder.id(), "Crate");
    engine_core::Script& script = add_script(game, "Mover", "-- one");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    REQUIRE(pair.tick());
    script.set_source("-- two");
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(static_cast<engine_core::PointLight*>(pair.client.game.instance(pair.client_id(lamp.id())))->transform().m[13] == 7.0f);
    REQUIRE(pair.applier->skipped() == 0);
}

TEST_CASE("RP31 the last write wins, a create and destroy in one tick sends nothing, a quiet tick sends nothing", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    REQUIRE_FALSE(pair.tick());
    engine_core::Folder& gone = add_folder(game, workspace, "Gone");
    game.destroy(gone.id());
    REQUIRE_FALSE(pair.tick());
    engine_core::Folder& folder = add_folder(game, workspace, "A");
    REQUIRE(pair.tick());
    game.set_name(folder.id(), "B");
    game.set_name(folder.id(), "C");
    std::vector<std::uint8_t> message;
    REQUIRE(pair.replicator->end_tick(message));
    engine_network::StringTable strings;
    engine_network::ByteReader in(message.data() + 1, message.size() - 1);   // past the message type
    in.u8();
    in.varuint();
    std::vector<engine_network::Op> ops;
    REQUIRE(engine_network::read_ops(in, strings, ops));
    REQUIRE(ops.size() == 1);
    REQUIRE(ops[0].kind == engine_network::Op::Kind::Rename);
    REQUIRE(ops[0].text == "C");
}

TEST_CASE("RP32 destroy_tree removes a subtree; a plain destroy's orphans leave too, leaking nothing", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& tree = add_folder(game, workspace, "Tree");
    add_folder(game, add_folder(game, tree.id(), "Branch").id(), "Leaf");
    engine_core::Folder& plain = add_folder(game, workspace, "Plain");
    add_folder(game, add_folder(game, plain.id(), "Kid").id(), "Grandkid");
    REQUIRE(pair.tick());
    game.destroy_tree(tree.id());
    game.destroy(plain.id());   // Kid (with Grandkid) is left nowhere on the server
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(pair.applier->skipped() == 0);
    require_no_orphans(pair.client.game);
}

TEST_CASE("RP33 an instance moved into ServerStorage disappears; moved back, it returns whole", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& box = add_folder(game, workspace, "Box");
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), box.id());
    REQUIRE(pair.tick());
    game.set_parent(box.id(), game.service("ServerStorage"));
    REQUIRE(pair.tick());
    REQUIRE(pair.client.game.find_first_child(pair.client.game.scene_service("Workspace"), "Box") == 0);
    // Changed while hidden: the client hears nothing, and gets the current state when it returns.
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 9.0f;
    lamp.set_transform(moved);
    REQUIRE_FALSE(pair.tick());
    game.set_parent(box.id(), workspace);
    REQUIRE(pair.tick());
    pair.same();
    require_no_orphans(pair.client.game);
}

TEST_CASE("RP34 reparents work, under something new this tick, and when two swap places", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& a = add_folder(game, workspace, "A");
    engine_core::Folder& b = add_folder(game, a.id(), "B");
    engine_core::Folder& c = add_folder(game, workspace, "C");
    REQUIRE(pair.tick());
    engine_core::Folder& fresh = add_folder(game, workspace, "Fresh");
    game.set_parent(c.id(), fresh.id());
    REQUIRE(pair.tick());
    pair.same();
    // A held B; now B holds A.
    game.set_parent(b.id(), workspace);
    game.set_parent(a.id(), b.id());
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(pair.applier->skipped() == 0);
}

TEST_CASE("RP35 RunContext decides what source a client holds; a Server script's edits are never sent", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::Script& script = add_script(game, "Logic", "-- secret");
    REQUIRE(pair.tick());
    pair.same();   // a Server shell: empty source
    script.set_source("-- still secret");
    REQUIRE_FALSE(pair.tick());
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    REQUIRE(pair.tick());
    pair.same();   // now the client holds the source
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    REQUIRE(pair.tick());
    pair.same();   // and it is emptied again
}

TEST_CASE("RP36 quiet physics writes and view-local properties send nothing", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    game.set_parent(object.id(), game.scene_service("Workspace"));
    REQUIRE(pair.tick());
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 2.0f;
    game.write_simulated_transform(object.id(), moved);
    REQUIRE_FALSE(pair.tick());
}

TEST_CASE("RP37 a client's own writes stay local until the server writes that property", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::Folder& folder = add_folder(game, game.scene_service("Workspace"), "Shared");
    REQUIRE(pair.tick());
    const InstanceId mine = pair.client_id(folder.id());
    pair.client.game.set_name(mine, "ClientName");
    REQUIRE_FALSE(pair.tick());
    REQUIRE(pair.client.game.name(mine) == "ClientName");
    game.set_name(folder.id(), "ServerName");
    REQUIRE(pair.tick());
    REQUIRE(pair.client.game.name(mine) == "ServerName");
}

TEST_CASE("RP38 an operation on an instance the client destroyed is skipped and counted", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::Folder& folder = add_folder(game, game.scene_service("Workspace"), "Doomed");
    REQUIRE(pair.tick());
    pair.client.game.destroy(pair.client_id(folder.id()));
    game.set_name(folder.id(), "Renamed");
    REQUIRE(pair.tick());
    REQUIRE(pair.applier->skipped() == 1);
}

TEST_CASE("RP39 a string only a later client's snapshot named reaches the first client in the next change", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    // Created at the identity transform, so its Create names no Transform.
    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    game.set_parent(object.id(), game.scene_service("Workspace"));
    REQUIRE(pair.tick());
    // Moved by a quiet write, as physics moves things: no message says so.
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 5.0f;
    game.write_simulated_transform(object.id(), moved);
    REQUIRE_FALSE(pair.tick());
    // A second client joins: its snapshot is the first message to name "Transform".
    ScriptRig second;
    replication::start_client(second);
    engine_network::ReplicaApplier second_applier(second.game);
    std::vector<std::vector<std::uint8_t>> snapshot;
    pair.replicator->snapshot_messages(0, snapshot);
    REQUIRE(replication::deliver(second_applier, snapshot) == engine_network::ApplyResult::SnapshotApplied);
    // A checked write of Transform: both clients must read the property's name.
    moved.m[12] = 6.0f;
    object.set_transform(moved);
    std::vector<std::uint8_t> message;
    REQUIRE(pair.replicator->end_tick(message));
    REQUIRE(pair.applier->receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
    REQUIRE(second_applier.receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
    pair.same();
    require_same_world(*pair.replicator, game, second.game);
}
```

Append to RP26 (in `replication_snapshot_tests.cpp`), on a fresh applier that has not joined: a Changes message from `end_tick` (after any change on the server) returns `ApplyResult::Bad`.

- [ ] **Step 2: Build to see the failure**

Expected: `end_tick` is not a member.

- [ ] **Step 3: Implement**

In `Replicator.cpp`:

```cpp
namespace {

// The property a change names, or "" for one never sent.
std::string property_of(const engine_core::TreeChange& change) {
    using engine_core::Field;
    switch (change.field) {
        case Field::Transform: return "Transform";
        case Field::Source: return "Source";
        case Field::Enabled: return "Enabled";
        case Field::Simulated: return "Simulated";
        case Field::VisualOnly: return "VisualOnly";
        case Field::Reflected: return engine_core::lua_property_name(static_cast<std::uint32_t>(change.payload));
        default: return std::string();
    }
}

}  // namespace

int Replicator::depth(InstanceId id) const {
    int depth = 0;
    for (InstanceId at = id; at != 0 && at != DataModel::kNoParent; at = game_.parent(at)) {
        ++depth;
    }
    return depth;
}

void Replicator::write_subtree(OpWriter& writer, InstanceId id, std::unordered_set<InstanceId>& created) {
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        write_record(writer, next);
        known_.insert(next);
        created.insert(next);
        const std::size_t mark = pending.size();
        for (InstanceId child = game_.first_child(next); child != 0; child = game_.next_sibling(child)) {
            // A child clients already hold moved here this tick: it gets its own Parent op.
            if (replicated(child) && known_.count(child) == 0) {
                pending.push_back(child);
            }
        }
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(mark), pending.end());
    }
}

void Replicator::forget_subtree(InstanceId id) {
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        known_.erase(next);
        if (!game_.alive(next)) {
            continue;
        }
        for (InstanceId child = game_.first_child(next); child != 0; child = game_.next_sibling(child)) {
            pending.push_back(child);
        }
    }
}

bool Replicator::end_tick(std::vector<std::uint8_t>& message) {
    message.clear();
    ++tick_;
    std::vector<engine_core::TreeChange> journal;
    journal.swap(journal_);

    std::vector<InstanceId> destroyed;
    std::unordered_set<InstanceId> destroyed_set;
    std::vector<InstanceId> moved;
    std::unordered_set<InstanceId> moved_set;
    std::unordered_set<InstanceId> orphaned;
    std::vector<InstanceId> renamed;
    std::unordered_set<InstanceId> renamed_set;
    std::vector<std::pair<InstanceId, std::string>> written;
    std::set<std::pair<InstanceId, std::string>> written_set;
    std::unordered_set<InstanceId> context_changed;
    const auto note_write = [&](InstanceId id, std::string name) {
        if (written_set.emplace(id, name).second) {
            written.emplace_back(id, std::move(name));
        }
    };
    for (const engine_core::TreeChange& change : journal) {
        switch (change.kind) {
            case engine_core::TreeChange::Kind::Destroyed:
                if (destroyed_set.insert(change.id).second) destroyed.push_back(change.id);
                break;
            case engine_core::TreeChange::Kind::Parented:
                if (moved_set.insert(change.id).second) moved.push_back(change.id);
                if (destroyed_set.count(change.previous_parent) != 0) orphaned.insert(change.id);
                break;
            case engine_core::TreeChange::Kind::Renamed:
                if (renamed_set.insert(change.id).second) renamed.push_back(change.id);
                break;
            case engine_core::TreeChange::Kind::Property: {
                std::string name = property_of(change);
                if (name.empty()) break;
                if (name == "RunContext") {
                    context_changed.insert(change.id);
                    note_write(change.id, "Source");
                }
                note_write(change.id, std::move(name));
                break;
            }
        }
    }

    ByteWriter out;
    out.u8(static_cast<std::uint8_t>(MessageType::Changes));
    out.u8(kCodecVersion);
    out.varuint(tick_);
    OpWriter writer(out, strings_);
    // Strings a later snapshot added that the clients already joined have not heard.
    writer.define_range(broadcast_strings_, static_cast<std::uint32_t>(strings_.size()));
    std::unordered_set<InstanceId> created;

    // 1. Creates and moves, shallowest first.
    std::vector<std::pair<int, InstanceId>> by_depth;
    for (const InstanceId id : moved) {
        if (game_.alive(id)) by_depth.emplace_back(depth(id), id);
    }
    std::stable_sort(by_depth.begin(), by_depth.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<InstanceId> left_filter;
    for (const auto& [unused, id] : by_depth) {
        (void)unused;
        if (orphaned.count(id) != 0 && game_.parent(id) == DataModel::kNoParent) {
            forget_subtree(id);   // the client's destroy_tree of its parent takes it
            continue;
        }
        if (replicated(id)) {
            if (created.count(id) != 0) continue;
            if (known_.count(id) == 0) {
                write_subtree(writer, id, created);
            } else {
                writer.parent(id, game_.parent(id));
            }
        } else if (known_.count(id) != 0) {
            left_filter.push_back(id);
        }
    }
    // 2. Destroys: what left the filter, then what was destroyed.
    for (const InstanceId id : left_filter) {
        if (known_.count(id) != 0) {
            writer.destroy(id);
            forget_subtree(id);
        }
    }
    for (const InstanceId id : destroyed) {
        if (known_.erase(id) != 0) writer.destroy(id);
    }
    // 3. Renames.
    for (const InstanceId id : renamed) {
        if (known_.count(id) != 0 && created.count(id) == 0 && game_.alive(id)) writer.rename(id, game_.name(id));
    }
    // 4. Property writes, with their current values.
    for (const auto& [id, name] : written) {
        if (known_.count(id) == 0 || created.count(id) != 0 || !game_.alive(id)) continue;
        const DataModel& object = *game_.instance(id);
        if (name == "Source") {
            if (dynamic_cast<const engine_core::LuaSource*>(&object) == nullptr) continue;
            const auto* script = dynamic_cast<const engine_core::Script*>(&object);
            const bool shell = script != nullptr && script->run_context() == engine_core::RunContext::Server;
            if (shell && context_changed.count(id) == 0) continue;   // a Server script's edits stay on the server
            writer.source(id, client_source(object));
            continue;
        }
        engine_core::JsonValue value;
        if (object.read_property_json(name, value)) writer.set(id, name, value);
    }

    if (writer.ops() == 0) {
        return false;   // the pending Define ops wait for the next message that carries an op
    }
    broadcast_strings_ = static_cast<std::uint32_t>(strings_.size());
    message = std::move(out.data());
    return true;
}
```

Declare in `Replicator.hpp`: `int depth(engine_core::InstanceId id) const;` and change `write_subtree` to `void write_subtree(OpWriter& writer, engine_core::InstanceId id, std::unordered_set<engine_core::InstanceId>& created);`. Includes: `<set>`, `<unordered_set>`, `LuaApi.hpp`.

Two details to check while implementing:
- `Destroyed` for an instance clients know, in the same tick as `Parented` reports for its orphans, must still send exactly one `Destroy` for it: the orphans are forgotten in step 1 (no op) and the parent's `Destroy` follows in step 2. RP32 proves it.
- A `Rename` or `Set` for an instance destroyed later the same tick is skipped (`alive` check) and its `Destroy` is sent.

- [ ] **Step 4: Build and run**

Run `"[replication]"`. Expected: RP30-RP39 pass, RP26's new assertion passes, everything earlier still passes.

- [ ] **Step 5: Commit**

```bash
git add src/engine_network/Replicator.hpp src/engine_network/Replicator.cpp sandbox/replication_changes_tests.cpp sandbox/replication_snapshot_tests.cpp CMakeLists.txt
git commit -m "Network: each tick's tree changes, coalesced and filtered, reach every joined client

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Server and client sessions over the transport

**Files:**
- Create: `src/engine_network/Sessions.hpp`, `src/engine_network/ServerSession.cpp`, `src/engine_network/ClientSession.cpp`, `sandbox/replication_session_tests.cpp`
- Modify: `CMakeLists.txt` (the two `.cpp` in `engine_network`; the test in the sandbox list), `src/engine_network/README.md`

**Interfaces:**
- Consumes: `Replicator`, `ReplicaApplier`, `ApplyResult` (Tasks 4-5); `network::Endpoint`, `EndpointConfig`, `EndpointCallbacks`, `PeerId`, `ConnectionState`, `DisconnectReason`, `DenyReason`, `Address`, `ISocket`; `engine_core::INetworkPump` (Task 1); `make_connection_uuid`, `ConnectionInstance`.
- Produces (namespace `engine_network`, all in `Sessions.hpp`):
  ```cpp
  struct SessionConfig {
      network::EndpointConfig endpoint;            // role is set by the session
      ReplicatorOptions replicator;
      std::size_t max_queued_messages = 3600;      // about a minute of ticks waiting on one client
      std::size_t max_queued_bytes = 64u << 20;
  };
  class ServerSession : public engine_core::INetworkPump {
  public:
      ServerSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
      ~ServerSession() override;
      void receive(double now) override;
      void send(double now) override;
      void close(double now);                                   // disconnects every client
      network::Endpoint& endpoint();
      Replicator& replicator();
      engine_core::InstanceId connection_of(network::PeerId peer) const;   // 0 when none
      std::size_t dropped_messages() const;                     // what clients sent; nothing yet has meaning
      std::size_t fell_behind() const;                          // clients dropped for an overfull queue
  };
  enum class ClientState { Idle, Connecting, Joining, Joined, Disconnected };
  class ClientSession : public engine_core::INetworkPump {
  public:
      ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config);
      ~ClientSession() override;
      bool connect(const network::Address& server, double now);
      void disconnect(double now);                              // the player leaves; reason "You left the game"
      void receive(double now) override;
      void send(double now) override;
      ClientState state() const;
      const std::string& reason() const;                        // why it ended, worded for a player; empty until then
      network::Endpoint& endpoint();
      ReplicaApplier& applier();
  };
  }
  ```

Behavior:
- **Server, a client connects** (`on_connected`, inside `receive`): create a `Connection` with `set_connection_id(make_connection_uuid())` under `game.connections()` and remember it for the peer. Its snapshot goes at the end of this tick.
- **Server `send(now)`**: `replicator.end_tick(changes)`; queue `changes` for every peer already joined; then for each newly connected peer, queue `replicator.snapshot_messages(its Connection)` and mark it joined (it gets `changes` from the next tick on); then flush each peer's queue into `endpoint.send_message` until it refuses (window full); a peer whose queue passes `max_queued_messages` or `max_queued_bytes` is disconnected and counted in `fell_behind`; finally `endpoint.send(now)`.
- **Server, a client leaves** (`on_disconnected`): destroy its `Connection` if alive; forget its queue.
- **Server, a client sends a message** (`on_message`): count it in `dropped_messages`, nothing else.
- **Client `connect`**: `endpoint.connect`; state Connecting.
- **Client, connected** (`on_connected`): state Joining; the world becomes a client, playing while still empty: `set_world_role(Client)`, then if not playing `capture_place()` and `start_simulation()`.
- **Client, a message** (`on_message`, inside `receive`, so before scripts): `applier.receive_message`; `SnapshotApplied` → Joined; `Bad` → `endpoint.disconnect(0, now)`, reason "Bad data from the server".
- **Client, disconnected**: state Disconnected; stop play if playing (`stop_simulation()` puts back the empty place); reason, unless already set: Denied WrongGame "This server runs a different game"; WrongVersion "This game needs an update (the server runs " + `endpoint.denied_version()` + ")"; WrongContent "This game's files differ from the server's"; Full "The server is full"; Timeout "Could not reach the server" before Joining, else "Lost connection to the server"; Requested "The server closed the connection"; Socket "Network error".

- [ ] **Step 1: Write the failing tests**

`sandbox/replication_session_tests.cpp`:

```cpp
// Sessions: a server and its clients over the transport, with Connections,
// join snapshots, and every way a connection ends
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "replication_support.hpp"

#include "Connections.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <memory>

using engine_core::InstanceId;

namespace {

network::GameIdentity identity(std::uint64_t hash = 7) {
    network::GameIdentity id;
    network::parse_game_id("0123456789abcdef0123456789abcdef", id.game_id);
    id.resources_hash = hash;
    return id;
}

const network::Address kServer = network::Address::loopback(1);
network::Address client_address(int n) { return network::Address::loopback(static_cast<std::uint16_t>(100 + n)); }

struct Client {
    ScriptRig rig;
    std::unique_ptr<engine_network::ClientSession> session;
};

// A server world and its clients, each world stepped by hand as an engine steps it.
struct Net {
    network::LinkHub hub;
    ScriptRig server;
    std::unique_ptr<engine_network::ServerSession> session;
    std::deque<Client> clients;
    double now = 0.0;

    explicit Net(engine_network::SessionConfig config = {}) {
        config.endpoint.identity = identity();
        replication::start_server(server);
        session = std::make_unique<engine_network::ServerSession>(server.game, hub.open(kServer), config);
    }
    Client& add_client(network::GameIdentity id = identity()) {
        clients.emplace_back();
        Client& client = clients.back();
        engine_network::SessionConfig config;
        config.endpoint.identity = id;
        client.session = std::make_unique<engine_network::ClientSession>(
            client.rig.game, hub.open(client_address(static_cast<int>(clients.size()) - 1)), config);
        REQUIRE(client.session->connect(kServer, now));
        return client;
    }
    // One tick: receive, scripts, send, for every world.
    void tick() {
        now += 1.0 / 60.0;
        session->receive(now);
        for (Client& c : clients) c.session->receive(now);
        server.frames(1);
        for (Client& c : clients) c.rig.frames(1);
        session->send(now);
        for (Client& c : clients) c.session->send(now);
    }
    void run(double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) tick();
    }
    void same(Client& client) {
        replication::require_same_world(session->replicator(), server.game, client.rig.game);
    }
};

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

}  // namespace

TEST_CASE("RP40 a client joins: it gets a Connection, the world, and its LocalConnection", "[replication][session]") {
    Net net;
    add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Level");
    Client& client = net.add_client();
    net.run(0.5);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(client.rig.game.world_role() == engine_core::WorldRole::Client);
    REQUIRE(client.rig.game.simulation_running());
    net.same(client);
    const InstanceId server_connection = net.session->connection_of(0);
    REQUIRE(server_connection != 0);
    auto* local = static_cast<engine_core::Connections*>(client.rig.game.instance(client.rig.game.connections()));
    REQUIRE(local->local_connection() != 0);
    REQUIRE(static_cast<engine_core::ConnectionInstance*>(client.rig.game.instance(local->local_connection()))->connection_id() ==
            static_cast<engine_core::ConnectionInstance*>(net.server.game.instance(server_connection))->connection_id());
    auto* on_server = static_cast<engine_core::Connections*>(net.server.game.instance(net.server.game.connections()));
    REQUIRE(on_server->local_connection() == 0);
}

TEST_CASE("RP41 changes made while a client joins reach it once, after its snapshot", "[replication][session]") {
    engine_network::SessionConfig config;
    config.replicator.snapshot_part_bytes = 2048;   // a snapshot several ticks long
    Net net(config);
    const InstanceId workspace = net.server.game.scene_service("Workspace");
    for (int i = 0; i < 400; ++i) {
        add_folder(net.server.game, workspace, ("F" + std::to_string(i)).c_str());
    }
    Client& client = net.add_client();
    engine_core::Folder& busy = add_folder(net.server.game, workspace, "Busy");
    for (int i = 0; i < 240; ++i) {
        net.server.game.set_name(busy.id(), "Busy" + std::to_string(i));
        if (i % 20 == 0) add_folder(net.server.game, busy.id(), ("Kid" + std::to_string(i)).c_str());
        net.tick();
    }
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    net.same(client);
    REQUIRE(client.session->applier().skipped() == 0);
}

TEST_CASE("RP42 two clients see the same world, and see each other leave", "[replication][session]") {
    Net net;
    Client& a = net.add_client();
    Client& b = net.add_client();
    net.run(0.5);
    add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Shared");
    net.run(0.2);
    net.same(a);
    net.same(b);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 2);
    b.session->disconnect(net.now);
    net.run(0.5);
    REQUIRE(b.session->reason() == "You left the game");
    REQUIRE(net.server.game.get_children(net.server.game.connections()).size() == 1);
    REQUIRE(a.rig.game.get_children(a.rig.game.connections()).size() == 1);
    REQUIRE(b.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE_FALSE(b.rig.game.simulation_running());   // play stopped: the empty place is back
}

TEST_CASE("RP43 a message from a client is dropped and counted", "[replication][session]") {
    Net net;
    Client& client = net.add_client();
    net.run(0.5);
    const std::uint8_t hello[3] = {1, 2, 3};
    REQUIRE(client.session->endpoint().send_message(0, hello, sizeof(hello)));
    net.run(0.3);
    REQUIRE(net.session->dropped_messages() == 1);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
}

TEST_CASE("RP44 a client of another game or version is told why", "[replication][session]") {
    Net net;
    network::GameIdentity other = identity();
    other.game_id[0] ^= 0xFF;
    Client& wrong_game = net.add_client(other);
    network::GameIdentity old = identity();
    old.version = "0.9.0";
    Client& wrong_version = net.add_client(old);
    net.run(0.5);
    REQUIRE(wrong_game.session->state() == engine_network::ClientState::Disconnected);
    REQUIRE(wrong_game.session->reason() == "This server runs a different game");
    REQUIRE(wrong_version.session->reason() == "This game needs an update (the server runs 1.0.0)");
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
}

TEST_CASE("RP45 the server closing tells clients so; a client that stops hearing the server says it lost it", "[replication][session]") {
    Net net;
    Client& closed = net.add_client();
    net.run(0.5);
    net.session->close(net.now);
    net.run(0.5);
    REQUIRE(closed.session->reason() == "The server closed the connection");
    Net silent;
    Client& lost = silent.add_client();
    silent.run(0.5);
    silent.session->endpoint().conditioner().loss = 1.0f;   // nothing more reaches the client
    silent.run(6.0);
    REQUIRE(lost.session->reason() == "Lost connection to the server");
}

TEST_CASE("RP46 a client too far behind is dropped", "[replication][session]") {
    engine_network::SessionConfig config;
    config.max_queued_messages = 4;
    Net net(config);
    Client& client = net.add_client();
    net.run(0.5);
    net.session->endpoint().conditioner().loss = 1.0f;   // the client never acks
    engine_core::Folder& churn = add_folder(net.server.game, net.server.game.scene_service("Workspace"), "Churn");
    for (int i = 0; i < 60 * 5 && net.session->fell_behind() == 0; ++i) {
        net.server.game.set_name(churn.id(), "Churn" + std::to_string(i));
        net.tick();
    }
    REQUIRE(net.session->fell_behind() == 1);
    REQUIRE(net.server.game.get_children(net.server.game.connections()).empty());
    (void)client;
}

TEST_CASE("RP47 bad data from the server ends the connection with a reason", "[replication][session]") {
    // A raw server endpoint that sends garbage once the client connects.
    network::LinkHub hub;
    network::EndpointConfig server_config;
    server_config.identity = identity();
    network::EndpointCallbacks callbacks;
    std::vector<network::PeerId> connected;
    callbacks.on_connected = [&](network::PeerId peer) { connected.push_back(peer); };
    network::Endpoint server(hub.open(kServer), server_config, callbacks);
    ScriptRig rig;
    engine_network::SessionConfig config;
    config.endpoint.identity = identity();
    engine_network::ClientSession client(rig.game, hub.open(client_address(0)), config);
    REQUIRE(client.connect(kServer, 0.0));
    double now = 0.0;
    bool sent = false;
    for (int i = 0; i < 60 && client.state() != engine_network::ClientState::Disconnected; ++i) {
        now += 1.0 / 60.0;
        server.receive(now);
        client.receive(now);
        if (!connected.empty() && !sent) {
            const std::uint8_t garbage[4] = {0xEE, 0x01, 0x02, 0x03};
            REQUIRE(server.send_message(connected[0], garbage, sizeof(garbage)));
            sent = true;
        }
        server.send(now);
        client.send(now);
    }
    REQUIRE(client.state() == engine_network::ClientState::Disconnected);
    REQUIRE(client.reason() == "Bad data from the server");
}
```

If the `Net` helper's `std::deque<Client>` move constraints bite (ScriptRig is not movable), keep `std::deque<std::unique_ptr<Client>>` instead and adjust; the behavior is what matters.

- [ ] **Step 2: Build to see the failure**

Expected: `Sessions.hpp` cannot be opened.

- [ ] **Step 3: Implement `ServerSession`**

`Sessions.hpp` declares the Interfaces with comments; `ServerSession` keeps:

```cpp
private:
    struct Peer {
        engine_core::InstanceId connection = 0;
        bool joined = false;
        std::deque<std::vector<std::uint8_t>> queue;
        std::size_t queued_bytes = 0;
    };
    void on_connected(network::PeerId peer);
    void on_disconnected(network::PeerId peer);
    void push(network::PeerId peer, const std::vector<std::uint8_t>& message);
    void flush(network::PeerId peer, double now);

    engine_core::DataModel& game_;
    SessionConfig config_;
    Replicator replicator_;
    std::unique_ptr<network::Endpoint> endpoint_;
    std::unordered_map<network::PeerId, Peer> peers_;
    std::size_t dropped_messages_ = 0;
    std::size_t fell_behind_ = 0;
```

`ServerSession.cpp`:

```cpp
#include "Sessions.hpp"

#include "ConnectionInstance.hpp"

namespace engine_network {

using engine_core::InstanceId;

ServerSession::ServerSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config)
    : game_(game), config_(std::move(config)), replicator_(game, config_.replicator) {
    config_.endpoint.role = network::Role::Server;
    network::EndpointCallbacks callbacks;
    callbacks.on_connected = [this](network::PeerId peer) { on_connected(peer); };
    callbacks.on_disconnected = [this](network::PeerId peer, network::DisconnectReason, network::DenyReason) {
        on_disconnected(peer);
    };
    callbacks.on_message = [this](network::PeerId, const std::uint8_t*, std::size_t) { ++dropped_messages_; };
    endpoint_ = std::make_unique<network::Endpoint>(std::move(socket), config_.endpoint, std::move(callbacks));
}

ServerSession::~ServerSession() = default;

network::Endpoint& ServerSession::endpoint() { return *endpoint_; }
Replicator& ServerSession::replicator() { return replicator_; }
std::size_t ServerSession::dropped_messages() const { return dropped_messages_; }
std::size_t ServerSession::fell_behind() const { return fell_behind_; }

InstanceId ServerSession::connection_of(network::PeerId peer) const {
    const auto found = peers_.find(peer);
    return found != peers_.end() ? found->second.connection : 0;
}

void ServerSession::on_connected(network::PeerId peer) {
    // Who is connected, by a UUID the server chose; names and the rest are the game's.
    auto& connection = game_.create<engine_core::ConnectionInstance>();
    connection.set_connection_id(engine_core::make_connection_uuid());
    game_.set_parent(connection.id(), game_.connections());
    Peer& entry = peers_[peer];
    entry = Peer{};
    entry.connection = connection.id();
}

void ServerSession::on_disconnected(network::PeerId peer) {
    const auto found = peers_.find(peer);
    if (found == peers_.end()) {
        return;
    }
    if (game_.alive(found->second.connection)) {
        game_.destroy_tree(found->second.connection);
    }
    peers_.erase(found);
}

void ServerSession::receive(double now) { endpoint_->receive(now); }

void ServerSession::push(network::PeerId peer, const std::vector<std::uint8_t>& message) {
    Peer& entry = peers_[peer];
    entry.queue.push_back(message);
    entry.queued_bytes += message.size();
}

void ServerSession::flush(network::PeerId peer, double now) {
    Peer& entry = peers_[peer];
    while (!entry.queue.empty() &&
           endpoint_->send_message(peer, entry.queue.front().data(), entry.queue.front().size())) {
        entry.queued_bytes -= entry.queue.front().size();
        entry.queue.pop_front();
    }
    // A client this far behind will not catch up: it starts again by reconnecting.
    if (entry.queue.size() > config_.max_queued_messages || entry.queued_bytes > config_.max_queued_bytes) {
        ++fell_behind_;
        endpoint_->disconnect(peer, now);
        on_disconnected(peer);
    }
}

void ServerSession::send(double now) {
    std::vector<std::uint8_t> changes;
    const bool any = replicator_.end_tick(changes);
    std::vector<network::PeerId> fresh;
    for (auto& [peer, entry] : peers_) {
        if (entry.joined) {
            if (any) push(peer, changes);
        } else {
            fresh.push_back(peer);
        }
    }
    // After this tick's changes: a snapshot taken now already holds them.
    for (const network::PeerId peer : fresh) {
        std::vector<std::vector<std::uint8_t>> snapshot;
        replicator_.snapshot_messages(peers_[peer].connection, snapshot);
        for (const auto& part : snapshot) push(peer, part);
        peers_[peer].joined = true;
    }
    std::vector<network::PeerId> ids;
    for (const auto& [peer, entry] : peers_) ids.push_back(peer);
    for (const network::PeerId peer : ids) {
        if (peers_.count(peer) != 0 && endpoint_->state(peer) == network::ConnectionState::Connected) flush(peer, now);
    }
    endpoint_->send(now);
}

void ServerSession::close(double now) {
    std::vector<network::PeerId> ids;
    for (const auto& [peer, entry] : peers_) ids.push_back(peer);
    for (const network::PeerId peer : ids) {
        endpoint_->disconnect(peer, now);
        on_disconnected(peer);
    }
    endpoint_->send(now);
}

}  // namespace engine_network
```

Check: `Endpoint::disconnect` from outside a callback fires `on_disconnected` synchronously or not (read Endpoint.cpp `disconnect`/`drop_peer`). If it does fire, `on_disconnected` above already erased the peer and the explicit call is a harmless no-op (`peers_.find` fails); if it does not, the explicit call is what removes the Connection. Either way the Connection is destroyed exactly once.

- [ ] **Step 4: Implement `ClientSession`**

`ClientSession` keeps `engine_core::DataModel& game_; SessionConfig config_; ReplicaApplier applier_; std::unique_ptr<network::Endpoint> endpoint_; ClientState state_ = ClientState::Idle; std::string reason_; double now_ = 0.0;`.

`ClientSession.cpp`:

```cpp
#include "Sessions.hpp"

namespace engine_network {

ClientSession::ClientSession(engine_core::DataModel& game, std::unique_ptr<network::ISocket> socket, SessionConfig config)
    : game_(game), config_(std::move(config)), applier_(game) {
    config_.endpoint.role = network::Role::Client;
    network::EndpointCallbacks callbacks;
    callbacks.on_connected = [this](network::PeerId) {
        state_ = ClientState::Joining;
        // A client plays the server's world: it starts while still empty, and
        // the snapshot builds the place during play.
        game_.set_world_role(engine_core::WorldRole::Client);
        if (!game_.simulation_running()) {
            game_.capture_place();
            game_.start_simulation();
        }
    };
    callbacks.on_message = [this](network::PeerId, const std::uint8_t* data, std::size_t size) {
        switch (applier_.receive_message(data, size)) {
            case ApplyResult::SnapshotApplied: state_ = ClientState::Joined; break;
            case ApplyResult::Bad:
                if (reason_.empty()) reason_ = "Bad data from the server";
                endpoint_->disconnect(0, now_);
                break;
            case ApplyResult::Ok: break;
        }
    };
    callbacks.on_disconnected = [this](network::PeerId, network::DisconnectReason why, network::DenyReason deny) {
        if (reason_.empty()) reason_ = describe(why, deny);
        state_ = ClientState::Disconnected;
        if (game_.simulation_running()) game_.stop_simulation();
    };
    endpoint_ = std::make_unique<network::Endpoint>(std::move(socket), config_.endpoint, std::move(callbacks));
}

ClientSession::~ClientSession() = default;

std::string ClientSession::describe(network::DisconnectReason why, network::DenyReason deny) const {
    using network::DenyReason;
    using network::DisconnectReason;
    switch (why) {
        case DisconnectReason::Denied:
            switch (deny) {
                case DenyReason::WrongGame: return "This server runs a different game";
                case DenyReason::WrongVersion:
                    return "This game needs an update (the server runs " + endpoint_->denied_version() + ")";
                case DenyReason::WrongContent: return "This game's files differ from the server's";
                case DenyReason::Full: return "The server is full";
                default: return "The server refused the connection";
            }
        case DisconnectReason::Timeout:
            return state_ == ClientState::Connecting ? "Could not reach the server" : "Lost connection to the server";
        case DisconnectReason::Requested: return "The server closed the connection";
        case DisconnectReason::Socket: return "Network error";
        default: return "Disconnected";
    }
}

bool ClientSession::connect(const network::Address& server, double now) {
    now_ = now;
    if (!endpoint_->connect(server, now)) {
        return false;
    }
    state_ = ClientState::Connecting;
    reason_.clear();
    return true;
}

void ClientSession::receive(double now) {
    now_ = now;
    endpoint_->receive(now);
}

void ClientSession::send(double now) {
    now_ = now;
    endpoint_->send(now);
}

ClientState ClientSession::state() const { return state_; }
const std::string& ClientSession::reason() const { return reason_; }
network::Endpoint& ClientSession::endpoint() { return *endpoint_; }
ReplicaApplier& ClientSession::applier() { return applier_; }

}  // namespace engine_network
```

Declare `std::string describe(network::DisconnectReason, network::DenyReason) const;` private. The player leaving reports `Requested` from the endpoint, whose wording ("The server closed the connection") would be wrong, so `disconnect` sets its own reason first:

```cpp
void ClientSession::disconnect(double now) {
    now_ = now;
    reason_ = "You left the game";
    endpoint_->disconnect(0, now);
}
```

Note the stop: `stop_simulation()` runs inside the endpoint's callback, on the engine thread under the lock, which is where the client already is.

- [ ] **Step 5: Build and run**

Run `"[replication]"`. Expected: RP40-RP47 pass and everything earlier still passes. RP45's six seconds and RP46's five are simulated time, not wall time.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/engine_network sandbox/replication_session_tests.cpp
git commit -m "Network: server and client sessions join clients, replicate the tree, and say why a connection ended

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Two engines end to end, benchmarks, and the full suite

**Files:**
- Create: `sandbox/replication_bench_tests.cpp`
- Modify: `sandbox/replication_engine_tests.cpp` (one more test), `CMakeLists.txt` (sandbox list)

**Interfaces:**
- Consumes: everything above; `Engine::set_network`, `Engine::run_frame`, `Engine::analysis()`.

- [ ] **Step 1: The end-to-end test**

Append to `sandbox/replication_engine_tests.cpp` (include `Sessions.hpp`, `Socket.hpp`, `GameIdentity.hpp`, `Connections.hpp`):

```cpp
TEST_CASE("RP48 a server engine and a client engine: the client sees the server's script move a part, and runs its own", "[replication][engine]") {
    SimRole role;
    network::LinkHub hub;
    network::GameIdentity id;
    network::parse_game_id("0123456789abcdef0123456789abcdef", id.game_id);
    id.resources_hash = 7;

    engine_core::Engine server;
    server.analysis().set_enabled(false);
    engine_core::DataModel& sg = server.datamodel();
    sg.set_world_role(engine_core::WorldRole::Server);
    engine_core::GameObject& part = sg.create<engine_core::GameObject>();
    sg.set_name(part.id(), "Part");
    sg.set_parent(part.id(), sg.scene_service("Workspace"));
    engine_core::Script& mover = add_script(sg, "Mover",
        "game:GetService('RunService').Heartbeat:Connect(function()\n"
        "    workspace.Part.Transform = workspace.Part.Transform + Vector3.new(1, 0, 0)\n"
        "end)");
    REQUIRE_FALSE(mover.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    engine_core::Script& reader = add_script(sg, "Reader",
        "_G.my_id = game:GetService('Connections').LocalConnection.Id");
    REQUIRE_FALSE(reader.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    engine_network::SessionConfig server_config;
    server_config.endpoint.identity = id;
    engine_network::ServerSession server_session(sg, hub.open(network::Address::loopback(1)), server_config);
    server.set_network(&server_session);
    sg.start_simulation();
    server.resume();

    engine_core::Engine client;
    client.analysis().set_enabled(false);
    engine_core::DataModel& cg = client.datamodel();
    engine_network::SessionConfig client_config;
    client_config.endpoint.identity = id;
    engine_network::ClientSession client_session(cg, hub.open(network::Address::loopback(2)), client_config);
    client.set_network(&client_session);
    client.resume();
    REQUIRE(client_session.connect(network::Address::loopback(1), 0.0));

    for (int i = 0; i < 120; ++i) {
        server.run_frame(1.0 / 60.0);
        client.run_frame(1.0 / 60.0);
    }
    server.set_network(nullptr);
    client.set_network(nullptr);
    REQUIRE(client_session.state() == engine_network::ClientState::Joined);
    const engine_core::InstanceId seen = cg.find_first_child(cg.scene_service("Workspace"), "Part");
    REQUIRE(seen != 0);
    const float server_x = part.transform().m[12];
    const float client_x = static_cast<engine_core::GameObject*>(cg.instance(seen))->transform().m[12];
    REQUIRE(server_x > 30.0f);
    REQUIRE(client_x > 20.0f);   // a few ticks behind the server, never ahead
    REQUIRE(client_x <= server_x);
    std::string my_id;
    REQUIRE(client.scripts().global_string("my_id", my_id));
    REQUIRE(my_id.size() == 36);
}
```

The engines' `now` comes from the steady clock (Task 1), so the transport sees real elapsed time while the loop runs as fast as it can; 120 frames of a few milliseconds each is enough wall time for the handshake (connect requests every 0.1 s) only if the loop is not too fast. If the client has not joined by the end, step the loop until `client_session.state() == Joined` with a 5-second wall-clock deadline instead of a fixed 120, then run 60 more frames. Use the real accessor for an engine's script runtime (`scripts()` in player/main.cpp).

- [ ] **Step 2: The benchmarks**

`sandbox/replication_bench_tests.cpp`:

```cpp
// Replication's speed: a 10,000-instance snapshot, and a busy tick sent to
// fifty clients (docs/superpowers/specs/2026-10-13-server-client-design.md,
// Performance targets). Timed warmed up; the limits hold in Release.

#include "replication_support.hpp"

#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "Light.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using engine_core::InstanceId;

namespace {

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void fill(engine_core::DataModel& game, int count, std::vector<InstanceId>& lights) {
    const InstanceId workspace = game.scene_service("Workspace");
    for (int i = 0; i < count; ++i) {
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), "Group" + std::to_string(i));
        game.set_parent(folder.id(), workspace);
        if (i % 10 == 0) {
            engine_core::PointLight& light = game.create<engine_core::PointLight>();
            game.set_parent(light.id(), folder.id());
            lights.push_back(light.id());
        }
    }
}

}  // namespace

TEST_CASE("RP60 a 10,000-instance snapshot encodes in under 50 ms and under 1 MB", "[replication][bench]") {
    ScriptRig server;
    std::vector<InstanceId> lights;
    fill(server.game, 9000, lights);
    replication::start_server(server);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> warm;
    replicator.snapshot_messages(0, warm);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    const double ms = milliseconds_since(start);
    std::size_t bytes = 0;
    for (const auto& m : messages) bytes += m.size();
    WARN("snapshot: " << ms << " ms, " << bytes << " bytes, " << messages.size() << " parts");
    ScriptRig client;
    replication::start_client(client);
    engine_network::ReplicaApplier applier(client.game);
    const auto apply_start = std::chrono::steady_clock::now();
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    WARN("snapshot apply: " << milliseconds_since(apply_start) << " ms");
#ifdef NDEBUG
    REQUIRE(ms < 50.0);
    REQUIRE(bytes < (1u << 20));
#endif
}

TEST_CASE("RP61 1,000 changes a tick, sent to 50 clients, cost the server under 1 ms; a client applies them in under 0.5 ms", "[replication][bench]") {
    network::LinkHub hub;
    network::GameIdentity id;
    network::parse_game_id("0123456789abcdef0123456789abcdef", id.game_id);
    id.resources_hash = 7;
    ScriptRig server;
    std::vector<InstanceId> lights;
    fill(server.game, 10000, lights);   // 1,000 lights
    replication::start_server(server);
    engine_network::SessionConfig config;
    config.endpoint.identity = id;
    config.endpoint.max_peers = 64;
    // Raw clients ack one packet a tick, so queues grow while this runs; none may be dropped.
    config.max_queued_messages = 100000;
    config.max_queued_bytes = std::size_t{1} << 31;
    engine_network::ServerSession session(server.game, hub.open(network::Address::loopback(1)), config);
    // Fifty raw client endpoints: they connect and receive, which is all the server sees of a client.
    std::vector<std::unique_ptr<network::Endpoint>> clients;
    for (int i = 0; i < 50; ++i) {
        network::EndpointConfig client_config;
        client_config.role = network::Role::Client;
        client_config.identity = id;
        clients.push_back(std::make_unique<network::Endpoint>(
            hub.open(network::Address::loopback(static_cast<std::uint16_t>(100 + i))), client_config, network::EndpointCallbacks{}));
        clients.back()->connect(network::Address::loopback(1), 0.0);
    }
    double now = 0.0;
    auto tick = [&](bool busy) {
        now += 1.0 / 60.0;
        session.receive(now);
        for (auto& c : clients) c->receive(now);
        if (busy) {
            for (std::size_t i = 0; i < lights.size(); ++i) {
                engine_core::Matrix4 moved = engine_core::matrix4_identity();
                moved.m[12] = static_cast<float>(now) + static_cast<float>(i);
                static_cast<engine_core::PointLight*>(server.game.instance(lights[i]))->set_transform(moved);
            }
        }
        const auto start = std::chrono::steady_clock::now();
        session.send(now);
        const double ms = milliseconds_since(start);
        for (auto& c : clients) c->send(now);
        return ms;
    };
    for (int i = 0; i < 120; ++i) tick(false);   // everyone joined and their snapshots drained
    for (int i = 0; i < 30; ++i) tick(true);     // warm up
    double total = 0.0;
    for (int i = 0; i < 60; ++i) total += tick(true);
    const double server_ms = total / 60.0;
    WARN("server send with 1,000 changes to 50 clients: " << server_ms << " ms a tick");

    // The client side: apply one busy tick's message.
    ScriptRig client_rig;
    replication::start_client(client_rig);
    engine_network::ReplicaApplier applier(client_rig.game);
    std::vector<std::vector<std::uint8_t>> snapshot;
    session.replicator().snapshot_messages(0, snapshot);
    REQUIRE(replication::deliver(applier, snapshot) == engine_network::ApplyResult::SnapshotApplied);
    for (std::size_t i = 0; i < lights.size(); ++i) {
        engine_core::Matrix4 moved = engine_core::matrix4_identity();
        moved.m[13] = static_cast<float>(i);
        static_cast<engine_core::PointLight*>(server.game.instance(lights[i]))->set_transform(moved);
    }
    std::vector<std::uint8_t> message;
    REQUIRE(session.replicator().end_tick(message));
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(applier.receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
    const double client_ms = milliseconds_since(start);
    WARN("client apply of 1,000 changes: " << client_ms << " ms");
#ifdef NDEBUG
    REQUIRE(server_ms < 1.0);
    REQUIRE(client_ms < 0.5);
#endif
}
```

A note on RP61: the 50 raw clients ack slowly (one packet a tick each), and a 1,000-change message is a multi-kilobyte block that the reliable channel sends one fragment a tick, so the per-peer queues grow during the timed ticks. That is the transport's known one-fragment-a-tick limit (parked in `docs/superpowers/networking-parked.md`); the benchmark measures server CPU time, which is what the target is, and raises the queue limits so no client is dropped while it runs.

- [ ] **Step 3: Build and run**

Run `"[replication]"` (all tags), then the full sandbox once: `./build/Release/sandbox.exe > build/2b-full.log 2>&1; tail -3 build/2b-full.log`. Report RP60/RP61's measured numbers. Known failures elsewhere (Q6; S38 and A34 under load): re-run a failing one alone and report it.

If RP60 or RP61 misses its limit, profile the hot path before changing tests: likely candidates are `save_properties` building a `PropertyBag` per instance (snapshot), the `std::set<std::pair<InstanceId,std::string>>` in `end_tick` (use an `unordered_set` keyed on id and property id), `read_property_json`'s linear field search (fine at 10-20 fields), and per-peer message copies in `ServerSession::push` (share one buffer through `std::shared_ptr<const std::vector<std::uint8_t>>` if copies dominate; the transport copies again on send, which is outside this plan).

- [ ] **Step 4: Commit**

```bash
git add sandbox/replication_engine_tests.cpp sandbox/replication_bench_tests.cpp CMakeLists.txt
git commit -m "Network: a server and client engine replicate end to end, and replication's speed is measured

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
