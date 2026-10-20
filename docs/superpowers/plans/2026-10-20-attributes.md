# Attributes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Instance can carry named attributes, set and read from Lua, shown and edited in Properties, saved with the place, undone, and restored at Stop, with signals that fire when one changes.

**Architecture:** The attribute map lives on `DataModel` and is exposed as one hidden saved JSON property, `Attributes`, so the existing property path gives save, load, copy, paste, `.aeinst`, Stop, and undo. Four methods and one event are registered on the `DataModel` Lua class beside `Changed`. The Properties sheet appends an Attributes group of rows read from the map, and the panel adds and removes them.

**Tech Stack:** C++17, Luau, Catch2 (sandbox and tests suites), JadeFX UI, CMake with MSVC.

**Spec:** `docs/superpowers/specs/2026-10-20-custom-shaders-design.md`, section "Attributes". Only "The feature" and "In the Properties panel" (the general Attributes group) are built here. The shader parameter rows, `ShaderParameters`, and the snapshot are the shader plan's.

## Global Constraints

- An attribute name is 1 to 100 characters of letters, digits, and underscores; anything else is refused with a message.
- Allowed values: number, boolean, string, Vector2, Vector3, Color3, Matrix4, Instance. Every other `LuaSlot::Kind` is refused with a message. `nil` removes.
- An Instance value is held by GUID, reads back as the live instance or `nil` when gone, and is remapped on copy like a reference property.
- Storage is one hidden saved property named `Attributes` on `DataModel`, a JSON object `{"<name>": {"type": "<registry type>", "value": <slot_to_json>}}`, default `null`.
- `SetAttribute` runs on the simulation thread, records one undo step, fires `AttributeChanged(name)`, `GetAttributeChangedSignal(name)`, and `Changed("Attributes")`.
- Setting an attribute to the value it already holds does nothing: no record, no signal.
- `Changed` keeps its meaning for properties; `Attributes` is the one property name it reports for attribute edits (my call, so the Properties panel refreshes through the path it already watches).
- No file in this plan may change the behavior of an existing registered property.

## Review Focus

- A script sets an attribute on `game` itself (id 0): `game:SetAttribute` must be refused with "game cannot hold attributes", not crash the root's bag. Pinned in Task 2.
- A save file hand-edited with `"Attributes": {"X": {"type": "Color3", "value": "red"}}`: the load must refuse that one entry with a message naming X and keep the rest, not fail the whole instance. Pinned in Task 3.
- An Instance attribute whose target was destroyed before the save: it must save as its GUID and load back as an attribute that reads `nil`, not vanish. Pinned in Task 3.
- Undo of a `SetAttribute` that replaced an Instance value with a number: redo and undo must restore the exact kind each way. Pinned in Task 2.
- A Properties Add with a name an attribute already has must be refused in the panel, not overwrite silently. Pinned in Task 6.

---

### Task 1: The attribute map and its JSON property

**Files:**
- Create: `src/engine_core/Attributes.hpp`, `src/engine_core/Attributes.cpp`
- Modify: `src/engine_core/DataModel.hpp` (members near `persistent_`), `src/engine_core/DataModel.cpp:2840-2870` (registration), `src/engine_core/PropertyReflection.hpp` (nothing new, reused)
- Modify: `CMakeLists.txt` (add `Attributes.cpp` to the engine_core source list beside `PropertyReflection.cpp` at line 720; add `sandbox/attribute_tests.cpp` to the `sandbox` executable at the `sandbox/archivable_tests.cpp` line, 1241)
- Test: `sandbox/attribute_tests.cpp`

**Interfaces:**
- Produces, in `Attributes.hpp`, namespace `engine_core`:
  - `using AttributeMap = std::map<std::string, LuaSlot>;` (ordered, so JSON and rows are stable by name)
  - `std::optional<std::string> attribute_name_error(std::string_view name);` — empty name, over 100 characters, or a character outside `[A-Za-z0-9_]` gives "an attribute name is letters, digits, and underscores, up to 100".
  - `std::optional<std::string> attribute_value_error(const LuaSlot& value);` — a kind outside Number, Bool, String, Vec2, Vec3, Color, Matrix4, Instance gives "an attribute holds a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance". Nil is allowed here (it means remove).
  - `const char* attribute_type_name(LuaSlot::Kind kind);` — the registry type name `slot_to_json` takes: "number", "boolean", "string", "Vector2", "Vector3", "Color3", "Matrix4", "Instance?".
  - `std::string attributes_to_json(const AttributeMap& map);` — compact JSON of the object above; `""` for an empty map. An Instance slot writes its `text` (the GUID).
  - `bool attributes_from_json(const std::string& text, AttributeMap& out, std::string& error);` — parses; `""` or `null` gives an empty map; an entry with a bad name, unknown type, or a value `slot_from_json` refuses is skipped and named in `error` ("attribute X: ..."), the rest loaded; returns false only when the text is not a JSON object.
