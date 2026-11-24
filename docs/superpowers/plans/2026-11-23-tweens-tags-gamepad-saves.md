# Tweens, Tags, Gamepads, and Save Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `TweenService`, instance tags with `CollectionService`, gamepad input on `UserInputService`, and `SaveService` to the engine's Luau API.

**Architecture:** Each feature is a pure C++ core in `engine_core` (no Lua in it, unit-tested directly) plus one `*Bindings.cpp` file that is its Lua face, following `PhysicsServiceBindings.cpp` (a service) and `AnimationBindings.cpp` (a userdata with its own signals). Services are added to `kServiceClasses` in `ScriptBindings.hpp`; their state hangs off the `DataModel` like `collision_groups()` and `input()`. Signals a service owns are host signals (`EventQueue::host_signal`), as `Animator::track_signal` does.

**Tech Stack:** C++20, Luau, Catch2 (`sandbox/`), GLFW through JadeFX for gamepads, the engine's own `JsonValue` (`PropertyBag.hpp`).

**Spec:** `docs/superpowers/specs/2026-11-23-tweens-tags-gamepad-saves-design.md`

## Global Constraints

- Every Lua binding runs under `lua_guard` and raises refusals with `luaL_error`; none aborts.
- Every new member is registered with `register_lua_class` (so completion and strict analysis see it) and documented with `add(...)` in `LuaApi.cpp`.
- New sources are listed by hand in `CMakeLists.txt` beside their neighbours (`Attributes.cpp` at the engine_core list, test files at `add_executable(sandbox`).
- Enum values for gamepad items are Roblox's: `UserInputType.Gamepad1..4` = 12..15; `KeyCode.ButtonX` = 1000, `ButtonY` 1001, `ButtonA` 1002, `ButtonB` 1003, `ButtonR1` 1004, `ButtonL1` 1005, `ButtonR2` 1006, `ButtonL2` 1007, `ButtonR3` 1008, `ButtonL3` 1009, `ButtonStart` 1010, `ButtonSelect` 1011, `DPadLeft` 1012, `DPadRight` 1013, `DPadUp` 1014, `DPadDown` 1015, `Thumbstick1` 1016, `Thumbstick2` 1017.
- Tween defaults: `Time` 1, `Style` Quad, `Direction` Out, `Delay` 0, `Repeat` 0, `Reverses` false. Stick dead zone 0.15. Tag and attribute name limit 100. Store name 1 to 50 of letters, digits, `_`, `-`.
- Nothing is committed unless the user asks.

## Review Focus

1. A tween's instance is destroyed mid-tween: the tween ends as cancelled (`Completed(false)`), no write to a dead instance, no error. (Task 1 test TW7)
2. Stop during a tween: properties return to authored values and no tween survives into the next Test. (Task 1 test TW8)
3. A tagged instance is destroyed, or a place is stopped: `GetTagged` no longer lists it and `Observe` cleanups run exactly once. (Task 2 tests TAG6, TAG8)
4. A gamepad is unplugged with buttons held: every held button ends (`InputEnded`) and polls read released/zero. (Task 3 test GP5)
5. A save file is corrupt, or the folder is not writable: `Open` gives an empty store, the console says why, the bad file is left alone until a `Set`, and a failed write does not raise in the script. (Task 4 tests SV6, SV7)

---

### Task 1: TweenService

**Files:**
- Create: `src/engine_core/Easing.hpp`, `src/engine_core/Easing.cpp` (pure math)
- Create: `src/engine_core/TweenWorld.hpp`, `src/engine_core/TweenWorld.cpp` (tween state and stepping, no Lua)
- Create: `src/engine_core/TweenBindings.cpp` (the service, the `Tween` userdata, `Enum.EasingStyle`/`EasingDirection` use)
- Modify: `src/engine_datatypes/Enum.cpp`/`.hpp` (two enums), `src/engine_core/ScriptBindings.hpp` (`kServiceClasses`, `kSignalTween`), `ScriptBindings.cpp` (`signal_of`, link), `ScriptRuntime.cpp` (`open_tween`, step from `heartbeat`, drop at `close_vm`), `DataModel.hpp/.cpp` (`tweens()`), `LuaApi.cpp` (docs), `CMakeLists.txt`
- Test: `sandbox/tween_tests.cpp`

**Interfaces (produces):**
- `double ease(EasingStyle style, EasingDirection direction, double t)`; `t` and the result at 0 and 1 are exactly 0 and 1.
- `struct TweenOptions { double time=1, delay=0; int repeat=0; bool reverses=false; EasingStyle style=Quad; EasingDirection direction=Out; }`
- `class TweenWorld`: `std::uint32_t start(DataModel& world, InstanceId target, std::vector<TweenGoal> goals, const TweenOptions&)` (a goal is `{const LuaField* field; LuaSlot from, to;}`), `std::uint32_t start_value(LuaSlot from, LuaSlot to, const TweenOptions&)`, `void pause(id)`, `void resume(id)`, `void cancel(id)`, `bool playing(id) const`, `bool alive(id) const`, `Signal& completed(id)`, `Signal& stepped(id)` (value tweens), `void step(DataModel& world, double dt)`, `void clear()`.
- `bool tween_lerp(const LuaSlot& from, const LuaSlot& to, double alpha, LuaSlot& out)`; false for a kind that cannot tween.

