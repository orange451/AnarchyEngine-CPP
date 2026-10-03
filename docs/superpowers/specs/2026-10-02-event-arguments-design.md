# Event Arguments Design

2026-10-02 · First of three: event arguments, then the Core service (`2026-10-02-core-service-design.md`), then the Dragger (`2026-10-02-dragger-brainstorm.md`), whose `Dragged(handle, offset)` needs this.

## Goal

An event can carry values to its Lua handlers. A class declares an event's arguments, C++ fires it with values, and `Connect` handlers and `Signal:Wait()` receive them. Today each signal kind passes its own hard-coded argument (ScriptBindings.cpp, `signal_connect` and `signal_wait`), and an instance event (`lua_event`) passes none. UserInputService moves onto the new path to prove it, with no change a script can see.

## Decisions

| Question | Decision |
| --- | --- |
| Where the values live | On the queued event in `EventQueue` (a shared pointer), dropped once its handlers have run. |
| Value type | `LuaSlot`, the type properties already read and write through. |
| Declaring arguments | `lua_event` takes a `LuaParam` list, which the checker and completion already read for callback parameters. |
| Wrong count or kind at fire time | A C++ bug: `contract_fail`. |
| The no-argument form | `fire_event(id, name)` stays and fires with no values. |
| Proof | UserInputService fires `InputBegan`, `InputChanged`, and `InputEnded` with `{InputObject, gameProcessed}`, and its payload numbering goes away. |
| `Changed` and the phase signals | Unchanged. They could move onto this later, but nothing needs them to. |

## Architecture

### 1. `LuaSlot` holds an InputObject (`engine_core/LuaApi.hpp`)

1. `InputRecord` moves from `engine_services/UserInputService.hpp` into its own `engine_core/InputRecord.hpp`, so `LuaApi.hpp` can hold one without depending on a service. `UserInputService.hpp` includes it. The fields and their comments are unchanged.
2. `LuaSlot::Kind` gains `InputObject`, and `LuaSlot` gains an `InputRecord input` member, used only by that kind.
3. `push_registered` (ScriptBindings.cpp) pushes an `InputObject` slot with the existing `push_input_object`. No property has the type, so the property reflection code never meets one. A write path that receives one refuses it, as for any kind it does not take.

### 2. The queue carries values (`engine_core/Events.{hpp,cpp}`)

1. `using EventArgs = std::vector<LuaSlot>;`
2. `void emit_args(SignalId signal, InstanceId id, EventArgs args);` queues an event like `emit`, with origin Simulation and field `Reflected`, and keeps `args` with it. Under the Immediate policy, the handlers run at once with the values, as `emit` runs them.
3. Storage: `Event` gains `std::shared_ptr<const EventArgs> args`, null for none. The values go when the event leaves the ring: `pop` hands the event to `invoke` and they go after its handlers return, and `drop_pending`, `seal_instance` (so `destroy_instance`), and `shutdown` reset the events they drop. `std::size_t queued_with_args() const` counts the queued events still holding values, for tests.
4. `const EventArgs* current_args() const;` returns the values of the event whose handlers are running, and null outside a handler or for an event without values. It works like `payload()` and is saved and restored around nested invokes the same way.

### 3. Declaring and firing instance events (`LuaApi.hpp`, `DataModel`)

1. `lua_event(const char* name, const LuaParam* params, int count)` sets `field.params` and `field.param_count`. The existing one-argument `lua_event(name)` stays.
2. `void DataModel::fire_event(InstanceId id, std::string_view name, EventArgs args);` finds the event as `fire_event(id, name)` does, then checks `args` against the class's declaration: the same count, and each slot's kind matches its param's `type_name` (`number` Number, `boolean` Bool, `string` String, `Vector3` Vec3, `Vector2` Vec2, `Color3` Color, `Matrix4` Matrix4, `InputObject` InputObject, an `EnumItem` an Enum, any registered class an Instance, with a nil Instance allowed when the type ends in `?`). A mismatch calls `contract_fail` naming the class, event, and argument. The check runs even when nothing listens, so a wrong fire fails in every test, not only the ones that connect. With no listeners it then returns, as today.
3. The existing `fire_event(id, name)` calls the new one with no values, so an event declared with arguments fired without them is the same contract failure.

### 4. Handlers receive the values (`engine_core/ScriptBindings.cpp`, `ScriptRuntime`)

1. In `signal_connect`, the `kSignalEvent` branch calls a new `ScriptRuntime::invoke_listener_args(vm, ref, script, generation, const EventArgs*)`, which pushes each slot with `push_registered` and sets `nargs` to their count. A null pointer pushes nothing.
2. In `signal_wait`, the `kSignalEvent` branch calls a new `make_ready_args(thread, const EventArgs*)`, which pushes the same values as Wait's results.
3. Values are pushed while the handler is being invoked, before it runs, so nothing reads them after the event is released.

### 5. UserInputService moves over (`engine_services/UserInputService`, `ScriptBindings`, `ScriptRuntime`)

1. `dispatch` fires each record with `events.emit_args(target->id(), 0, {input_slot(record), bool_slot(record.processed)})` in place of `emit_payload`.
2. `kSignalInput` stays, since `signal_of` uses it to find the service's signal. In `signal_connect` and `signal_wait`, its handler branch merges with `kSignalEvent`'s: both push the event's values.
3. These go away: the input-only handler branches, `ScriptRuntime::delivered_input`, `invoke_listener_input`, `make_ready_input`, UserInputService's `first_payload_`, `next_payload_`, and `record(payload)`. The `emit_payload` overload stays only if something else still calls it. Otherwise it goes too.
4. Handlers still receive `(input, gameProcessedEvent)`. `kInputSignalArgs` is unchanged.

## Tests

Sandbox Catch2, in `sandbox/game_services_tests.cpp` beside the input tests, with a test-only class that declares a `lua_event` with arguments. The tests register that class themselves.

- EA1 An event declared `(number, Vector3, EnumItem, Instance?)` and fired with those values reaches a `Connect` handler with each value.
- EA2 `local a, b = obj.Event:Wait()` returns the fired values.
- EA3 Two events fired in the same round each reach their handlers with their own values.
- EA4 A handler that fires the same event again gets the new values in the next drain, and the first handler's values are unchanged.
- EA5 Firing with the wrong count, or a wrong kind, is a contract failure.
- EA6 A nil Instance passes only where the type ends in `?`.
- EA7 Destroying the instance with events queued releases their values: `queued_with_args()` is 0.
- EA8 The script checker types a `Connect` callback's parameters from the declaration.
- EA9 The Immediate policy delivers the values too.
- UserInputService: every existing test in `game_services_tests.cpp`, `scene_camera_tests.cpp`, and `plugin_tests.cpp` passes unchanged, and SceneCamera still flies in the studio.