- Produces on `DataModel`:
  - `const AttributeMap& attributes() const;`
  - `LuaSlot attribute(std::string_view name) const;` — Nil kind when absent; an Instance kind resolves its GUID through `instance_reference_slot(ref, "DataModel")` semantics: id set when alive, else kind Nil with text kept.
  - `std::optional<std::string> set_attribute(std::string_view name, LuaSlot value);` — the whole rule set of Task 2; this task only stores (no history, no signals) so Task 1's tests can read back.
  - The hidden saved property `Attributes` registered in `register_datamodel_lua`: `lua_hidden(lua_json_text(lua_saved_property("Attributes", "string", read_lua_attributes, write_lua_attributes, "null")))`. `write_lua_attributes` replaces the whole map from `attributes_from_json` (through `set_attributes_json`, a private DataModel member that stores without history or signals, as a load does) and puts the parse error in `in.error` only when the whole text was not an object; per-entry skips go to `std::fprintf(stderr, ...)` as other load warnings do.

- [ ] **Step 1: Write the failing tests**

```cpp
// sandbox/attribute_tests.cpp, tag [attributes]
TEST_CASE("ATR1 attribute names and kinds are checked", "[attributes]") {
    REQUIRE(!engine_core::attribute_name_error("Health"));
    REQUIRE(!engine_core::attribute_name_error("a_1"));
    REQUIRE(engine_core::attribute_name_error("") == "an attribute name is letters, digits, and underscores, up to 100");
    REQUIRE(engine_core::attribute_name_error("has space"));
    REQUIRE(engine_core::attribute_name_error(std::string(101, 'a')));
    LuaSlot number; number.kind = LuaSlot::Kind::Number; number.number = 3;
    REQUIRE(!engine_core::attribute_value_error(number));
    LuaSlot signal; signal.kind = LuaSlot::Kind::Signal;
    REQUIRE(engine_core::attribute_value_error(signal) ==
            "an attribute holds a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance");
}

TEST_CASE("ATR2 attributes round-trip through JSON", "[attributes]") {
    engine_core::AttributeMap map;
    // number 3, boolean true, string "x", Vector2 (1,2), Vector3 (1,2,3), Color3 (1,0,0), identity Matrix4,
    // Instance with text "0123456789abcdef0123456789abcdef"
    ...
    const std::string json = engine_core::attributes_to_json(map);
    engine_core::AttributeMap back; std::string error;
    REQUIRE(engine_core::attributes_from_json(json, back, error));
    REQUIRE(error.empty());
    REQUIRE(back.size() == 8);
    REQUIRE(back["Count"].kind == LuaSlot::Kind::Number); REQUIRE(back["Count"].number == 3);
    REQUIRE(back["Tint"].kind == LuaSlot::Kind::Color);
    REQUIRE(back["Target"].kind == LuaSlot::Kind::Instance); REQUIRE(back["Target"].text == "0123456789abcdef0123456789abcdef");
    REQUIRE(engine_core::attributes_to_json({}) == "");
    REQUIRE(engine_core::attributes_from_json("", back, error)); REQUIRE(back.empty());
    REQUIRE(engine_core::attributes_from_json("null", back, error)); REQUIRE(back.empty());
    REQUIRE(!engine_core::attributes_from_json("[1]", back, error));
}

TEST_CASE("ATR3 a bad entry is skipped and named, the rest load", "[attributes]") {
    engine_core::AttributeMap back; std::string error;
    REQUIRE(engine_core::attributes_from_json(
        R"({"X":{"type":"Color3","value":"red"},"Y":{"type":"number","value":2},"bad name":{"type":"number","value":1}})",
        back, error));
    REQUIRE(back.size() == 1);
    REQUIRE(back["Y"].number == 2);
    REQUIRE(error.find("attribute X") != std::string::npos);
}

TEST_CASE("ATR4 the Attributes property is hidden, saved, and reads the map", "[attributes]") {
    SimRole role; engine_core::Game game;
    const LuaField* field = engine_core::lua_class_find("DataModel", "Attributes");
    REQUIRE(field != nullptr); REQUIRE(field->hidden); REQUIRE(field->saved); REQUIRE(field->json_text);
    engine_core::GameObject& part = create_part(game);
    LuaSlot value; value.kind = LuaSlot::Kind::Number; value.number = 5;
    REQUIRE(!part.set_attribute("Health", value));
    LuaSlot out; REQUIRE(field->read(game, part, out));
    REQUIRE(out.text == R"({"Health":{"type":"number","value":5}})");
    REQUIRE(part.attribute("Health").number == 5);
    REQUIRE(part.attribute("Missing").kind == LuaSlot::Kind::Nil);
}
```