- [ ] **Step 1:** Write `sandbox/tween_tests.cpp`: TW1 `ease` endpoints and Linear midpoint 0.5, Quad Out at 0.5 is 0.75, Quad In 0.25, every style/direction finite and 0→0, 1→1; TW2 `tween_lerp` for Number, Vec2, Vec3, Color, Matrix4 (position halfway, rotation halfway by slerp), refuses String; TW3 Lua: `TweenService:Tween(part, {Transform = ...}, {Time = 1, Style = "Linear"})` reaches the midpoint after 30 frames of 1/60 and the goal after 60, then `Completed` fires `true` once; TW4 options (`Delay`, `Repeat = 1`, `Reverses = true` returns to start), unknown option key and unknown style raise; TW5 a second tween on the same property replaces the first: the first's `Completed` fires `false`, a tween on another property keeps running; TW6 `Pause`/`Resume`/`Cancel`/`Playing`/`Wait()` returning the boolean; TW7 destroyed instance; TW8 Stop (`close_vm`) drops tweens; TW9 `TweenService:Value(0, 10, {Time = 1, Style = "Linear"}, fn)` calls `fn` each frame ending on exactly 10; TW10 a non-tweenable property (`Name`) and a read-only one raise naming the property; TW11 strict analysis accepts the members.
- [ ] **Step 2:** Add the test file to CMake, build `sandbox`, run `./build/sandbox "[tween]"`: fails to compile (no `Easing.hpp`).
- [ ] **Step 3:** Implement `Easing`, the enums, `TweenWorld`, bindings, runtime hooks. Step order: tweens step inside `ScriptRuntime::heartbeat` before the Heartbeat signal's handlers resume. Writes use `field->write(world, object, slot)`, the path `instance_newindex` uses for registry properties, and the core `Transform`/velocity setters for `Field` properties.
- [ ] **Step 4:** Run `./build/sandbox "[tween]"`: all pass.

### Task 2: Tags and CollectionService

**Files:**
- Create: `src/engine_core/Tags.hpp`, `src/engine_core/Tags.cpp` (`tag_error`, `tags_to_json`, `tags_from_json`)
- Create: `src/engine_core/CollectionServiceBindings.cpp`
- Modify: `DataModel.hpp/.cpp` (`tags()`, `add_tag`, `remove_tag`, `has_tag`, `set_tags`, hidden saved `Tags` property, `tagged(tag)` scan, `tag_signal(tag, added)`, firing removal on destroy), `ScriptBindings.cpp/.hpp` (instance methods, service kind), `LuaApi.cpp`, `CMakeLists.txt`
- Test: `sandbox/tag_tests.cpp`

**Interfaces (produces):**
- `std::optional<std::string> tag_error(std::string_view)`; `using TagSet = std::set<std::string, std::less<>>`.
- `DataModel`: `const TagSet& tags() const`, `std::optional<std::string> add_tag(std::string_view)`, `bool remove_tag(std::string_view)`, `bool has_tag(std::string_view) const`, `void set_tags(TagSet)`, `std::vector<InstanceId> tagged(std::string_view) const`, `Signal& tag_signal(std::string_view tag, bool added)`.

- [ ] **Step 1:** Write `sandbox/tag_tests.cpp`: TAG1 name rules (empty, 101 chars, control char refused); TAG2 JSON round trip, sorted; TAG3 add/remove undo as one step each and `Changed` says `Tags`; TAG4 saved with the place and loaded; TAG5 Clone copies tags; TAG6 Stop restores tags; destroy removes from `GetTagged`; TAG7 Lua `AddTag`/`RemoveTag`/`HasTag`/`GetTags`/`GetTagged` and the two signals fire with the instance; TAG8 `Observe` runs for existing and later instances, cleanup runs once on `RemoveTag`, on `Destroy`, and on `Disconnect`; TAG9 strict analysis accepts the members.
- [ ] **Step 2:** Build and run `./build/sandbox "[tags]"`: fails to compile.
- [ ] **Step 3:** Implement, mirroring `set_attribute`/`attributes_moved`/`read_lua_attributes`. `Observe` is implemented in C++ over the two tag signals with Lua references held per connection.
- [ ] **Step 4:** Run `./build/sandbox "[tags]"`: all pass.

### Task 3: Gamepad input