- [ ] **Step 2: Build and run to verify they fail**

Run: `cmake --build build --config Release --target sandbox` then `build/Release/sandbox.exe "[attributes]"`
Expected: compile failure on `Attributes.hpp` missing.

- [ ] **Step 3: Implement `Attributes.hpp/.cpp` and the DataModel members and registration**

`attributes_to_json` builds a `JsonValue::object()` and uses `slot_to_json(slot, attribute_type_name(kind), value)`; `attributes_from_json` uses `parse_json` then `slot_from_json(value, type, name, slot, error)` per entry, mapping the type string back to a kind by comparing against `attribute_type_name` of each allowed kind. `DataModel` stores `AttributeMap attributes_;` beside `persistent_`; `on_reuse` paths that reset `persistent_` clear it too (find them by grepping `persistent_ = false`).

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/Release/sandbox.exe "[attributes]"`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/Attributes.hpp src/engine_core/Attributes.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp CMakeLists.txt sandbox/attribute_tests.cpp
git commit -m "Attributes: every instance keeps a map of named values, saved as one hidden JSON property"
```

---

### Task 2: set_attribute's rules, undo, and signals

**Files:**
- Modify: `src/engine_core/DataModel.cpp` (`set_attribute`), `src/engine_core/DataModel.hpp`
- Modify: `src/engine_core/DataModel.cpp:2840-2870` (register the `AttributeChanged` event)
- Test: `sandbox/attribute_tests.cpp`

**Interfaces:**
- Consumes: Task 1's `set_attribute`, `attribute`, `attributes_to_json`.
- Produces: `std::optional<std::string> DataModel::set_attribute(std::string_view name, LuaSlot value)` with the full rules:
  - Contract-fails off the simulation thread, as `BloomEffect`'s setters do (`require_thread` pattern).
  - Refuses with "game cannot hold attributes" on the root (id 0).
  - Refuses with `attribute_name_error` / `attribute_value_error` messages.
  - An Instance value must be alive: `value.id` resolves through `instance(id)`, else "the instance is gone"; stores `value.text = guid(value.id)`.
  - Nil removes; removing an absent name is a no-op; setting an equal value (`same_slot`, Instance compared by text) is a no-op.
  - Records undo as one `note_property_change("Attributes", before, after)` where before and after are String slots of `attributes_to_json` before and after (this also fires `Changed("Attributes")` and marks dirty).
  - Fires `fire_event(id, "AttributeChanged", {name slot})` and the per-name signal (Task 4 adds the per-name part; here only the event).
- Produces the event registration: `static const LuaParam kAttributeArgs[] = {{"attribute", "string"}};` and `lua_event("AttributeChanged", kAttributeArgs, 1)` in the DataModel fields array.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ATR5 set_attribute refuses what the spec refuses", "[attributes]") {
    SimRole role; engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    LuaSlot number; number.kind = LuaSlot::Kind::Number; number.number = 1;
    REQUIRE(game.set_attribute("X", number) == "game cannot hold attributes");   // root: DataModel& game is id 0
    REQUIRE(part.set_attribute("bad name", number).has_value());
    LuaSlot signal; signal.kind = LuaSlot::Kind::Signal;
    REQUIRE(part.set_attribute("X", signal).has_value());
    LuaSlot gone; gone.kind = LuaSlot::Kind::Instance; gone.id = 99999;
    REQUIRE(part.set_attribute("X", gone) == "the instance is gone");
    REQUIRE(part.attributes().empty());
}

TEST_CASE("ATR6 an Instance attribute is held by GUID and reads nil once destroyed", "[attributes]") {
    SimRole role; engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    engine_core::GameObject& other = create_part(game);
    LuaSlot ref; ref.kind = LuaSlot::Kind::Instance; ref.id = other.id();
    REQUIRE(!part.set_attribute("Target", ref));
    REQUIRE(part.attribute("Target").id == other.id());
    REQUIRE(part.attributes().at("Target").text == game.guid(other.id()));
    game.destroy(other.id());
    REQUIRE(part.attribute("Target").kind == LuaSlot::Kind::Nil);
    REQUIRE(part.attributes().count("Target") == 1);   // still saved
}

TEST_CASE("ATR7 set, replace, and remove each undo as one step, kinds exact", "[attributes]") {
    SimRole role; engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    engine_core::GameObject& other = create_part(game);
    LuaSlot ref; ref.kind = LuaSlot::Kind::Instance; ref.id = other.id();
    LuaSlot number; number.kind = LuaSlot::Kind::Number; number.number = 7;
    begin_step(game); REQUIRE(!part.set_attribute("X", ref)); end_step(game);
    begin_step(game); REQUIRE(!part.set_attribute("X", number)); end_step(game);
    begin_step(game); REQUIRE(!part.set_attribute("X", LuaSlot{})); end_step(game);
    REQUIRE(part.attributes().empty());
    game.history().undo(); REQUIRE(part.attribute("X").number == 7);
    game.history().undo(); REQUIRE(part.attribute("X").kind == LuaSlot::Kind::Instance); REQUIRE(part.attribute("X").id == other.id());
    game.history().undo(); REQUIRE(part.attributes().empty());
    game.history().redo(); REQUIRE(part.attribute("X").kind == LuaSlot::Kind::Instance);
    game.history().redo(); REQUIRE(part.attribute("X").number == 7);
}

TEST_CASE("ATR8 an equal value records nothing and fires nothing", "[attributes]") {
    SimRole role; engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    LuaSlot number; number.kind = LuaSlot::Kind::Number; number.number = 7;
    int fired = 0;
    game.event_signal(part.id(), "AttributeChanged").connect([&](engine_core::InstanceId, engine_core::Field) { ++fired; });
    begin_step(game); REQUIRE(!part.set_attribute("X", number)); end_step(game);
    game.events().drain();
    REQUIRE(fired == 1);
    REQUIRE(game.history().can_undo().first);
    begin_step(game); REQUIRE(!part.set_attribute("X", number)); end_step(game);
    game.events().drain();
    REQUIRE(fired == 1);
    REQUIRE(!part.set_attribute("Absent", LuaSlot{}));
    game.events().drain();
    REQUIRE(fired == 1);
    // Only the first set was a step: one undo empties the map.
    game.history().undo();
    REQUIRE(part.attributes().empty());
    REQUIRE(!game.history().can_undo().first);
}