**Files:**
- Create: `src/engine_core/GamepadState.hpp` (`struct GamepadState { bool connected; bool buttons[16]; float axes[6]; }`, `Vec3 stick_with_dead_zone(float x, float y, float dead = 0.15f)`)
- Modify: `src/engine_services/UserInputService.hpp/.cpp` (`post_gamepad(int pad, const GamepadState&)`, `gamepad_connected(pad)`, `gamepad_button_down(pad, key)`, `gamepad_axis(pad, key)`, `Kind::GamepadConnected`/`GamepadDisconnected`), `src/engine_datatypes/Enum.cpp` (items above), `ScriptBindings.cpp` (four methods, two signals), `src/runner/GameView.cpp` (poll `glfwGetGamepadState` for `GLFW_JOYSTICK_1..4` once a paint while the view has the keyboard, post each), `LuaApi.cpp`
- Test: `sandbox/gamepad_tests.cpp`

- [ ] **Step 1:** Write tests: GP1 dead zone (inside is 0, full deflection is length 1, rescaled between, Y up positive); GP2 posting a state with A down fires `InputBegan` with `UserInputType.Gamepad1` and `KeyCode.ButtonA`, releasing fires `InputEnded`, an unchanged state fires nothing; GP3 a stick move fires `InputChanged` with `Thumbstick1` and the value in `Position`; `GetGamepadAxis` returns it, triggers as `(value, 0)`; GP4 `GamepadConnected`/`GamepadDisconnected` and `GetConnectedGamepads`; GP5 disconnect with A held ends A; GP6 strict analysis accepts the members; `IsGamepadButtonDown` with a non-gamepad type raises.
- [ ] **Step 2:** Build and run `./build/sandbox "[gamepad]"`: fails to compile.
- [ ] **Step 3:** Implement. The service diffs each posted state against the last posted one under its own lock and queues records, as keys do.
- [ ] **Step 4:** Run `./build/sandbox "[gamepad]"`: all pass.

### Task 4: SaveService

**Files:**
- Create: `src/engine_core/SaveStore.hpp`, `src/engine_core/SaveStore.cpp` (`SaveStores`: directory, stores by name, values as `JsonValue`, dirty time, atomic write), `src/engine_core/UserData.hpp/.cpp` (`std::filesystem::path user_data_directory()`: `ANARCHY_USER_DATA` when set, else the same per-platform folder `ide::config_directory` returns), `src/engine_core/SaveServiceBindings.cpp`
- Modify: `DataModel.hpp/.cpp` (`saves()`), `Project.cpp` (after `game_id_` is known at load and first save: `game.saves().set_directory(user_data_directory() / "saves" / game_id_)`), `Engine.cpp` (`saves().flush_due(now)` once a frame; `flush()` at Stop and shutdown), `ScriptBindings`, `LuaApi.cpp`, `CMakeLists.txt`
- Test: `sandbox/save_tests.cpp`

**Interfaces (produces):**
- `std::optional<std::string> store_name_error(std::string_view)`.
- `class SaveStores`: `void set_directory(std::filesystem::path)`, `SaveStore* open(std::string_view name, std::string& error)`, `void flush_due(Clock::time_point now)`, `void flush()`, `std::vector<std::string> take_messages()`.
- `class SaveStore`: `const JsonValue* get(std::string_view key) const`, `void set(std::string key, JsonValue)`, `bool remove(std::string_view)`, `std::vector<std::string> keys() const`, `void clear()`.
- Typed values on disk: `{"$type": "Vector3", "value": [x, y, z]}` (also `Vector2`, `Color3`, `Matrix4`); a plain table whose own key is `$type` is refused.

- [ ] **Step 1:** Write tests with a `TempDir` as the directory: SV1 name rules; SV2 set/get/remove/keys/clear and `Get`'s default; SV3 values round-trip through a flush and a fresh `SaveStores` (nested table, array, Vector3, Color3, Matrix4); SV4 a function, an Instance, a cyclic table, a mixed-key table each raise naming the kind; SV5 nothing is written before one second has passed, then one file; `flush()` writes at once; the write leaves no temp file; SV6 corrupt file opens empty, a message is taken, the file is untouched until `Set`; SV7 an unwritable directory produces a message and no Lua error; SV8 with no directory, values last the session only; SV9 strict analysis accepts the members.
- [ ] **Step 2:** Build and run `./build/sandbox "[saves]"`: fails to compile.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `./build/sandbox "[saves]"`: all pass.

### Task 5: Docs and whole-suite check

- [ ] **Step 1:** README: a paragraph each under Scripts for TweenService, tags, gamepads, and SaveService, in the README's voice, with one short example apiece; `src/engine_core/README.md` gains a paragraph on each core.
- [ ] **Step 2:** `make test`: every suite passes.
- [ ] **Step 3:** Build the studio, open it, and from the command line / a Test confirm one tween, one tag query, and one save round-trip through the `anarchy` MCP tools.