TEST_CASE("ATR9 AttributeChanged carries the name and Changed says Attributes", "[attributes]") {
    SimRole role; engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    std::string named; std::string changed;
    game.event_signal(part.id(), "AttributeChanged").connect([&](engine_core::InstanceId, engine_core::Field) {
        named = game.events().current_args()->at(0).text; });
    game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field) {
        changed = engine_core::lua_property_name(static_cast<std::uint32_t>(game.events().payload())); });
    LuaSlot flag; flag.kind = LuaSlot::Kind::Bool; flag.flag = true;
    REQUIRE(!part.set_attribute("Open", flag));
    game.events().drain();
    REQUIRE(named == "Open"); REQUIRE(changed == "Attributes");
}
```

Use the history helpers `begin_step` / `end_step` from `sandbox/support.hpp` and the `can_undo` name `ChangeHistoryService` actually exposes (check `ChangeHistoryService.hpp`; `history_tests.cpp` shows the calls).

- [ ] **Step 2: Run to verify they fail**

Run: `build/Release/sandbox.exe "[attributes]"`
Expected: ATR5 to ATR9 fail (no refusals, no history, no event).

- [ ] **Step 3: Implement the rules in `DataModel::set_attribute` and register the event**

Order inside: thread check, root check, name check, value check, Instance resolution, equality short-circuit, store, `note_property_change("Attributes", before, after)`, `fire_event(id_, "AttributeChanged", {name})`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/Release/sandbox.exe "[attributes]"`
Expected: 9 passed. Also run `build/Release/sandbox.exe "[history]"` to confirm nothing else moved.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp sandbox/attribute_tests.cpp
git commit -m "Attributes: SetAttribute checks its name and value, records one undo step, and fires AttributeChanged"
```

---

### Task 3: Save, load, Stop, clone, paste, and reference remapping

**Files:**
- Modify: `src/engine_core/InstanceFile.cpp:212-257` (`remap_references`)
- Test: `sandbox/attribute_tests.cpp`

**Interfaces:**
- Consumes: Task 1's property and Task 2's setter.
- Produces: `remap_references` also walks each copied instance's attributes; an Instance attribute whose GUID is in the `CopyMap` is pointed at the copy (through `set_attribute` with history off, as the field writes there run), and one pointing outside the copied set is left as it is (a reference property keeps its outside target too).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ATR10 attributes save with the place and load back, a dead target as its GUID", "[attributes]") {
    // As BLM1 saves and reloads a project (TempDir, Project save/load from bloom_tests.cpp), with
    // a part holding Health 5, Tint Color3, Target -> another part that is destroyed before the save.
    // After load: Health 5, Tint equal, Target present with the saved GUID and attribute("Target").kind Nil.
}

TEST_CASE("ATR11 a hand-edited bad entry loads the rest", "[attributes]") {
    // Write the part's JSON file with "Attributes": {"X":{"type":"Color3","value":"red"},"Y":{"type":"number","value":2}}
    // Load: Y is 2, X absent, the load succeeds.
}

TEST_CASE("ATR12 Stop puts attributes back", "[attributes]") {
    // As BLM1's Stop section: set Health 5 while stopped, Start, set Health 9 and add Extra, Stop:
    // Health 5, Extra absent.
}

TEST_CASE("ATR13 Clone and paste copy attributes and remap an Instance inside the set", "[attributes]") {
    // A Folder with parts A and B, A.Target = B, A.Outside = a part outside the folder.
    // Clone the folder (as clone_tests.cpp does): A'.Target == B', A'.Outside == the outside part.
    // Write the folder to an .aeinst and read it back (instance_file_tests.cpp): the same holds.
}
```

- [ ] **Step 2: Run to verify ATR13 fails**

Run: `build/Release/sandbox.exe "[attributes]"`
Expected: ATR10 to ATR12 pass already (the property path does them); ATR13 fails on the remap.

- [ ] **Step 3: Extend `remap_references`**

After the saved-field loop: for each `[name, slot]` of `object->attributes()` with `slot.kind == Instance` and `map.count(slot.text)`, build a `LuaSlot next{kind Instance, id = found->second}` and call `object->set_attribute(name, next)`. History is already off in the callers of `remap_references` (check `InstanceFile.cpp`'s paste path uses `HistoryOff` or the equivalent; if not, wrap the loop the way `finish_live_edit` does).

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/Release/sandbox.exe "[attributes]"` and `build/Release/sandbox.exe "[copy]"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/InstanceFile.cpp sandbox/attribute_tests.cpp
git commit -m "Attributes: a pasted or cloned Instance attribute points at the copy, as a reference property does"
```

---

### Task 4: The Lua methods and the per-name signal

**Files:**
- Modify: `src/engine_core/ScriptBindings.hpp` (declarations; a new `kSignalAttribute = 7` kind), `src/engine_core/ScriptBindings.cpp:1705-1720` (registration), the `signal_of` / `signal_cause` switches at `:840-900`
- Modify: `src/engine_core/DataModel.hpp/.cpp` (`attribute_signal`)
- Modify: `src/engine_core/LuaApi.cpp:1014` (docs beside `Changed`)
- Test: `sandbox/attribute_tests.cpp` (Lua through `ScriptRig`)

**Interfaces:**
- Consumes: Task 2's `set_attribute`, `attribute`, `attributes()`; `push_registered(state, runtime, slot, id, world)` to push a slot; the slot-from-Lua conversion in `instance_newindex` (`:560-640`), which this task lifts into a shared `bool ScriptBindings::slot_from_stack(lua_State*, int index, LuaSlot& out, std::string& error)` that reads a Lua value of any attribute-capable kind (nil, boolean, number, string, Vector3 via `lua_tovector`, Vector2 and Color3 and Matrix4 and Instance as the newindex branches read them) without a type name to guide it. `instance_newindex` is not rewritten to use it in this plan.
- Produces:
  - `int ScriptBindings::instance_set_attribute(lua_State*)`, `instance_get_attribute`, `instance_get_attributes`, `instance_get_attribute_changed_signal`, registered as `lua_method("SetAttribute", "nil", ...)`, `lua_method("GetAttribute", "", ...)` (return type unknown, as `GetService` registers), `lua_method("GetAttributes", "", ...)`, `lua_method("GetAttributeChangedSignal", "Signal", ...)` in the `DataModel` methods array.
  - `Signal& DataModel::attribute_signal(InstanceId id, std::string_view name)` — one `Signal` per (instance, name), made on first ask in a map on the instance bag beside the event signals (`event_signal` is the model); `set_attribute` emits it after the event, with no args.
  - `SignalUd` kind `kSignalAttribute`: `id` the instance, `event_name` the attribute name, which must outlive the userdata: intern it through `lua_property_name(lua_property_id(name))`, which keeps names for the process.
  - `signal_cause` returns "AttributeChanged" for both the event and the per-name kind.
  - Docs: `add("DataModel", "SetAttribute", "Sets the attribute, or removes it with nil. A name is letters, digits, and underscores; a value is a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance.", nullptr, true, {P("name", "string"), P("value", "any")})`, and the three others, plus `add("DataModel", "AttributeChanged", "Fires with the name of the attribute that was set or removed.", "Signal", false, {})`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ATR14 scripts set, get, list, and remove attributes", "[attributes]") {
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Part");
    rig.runtime.run_chunk(R"(
        local p = game.Workspace.Part
        p:SetAttribute("Health", 5)
        p:SetAttribute("Tint", Color3.new(1, 0, 0))
        p:SetAttribute("Where", Vector3.new(1, 2, 3))
        p:SetAttribute("Target", game.Workspace)
        print(p:GetAttribute("Health"), p:GetAttribute("Tint").r, p:GetAttribute("Where").z, p:GetAttribute("Target").Name)
        local n = 0 for k, v in pairs(p:GetAttributes()) do n += 1 end print(n)
        p:SetAttribute("Health", nil)
        print(p:GetAttribute("Health"))
        print(pcall(function() p:SetAttribute("bad name", 1) end))
        print(pcall(function() p:SetAttribute("X", p.Changed) end))
        print(pcall(function() game:SetAttribute("X", 1) end))
    )");
    const auto out = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(out, "5\t1\t3\tWorkspace\n"));
    REQUIRE(has_line(out, "4\n"));
    REQUIRE(has_line(out, "nil\n"));
    // the three pcalls print false and a message that contains the refusal text
}

TEST_CASE("ATR15 AttributeChanged and GetAttributeChangedSignal fire for the right name", "[attributes]") {
    ScriptRig rig;
    rig.game.set_name(create_part(rig.game).id(), "Part");
    rig.runtime.run_chunk(R"(
        local p = game.Workspace.Part
        p.AttributeChanged:Connect(function(name) print("any", name) end)
        p:GetAttributeChangedSignal("A"):Connect(function() print("A changed") end)
        p:GetAttributeChangedSignal("B"):Connect(function() print("B changed") end)
        p:SetAttribute("A", 1)
        p:SetAttribute("A", 1)
        p:SetAttribute("B", nil)
        p:SetAttribute("B", true)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    // exactly: "any\tA", "A changed", "any\tB", "B changed", in that order, and nothing for the equal set or the absent remove
}
```

`has_line` is the helper `archivable_tests.cpp` defines; copy it into this file's anonymous namespace.

- [ ] **Step 2: Run to verify they fail**

Run: `build/Release/sandbox.exe "[attributes]"`
Expected: ATR14 and ATR15 fail with "SetAttribute is not a valid member".

- [ ] **Step 3: Implement the four bindings, `slot_from_stack`, `attribute_signal`, the SignalUd kind, and the docs**

`instance_get_attributes` builds a table with `lua_newtable` and `push_registered` per entry. `instance_get_attribute_changed_signal` checks the name with `attribute_name_error` and raises the message. Errors from `set_attribute` raise through `luaL_error` with the message, as `instance_newindex` raises a refused write.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/Release/sandbox.exe "[attributes]"` and `build/Release/sandbox.exe "[analysis]"` (the class declarations now include the methods; the analysis tests must still pass).
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ScriptBindings.hpp src/engine_core/ScriptBindings.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/LuaApi.cpp sandbox/attribute_tests.cpp
git commit -m "Attributes: SetAttribute, GetAttribute, GetAttributes, GetAttributeChangedSignal, and AttributeChanged from Lua"
```

---

### Task 5: Attribute rows in the property sheet, and MCP

**Files:**
- Modify: `src/ide/PropertySheet.hpp` (`PropertyRow::attribute`, `PropertyEdit::attribute`), `src/ide/PropertySheet.cpp:412-530` (`read_sheet`), `:560-680` (`write_edit`)
- Modify: `src/ide/McpTools.cpp:352-372` (`Properties` already reads the sheet; add an `attributes` object)
- Test: `tests/PropertiesTest.cpp`, `tests/McpTest.cpp`

**Interfaces:**
- Consumes: Task 2's `attributes()`, `attribute()`, `set_attribute`.
- Produces:
  - `bool PropertyRow::attribute = false;` and `std::string group = "Attributes"` for such rows; `name` is the attribute name, `type_name` is `attribute_type_name(kind)`, `kind` from `property_kind_for(type_name)` (Instance? gives Ref). `same_slot` compares `attribute` too.
  - `read_sheet` appends, after the class rows and before the ordering step, one row per attribute name that every selected instance holds with the same kind, values merged as class rows are (`merge`, `blank_mixed`), writable true. The Attributes group sorts last, after Data.
  - `bool PropertyEdit::attribute = false;` `write_edit` with it set skips the field lookup and calls `set_attribute(edit.property, slot)` on each target that holds the attribute with that kind, converting `edit.value` to a `LuaSlot` the way the field path's `slot_from_value` does (find the existing conversion in `write_edit` and reuse it). A refusal's message becomes `result.error`.
  - MCP `Properties(world, id)` adds `"attributes": { name: {"type", "value"} }` from the sheet's attribute rows, and leaves them out of `"properties"`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/PropertiesTest.cpp
TEST_CASE("attribute rows follow the class rows under Attributes") {
    // a part with Health 5 and Tint Color3: read_sheet(world, {id}, false) has rows named Health (Number, attribute,
    // group "Attributes", value 5) and Tint (Color3), both after every class row.
    // Two parts, one with Health 5 and one with Health "x": no Health row. Both with Health 5 and 6: a mixed row.
}
TEST_CASE("an attribute edit writes through set_attribute and refuses what it refuses") {
    // PropertyEdit{property "Health", kind Number, value 9, attribute true} on the part: attribute("Health") is 9,
    // one undo step. The same edit on a part without Health: result has no error and nothing changes.
}
// tests/McpTest.cpp
TEST_CASE("inspect lists attributes apart from properties") {
    // the inspect result for the part has "attributes": {"Health": {"type": "number", "value": 5}} and no "Health" in "properties"
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --config Release --target properties-tests mcp-tests` then `build/Release/properties-tests.exe "attribute*"` and `build/Release/mcp-tests.exe "inspect lists attributes*"`
Expected: FAIL (no rows, no `attributes` key).

- [ ] **Step 3: Implement the sheet and MCP changes**

- [ ] **Step 4: Run the suites to verify they pass**

Run: `build/Release/properties-tests.exe` and `build/Release/mcp-tests.exe`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/ide/PropertySheet.hpp src/ide/PropertySheet.cpp src/ide/McpTools.cpp tests/PropertiesTest.cpp tests/McpTest.cpp
git commit -m "Properties: an instance's attributes are rows under Attributes, and inspect lists them"
```

---

### Task 6: Add and Remove in the Properties panel

**Files:**
- Modify: `src/ide/PropertiesPanel.cpp` (the `GroupHeader` at `:597-610` and `header()` at `:1461`; the row views near `:1150-1210`; `rebuild()` at `:2683`)
- Modify: `src/ide/PropertiesPanel.hpp` (nothing public; `Impl` only)
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: Task 5's rows and edits.
- Produces, on `PropertiesPanel::Impl`:
  - The Attributes group header always exists when the selection is one or more instances (not game), even with no attributes, and carries a `+` button at its right, as the Explorer's hover `+` does (`IdeExplorer`'s is the model; `InsertPopup` the popup style). Clicking opens a small popup with a name field and a type list (number, boolean, string, Vector2, Vector3, Color3, Instance; Matrix4 is left out of the picker since no row editor makes one) and Add. Add calls `set_attribute(name, default of that kind)` on every selected instance through the panel's `PropertiesRun`; the zero value of the kind, `""`, `false`, white, or `nil` for Instance (which is refused by `set_attribute` as a removal, so an Instance attribute starts as a Ref row only once a target is picked: Add for Instance opens the asset picker directly instead and refuses nil).
  - A name an attribute already has on any selected instance is refused in the popup with "X is already an attribute" before anything is written.
  - Each attribute row has a small `×` at its right (after the field) that removes it: `set_attribute(name, nil)` on every selected instance, as one undo step through the panel's `begin`/`end` recording the way a field edit is recorded.
  - Folding of the Attributes header persists by title like the others.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/PropertiesTest.cpp, in the panel tests that drive PropertiesPanel with a bound world
TEST_CASE("the Attributes header shows with a + for an instance selection, and not for game") {}
TEST_CASE("Add creates the attribute at the kind's zero value on every selected instance, as one undo step") {}
TEST_CASE("Add refuses a name any selected instance already has") {}
TEST_CASE("the row's remove button removes the attribute on every selected instance, as one undo step") {}
```

Drive the popup through the panel's test hooks the way the existing curve-editor and choices tests do (find `set_choice_editor` and the InsertPopup tests for the pattern).

- [ ] **Step 2: Run to verify they fail**

Run: `build/Release/properties-tests.exe "*Attributes*"`
Expected: FAIL.

- [ ] **Step 3: Implement the header button, the popup, and the row remove button**

- [ ] **Step 4: Run the suite and look at it in the studio**

Run: `build/Release/properties-tests.exe`, then `cmake --build build --config Release --target AnarchyStudio`, open the studio, select a part, add a number attribute, change it, undo, remove it. Take one screenshot of the panel with two attributes for the commit's record (the MCP screenshot tool, per the drive-studio memory).
Expected: all pass; the panel shows and edits attributes.

- [ ] **Step 5: Commit**

```bash
git add src/ide/PropertiesPanel.cpp src/ide/PropertiesPanel.hpp tests/PropertiesTest.cpp
git commit -m "Properties: an Attributes group with + to add one and × on each row to remove it"
```

---

### Task 7: Docs and completion

**Files:**
- Modify: `src/engine_core/README.md` (the paragraph on registry properties), `src/engine_instances/README.md` (one paragraph under the Instance basics), `README.md` (the scripting section, if it lists Instance members)
- Test: `tests/LuauCompleteTest.cpp`

**Interfaces:**
- Consumes: Task 4's docs.
- Produces: completion on an instance variable lists `SetAttribute`, `GetAttribute`, `GetAttributes`, `GetAttributeChangedSignal`, and `AttributeChanged` with their summaries.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("completion lists the attribute members on any instance") {
    // completing `game.Workspace.` includes SetAttribute with the doc summary from Task 4
}
```

- [ ] **Step 2: Run to verify it passes already or fails**

Run: `build/Release/studio-tests.exe "completion lists the attribute members*"` (`tests/LuauCompleteTest.cpp` is in `studio-tests`, CMakeLists line 1013).
Expected: PASS if Task 4's `add(...)` lines feed completion, else fix the docs.

- [ ] **Step 3: Write the README paragraphs**

One paragraph each: what attributes are, the four methods and two signals, the allowed kinds, that they save as the hidden `Attributes` property and show under Properties' Attributes group, and that `Changed` reports "Attributes".

- [ ] **Step 4: Commit**

```bash
git add src/engine_core/README.md src/engine_instances/README.md README.md tests/LuauCompleteTest.cpp
git commit -m "Docs: attributes on every instance"
```

---

## Self-review notes

- Spec coverage: the four methods and two signals (Tasks 2, 4), value kinds and Instance-by-GUID (1, 2, 3), the hidden JSON property with save, load, copy, paste, `.aeinst`, Stop, undo (1, 2, 3), one undo step per set (2), the Attributes group with Add and Remove (5, 6), MCP (5), completion and analysis needing nothing new beyond the docs (4, 7). The shader-parameter rows are explicitly the shader plan's.
- Names used across tasks: `set_attribute`, `attribute`, `attributes`, `attribute_signal`, `attributes_to_json`, `attributes_from_json`, `attribute_type_name`, `attribute_name_error`, `attribute_value_error`, `PropertyRow::attribute`, `PropertyEdit::attribute`, `kSignalAttribute`.
- Review Focus lines are pinned: root refusal (ATR5), bad saved entry (ATR3, ATR11), dead target saved by GUID (ATR6, ATR10), undo kinds (ATR7), duplicate Add (Task 6's third test).
