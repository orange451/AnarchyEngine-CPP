# Event Arguments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an event carry values to its Lua handlers. A class declares an event's arguments, C++ fires it with values, and `Connect` handlers and `Signal:Wait()` receive them. Then move UserInputService onto this path with no change a script can see.

**Architecture:** `EventQueue` events gain a `shared_ptr<const EventArgs>` (a `std::vector<LuaSlot>`), exposed to handlers through `current_args()` the way `payload()` is today. `DataModel::fire_event(id, name, args)` checks the values against the `LuaParam` list a `lua_event` now declares. The Lua handler path pushes each value with the existing `push_registered`. UserInputService fires `{InputObject, gameProcessed}` through `emit_args` and loses its payload numbering.

**Tech Stack:** C++17, Luau, flecs, Catch2 (sandbox suite), CMake.

**Spec:** `docs/superpowers/specs/2026-10-02-event-arguments-design.md`

## Global Constraints

- C++17. Follow the surrounding code's comment style: short declarative comments that say what and why, no comments that narrate edits.
- `contract_fail` is how a C++ programming error is reported. The sandbox installs a handler that throws `ContractViolation` (sandbox/tests.cpp:39-45). In the studio it aborts.
- Builds warn with `-Wall -Wextra`. Leave no new warnings, `-Wswitch` included.
- Build the suite: `cmake --build build --target sandbox --parallel`. Run one tag: `./build/sandbox "[EA1]"`. Run all: `./build/sandbox`.
- `fire_event(id, name)` keeps working for events declared without arguments, so GuiLayer's existing calls (src/runner/GuiLayer.cpp:357) are unchanged.
- Handlers still receive `(input, gameProcessedEvent)` from UserInputService. `kInputSignalArgs` is unchanged.

## Review Focus

- An event fired with values while nothing listens: no allocation is kept and no error, but a wrong declaration still fails (Task 2 test EA5 fires with no listener).
- A handler that fires another event with values while running: the running handler's `current_args()` must still be its own (Task 1 test EQ2).
- Values queued for an instance that is destroyed before the drain: released, and never delivered (Task 2 test EA7).
- A Lua `Wait()` resumed by an event with values: the values are pushed when the event fires, not when the thread resumes, so they survive the event's release (Task 3 test EA2).
- An unknown event name passed to `fire_event`: does nothing, as today, so a studio-side caller with a typo cannot abort the studio (Task 2 test EA5b).

---

## File Structure

| File | Responsibility | Change |
| --- | --- | --- |
| `src/engine_core/InputRecord.hpp` | The `InputRecord` struct, moved here from UserInputService.hpp | Create (Task 4) |
| `src/engine_core/LuaApi.hpp` | `LuaSlot` gains `InputObject`; `lua_event` overload with params | Modify (Tasks 2, 4) |
| `src/engine_core/Events.hpp/.cpp` | `EventArgs`, `emit_args`, `current_args`, `queued_with_args`; release on seal and shutdown; `emit_payload` removed | Modify (Tasks 1, 4) |
| `src/engine_core/DataModel.hpp/.cpp` | `fire_event(id, name, args)` with the declaration check | Modify (Task 2) |
| `src/engine_core/ScriptRuntime.hpp/.cpp` | `invoke_listener_args`, `make_ready_args`; input-only helpers removed | Modify (Tasks 3, 4) |
| `src/engine_core/ScriptBindings.cpp` | Handler branches push values; `push_registered` handles `InputObject` | Modify (Tasks 3, 4) |
| `src/engine_core/PropertyReflection.cpp` | `same_slot` handles `InputObject` | Modify (Task 4) |
| `src/engine_services/UserInputService.hpp/.cpp` | Fires with `emit_args`; payload numbering removed | Modify (Task 4) |
| `sandbox/event_args_tests.cpp` | EQ and EA tests, with a test-only `EventProbe` class | Create (Task 1), extend (Tasks 2, 3) |
| `CMakeLists.txt` | Add the new test file to `sandbox` | Modify (Task 1) |

---

### Task 1: The queue carries values

**Files:**
- Modify: `src/engine_core/Events.hpp` (includes; `EventArgs`; `EventQueue` public API; `Event` struct; `current_args_` member)
- Modify: `src/engine_core/Events.cpp` (`invoke`, new `emit_args`, `seal_instance`, `shutdown`, new `queued_with_args`)
- Create: `sandbox/event_args_tests.cpp`
- Modify: `CMakeLists.txt:757-781` (sandbox sources)

The spec's EA4 (re-firing from a handler) and EA9 (Immediate policy) are EQ2 and EQ5 here, tested on the queue directly.

**Interfaces:**
- Produces: `using EventArgs = std::vector<LuaSlot>;` in `engine_core`. `void EventQueue::emit_args(SignalId signal, InstanceId id, EventArgs args);`, `const EventArgs* EventQueue::current_args() const;`, `std::size_t EventQueue::queued_with_args() const;`

- [ ] **Step 1: Write the failing tests**

Create `sandbox/event_args_tests.cpp`:

```cpp
// Event arguments: values an event carries to its handlers.

#include "support.hpp"

#include "Contract.hpp"
#include "Events.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

engine_core::LuaSlot number_slot(double value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

// A host signal on its own queue, as RunService and UserInputService keep theirs.
struct QueueRig {
    SimRole role;
    engine_core::EventQueue queue;
    engine_core::Signal signal;

    QueueRig() { queue.host_signal(&signal); }
    ~QueueRig() { queue.release_signal(signal); }
};

}  // namespace

TEST_CASE("EQ1 a handler reads the values its event was emitted with", "[EQ1]") {
    QueueRig rig;
    std::vector<double> seen;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        const engine_core::EventArgs* args = rig.queue.current_args();
        REQUIRE(args != nullptr);
        REQUIRE(args->size() == 2);
        seen.push_back((*args)[0].number);
        seen.push_back((*args)[1].number);
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1), number_slot(2)});
    REQUIRE(rig.queue.queued_with_args() == 1);
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{1, 2});
    REQUIRE(rig.queue.queued_with_args() == 0);
    REQUIRE(rig.queue.current_args() == nullptr);
}

TEST_CASE("EQ2 a handler that emits again keeps its own values, and the next drain delivers the new ones", "[EQ2]") {
    QueueRig rig;
    std::vector<double> seen;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        const double first = (*rig.queue.current_args())[0].number;
        if (first == 1) {
            rig.queue.emit_args(rig.signal.id(), 0, {number_slot(2)});
        }
        seen.push_back((*rig.queue.current_args())[0].number);
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1)});
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{1});
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{1, 2});
}

TEST_CASE("EQ3 two events in one drain each bring their own values", "[EQ3]") {
    QueueRig rig;
    std::vector<double> seen;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        seen.push_back((*rig.queue.current_args())[0].number);
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(5)});
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(6)});
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{5, 6});
}

TEST_CASE("EQ4 dropped events let their values go, and nothing is kept without a listener", "[EQ4]") {
    QueueRig rig;
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1)});
    REQUIRE(rig.queue.queued_with_args() == 0);

    engine_core::Connection connection = rig.signal.connect([](engine_core::InstanceId, engine_core::Field) {});
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1)});
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(2)});
    REQUIRE(rig.queue.queued_with_args() == 2);
    rig.queue.drop_pending();
    REQUIRE(rig.queue.queued_with_args() == 0);
    connection.disconnect();
}

TEST_CASE("EQ5 the Immediate policy delivers the values at once", "[EQ5]") {
    QueueRig rig;
    rig.queue.set_policy(engine_core::EventPolicy::Immediate);
    double seen = 0;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        seen = (*rig.queue.current_args())[0].number;
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(9)});
    REQUIRE(seen == 9);
    REQUIRE(rig.queue.current_args() == nullptr);
}
```

Add the file to the `sandbox` target in `CMakeLists.txt`, after `sandbox/shadow_planner_tests.cpp`:

```cmake
    sandbox/shadow_planner_tests.cpp
    sandbox/event_args_tests.cpp
)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build --target sandbox --parallel`
Expected: compile errors: `emit_args`, `current_args`, `queued_with_args`, and `EventArgs` are not members of `engine_core` or `EventQueue`.

- [ ] **Step 3: Implement**

In `src/engine_core/Events.hpp`, add includes and the alias after `class EventQueue;`:

```cpp
#include "types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace engine_core {

class TaskScheduler;
class EventQueue;
struct LuaSlot;

// The values an event carries to its handlers, in the order its class declares them.
using EventArgs = std::vector<LuaSlot>;
```

In `EventQueue`'s public section, after `std::uint64_t payload() const { return payload_; }`, add:

```cpp
    // An event that carries values for its handlers, such as an instance event
    // declared with arguments. Its field is Reflected and its origin Simulation.
    // Nothing is kept when nothing listens.
    void emit_args(SignalId signal, InstanceId id, EventArgs args);
    // The values of the event whose handlers are running. Null outside a handler,
    // and for an event emitted without values.
    const EventArgs* current_args() const { return current_args_; }
    // Queued events still holding values. For tests.
    std::size_t queued_with_args() const;
```

In the private `struct Event`, after `std::uint64_t payload = 0;`, add:

```cpp
        // Shared, so the copies the ring makes are cheap. Null for none.
        std::shared_ptr<const EventArgs> args;
```

After `std::uint64_t payload_ = 0;` add:

```cpp
    const EventArgs* current_args_ = nullptr;
```

In `src/engine_core/Events.cpp`, add `#include "LuaApi.hpp"` after `#include "Contract.hpp"`. Replace the body of `EventQueue::invoke` from the `struct Restore` on:

```cpp
    // Immediate events nest, so the outer event's payload and values come back
    // after, even when a handler throws.
    struct Restore {
        std::uint64_t& payload;
        const std::uint64_t outer;
        const EventArgs*& args;
        const EventArgs* const outer_args;
        ~Restore() {
            payload = outer;
            args = outer_args;
        }
    } restore{payload_, payload_, current_args_, current_args_};
    payload_ = event.payload;
    current_args_ = event.args.get();
    invoke_connections(*signal, event);
```

After `EventQueue::emit_payload`, add:

```cpp
void EventQueue::emit_args(SignalId signal, InstanceId id, EventArgs args) {
    Signal* live = resolve(signal);
    if (live == nullptr || live->listeners_ <= 0) {
        return;
    }
    ++counts_[origin_index(WriteOrigin::Simulation)];
    Event event;
    event.signal = signal;
    event.instance = id;
    event.owner = live->owner_;
    event.field = Field::Reflected;
    event.live = true;
    if (!args.empty()) {
        event.args = std::make_shared<const EventArgs>(std::move(args));
    }
    post(event);
}

std::size_t EventQueue::queued_with_args() const {
    std::size_t count = 0;
    std::size_t index = head_;
    for (std::size_t n = 0; n < size_; ++n) {
        if (events_[index].args) {
            ++count;
        }
        index = (index + 1) % events_.size();
    }
    return count;
}
```

In `seal_instance`, after `event.signal = SignalId{};`, add `event.args.reset();`.

In `shutdown`, before `size_ = 0;`, add:

```cpp
    // Queued events let their values go with them.
    for (Event& event : events_) {
        event = Event{};
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[EQ1],[EQ2],[EQ3],[EQ4],[EQ5]"`
Expected: all 5 test cases pass.

- [ ] **Step 5: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass, as before this task.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/Events.hpp src/engine_core/Events.cpp sandbox/event_args_tests.cpp CMakeLists.txt
git commit -m "Let a queued event carry values to its handlers"
```

---

### Task 2: Declared arguments and `fire_event` with values

**Files:**
- Modify: `src/engine_core/LuaApi.hpp:184-192` (`lua_event` overload)
- Modify: `src/engine_core/DataModel.hpp:331-336` (`fire_event` declarations)
- Modify: `src/engine_core/DataModel.cpp:980-996` (`fire_event`, new argument check)
- Modify: `sandbox/event_args_tests.cpp`

**Interfaces:**
- Consumes: `EventArgs`, `EventQueue::emit_args`, `EventQueue::queued_with_args` (Task 1).
- Produces: `inline LuaField lua_event(const char* name, const LuaParam* params, int count);`, `void DataModel::fire_event(InstanceId id, std::string_view name, EventArgs args);`, and the test-only class `EventProbe` with events `Fired(count: number, where: Vector3, key: EnumItem, who: Instance?)`, `Strict(who: Instance)`, and `Bare()`, which Task 3 uses.

- [ ] **Step 1: Write the failing tests**

Add to `sandbox/event_args_tests.cpp`. First, the includes and the probe class: add `#include "DataModel.hpp"` and `#include "Enum.hpp"` to the includes, and inside the anonymous namespace, after `QueueRig`:

```cpp
// A class whose only purpose is to declare events for these tests.
class EventProbe : public engine_core::DataModel {
public:
    EventProbe(engine_core::DataModel::ChildTag tag, engine_core::DataModel::State& state, engine_core::InstanceId id)
        : engine_core::DataModel(tag, state, id) {}
    const char* class_name() const override { return "EventProbe"; }
};

const engine_core::LuaParam kFiredArgs[] = {
    {"count", "number"}, {"where", "Vector3"}, {"key", "EnumItem"}, {"who", "Instance?"}};
const engine_core::LuaParam kStrictArgs[] = {{"who", "Instance"}};

engine_core::LuaSlot vec3_slot(float x, float y, float z) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Vec3;
    slot.vec = engine_core::Vec3{x, y, z};
    return slot;
}

engine_core::LuaSlot key_slot(int value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Enum;
    slot.enum_type = &engine_core::key_code_enum();
    slot.number = value;
    return slot;
}

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::LuaSlot nil_slot() { return engine_core::LuaSlot{}; }

// The four values Fired declares: 3, (1, 2, 3), KeyCode.W, and who.
engine_core::EventArgs fired_args(engine_core::LuaSlot who) {
    return {number_slot(3), vec3_slot(1, 2, 3), key_slot(119), std::move(who)};
}

}  // namespace

ANARCHY_LUA_REGISTER(register_event_probe_lua) {
    const engine_core::LuaField fields[] = {
        engine_core::lua_event("Fired", kFiredArgs, 4),
        engine_core::lua_event("Strict", kStrictArgs, 1),
        engine_core::lua_event("Bare"),
    };
    engine_core::register_lua_class("EventProbe", "Instance", fields, static_cast<int>(std::size(fields)));
}

namespace {
```

This closes the anonymous namespace for the registration and reopens it after. Keep the closing `}  // namespace` that already ends the helper block before the first `TEST_CASE`. `ANARCHY_LUA_REGISTER` (LuaApi.hpp:269) defines a static function the loader runs at startup, as SoundEmitter.cpp:263 uses it. Add `#include <iterator>` for `std::size`.

Then the tests:

```cpp
TEST_CASE("EA5 firing with the wrong count or kind is a contract failure, even with nothing listening", "[EA5]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    const engine_core::InstanceId id = probe.id();
    REQUIRE_THROWS_AS(game.fire_event(id, "Fired", {number_slot(3)}), engine_core::ContractViolation);
    engine_core::EventArgs wrong = fired_args(nil_slot());
    wrong[1] = number_slot(1);
    REQUIRE_THROWS_AS(game.fire_event(id, "Fired", wrong), engine_core::ContractViolation);
    // An event declared with arguments, fired without them.
    REQUIRE_THROWS_AS(game.fire_event(id, "Fired"), engine_core::ContractViolation);
    // An event declared without arguments, fired with some.
    REQUIRE_THROWS_AS(game.fire_event(id, "Bare", {number_slot(1)}), engine_core::ContractViolation);
    REQUIRE_NOTHROW(game.fire_event(id, "Fired", fired_args(nil_slot())));
    REQUIRE_NOTHROW(game.fire_event(id, "Bare"));
}

TEST_CASE("EA5b an event the class does not declare does nothing, as before", "[EA5b]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Nope"));
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Nope", {number_slot(1)}));
}

TEST_CASE("EA6 nil passes only where the type ends in ?", "[EA6]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Fired", fired_args(nil_slot())));
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Strict", {instance_slot(probe.id())}));
    REQUIRE_THROWS_AS(game.fire_event(probe.id(), "Strict", {nil_slot()}), engine_core::ContractViolation);
}

TEST_CASE("EA7 destroying an instance lets its queued values go", "[EA7]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    const engine_core::InstanceId id = probe.id();
    int calls = 0;
    game.event_signal(id, "Fired").connect([&](engine_core::InstanceId, engine_core::Field) { ++calls; });
    game.fire_event(id, "Fired", fired_args(nil_slot()));
    game.fire_event(id, "Fired", fired_args(nil_slot()));
    REQUIRE(game.events().queued_with_args() == 2);
    game.destroy(id);
    REQUIRE(game.events().queued_with_args() == 0);
    game.events().drain();
    REQUIRE(calls == 0);
}

TEST_CASE("EA8 the script checker types an event's callback from its declaration", "[EA8]") {
    const std::string definitions = engine_core::lua_analysis_definitions();
    INFO(definitions.substr(0, 400));
    REQUIRE(definitions.find("Fired: Signal_EventProbe_Fired") != std::string::npos);
    REQUIRE(definitions.find("count: number, where: Vector3") != std::string::npos);
    REQUIRE(definitions.find("Bare: Signal\n") != std::string::npos);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile errors: no `lua_event` overload taking three arguments, and no `fire_event` taking an `EventArgs`.

- [ ] **Step 3: Implement the declaration**

In `src/engine_core/LuaApi.hpp`, change the comment on `lua_event` and add the overload after it:

```cpp
// A signal on each instance of the class, such as a Button's Action. The
// engine fires it with DataModel::fire_event; its callbacks get no arguments.
inline LuaField lua_event(const char* name) {
    LuaField field;
    field.name = name;
    field.type_name = "Signal";
    field.event = true;
    return field;
}

// An event whose callbacks get these values, in order. fire_event must pass
// exactly these: one LuaSlot per param, of the kind its type_name names.
inline LuaField lua_event(const char* name, const LuaParam* params, int count) {
    LuaField field = lua_event(name);
    field.params = params;
    field.param_count = count;
    return field;
}
```

- [ ] **Step 4: Implement `fire_event` with values**

In `src/engine_core/DataModel.hpp`, replace the `fire_event` declaration and its comment:

```cpp
    // Fires that event for whatever is connected to it; nothing when nothing
    // is. Its handlers get the instance, Field::Reflected, and args through
    // EventQueue::current_args. args must match the event's declared arguments
    // (lua_event), or the contract fails, whether or not anything listens. An
    // event the class does not declare does nothing. SimulationThread.
    void fire_event(InstanceId id, std::string_view name, EventArgs args);
    // An event declared without arguments.
    void fire_event(InstanceId id, std::string_view name);
```

DataModel.hpp already includes `Events.hpp` (line 4), where `EventArgs` is declared.

In `src/engine_core/DataModel.cpp`, in the anonymous namespace near the top (or a new one before `fire_event`), add:

```cpp
// Whether slot is a value an event argument declared as type may carry.
bool event_arg_fits(const LuaParam& param, const LuaSlot& slot) {
    const std::string_view type = param.type_name != nullptr ? param.type_name : "";
    using Kind = LuaSlot::Kind;
    if (type == "number") {
        return slot.kind == Kind::Number;
    }
    if (type == "boolean") {
        return slot.kind == Kind::Bool;
    }
    if (type == "string") {
        return slot.kind == Kind::String;
    }
    if (type == "Vector3") {
        return slot.kind == Kind::Vec3;
    }
    if (type == "Vector2") {
        return slot.kind == Kind::Vec2;
    }
    if (type == "Color3") {
        return slot.kind == Kind::Color;
    }
    if (type == "Matrix4") {
        return slot.kind == Kind::Matrix4;
    }
    if (type == "EnumItem") {
        return slot.kind == Kind::Enum;
    }
    const bool optional = !type.empty() && type.back() == '?';
    const std::string base(optional ? type.substr(0, type.size() - 1) : type);
    if (lua_class_known(base.c_str())) {
        return slot.kind == Kind::Instance || (optional && slot.kind == Kind::Nil);
    }
    return false;
}

// Fails the contract when args do not match what the class declares for the event.
void check_event_args(const DataModel& object, std::string_view name, const EventArgs& args) {
    const char* class_name = object.class_name() != nullptr ? object.class_name() : "Instance";
    const LuaField* field = lua_class_find(class_name, name);
    if (field == nullptr || !field->event) {
        return;
    }
    const std::string label = std::string(class_name) + "." + std::string(name);
    if (static_cast<int>(args.size()) != field->param_count) {
        const std::string message = "fire_event: " + label + " takes " + std::to_string(field->param_count) +
                                    " values, given " + std::to_string(args.size());
        contract_fail(message.c_str());
    }
    for (int index = 0; index < field->param_count; ++index) {
        if (!event_arg_fits(field->params[index], args[static_cast<std::size_t>(index)])) {
            const LuaParam& param = field->params[index];
            const std::string message = "fire_event: " + label + " argument " + std::to_string(index + 1) + " (" +
                                        (param.name != nullptr ? param.name : "") + ") must be " +
                                        (param.type_name != nullptr ? param.type_name : "?");
            contract_fail(message.c_str());
        }
    }
}
```

Replace `DataModel::fire_event` with:

```cpp
void DataModel::fire_event(InstanceId id, std::string_view name, EventArgs args) {
    if (!on_gameplay_thread()) {
        contract_fail("fire_event runs on SimulationThread");
    }
    const DataModel* object = instance(id);
    if (object == nullptr) {
        return;
    }
    check_event_args(*object, name, args);
    InstanceSignals* bag = bag_for(id);
    if (bag == nullptr) {
        return;
    }
    for (const std::unique_ptr<InstanceSignals::Event>& event : bag->events) {
        if (event->name == name) {
            if (event->signal.bound() && event->signal.listeners_ > 0) {
                state_->events.emit_args(event->signal.id(), id, std::move(args));
            }
            return;
        }
    }
}

void DataModel::fire_event(InstanceId id, std::string_view name) { fire_event(id, name, EventArgs{}); }
```

`emit_args` with empty args queues the same event `emit(..., Field::Reflected)` did, so GuiLayer's no-argument events behave as before.

`lua_class_find(const char*, std::string_view)` (LuaApi.cpp:253) searches base classes too, so a `Button` finds the events `GuiBase` declares.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[EA5],[EA5b],[EA6],[EA7],[EA8]"`
Expected: all pass. If EA8's second check fails, print `definitions` around `Signal_EventProbe_Fired` and match the parameter text `signal_type` (AnalysisDefinitions.cpp:199) actually writes, then fix the test string, not the generator.

- [ ] **Step 6: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass. The GUI tests (`gui_tests.cpp`) cover `fire_event(id, name)` through GuiLayer's events.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/LuaApi.hpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp sandbox/event_args_tests.cpp
git commit -m "Declare an event's arguments, and check fire_event's values against them"
```

---

### Task 3: Lua handlers receive the values

**Files:**
- Modify: `src/engine_core/ScriptRuntime.hpp:344-366` (declarations)
- Modify: `src/engine_core/ScriptRuntime.cpp` (near `invoke_listener`, :1550, and `make_ready_number`, :987)
- Modify: `src/engine_core/ScriptBindings.cpp:671-685` (`signal_connect` handler) and `:722-738` (`signal_wait` handler)
- Modify: `sandbox/event_args_tests.cpp`

**Interfaces:**
- Consumes: `EventQueue::current_args()` (Task 1). `EventProbe` and `fired_args` (Task 2).
- Produces: `void ScriptRuntime::invoke_listener_args(Vm& vm, int ref, InstanceId script, std::uint32_t generation, const EventArgs* args);` and `void ScriptRuntime::make_ready_args(Thread& thread, const EventArgs* args);`, both private, used by ScriptBindings. Task 4 routes `kSignalInput` through them.

- [ ] **Step 1: Write the failing tests**

Add to `sandbox/event_args_tests.cpp`. Inside the helper namespace, add:

```cpp
bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

// A probe named Probe under Workspace, and a script beside it, running in play.
struct ProbeRig {
    ScriptRig rig;
    engine_core::InstanceId probe = 0;

    explicit ProbeRig(const char* source) {
        EventProbe& made = rig.game.create<EventProbe>();
        probe = made.id();
        rig.game.set_name(probe, "Probe");
        rig.game.set_parent(probe, rig.game.scene_service("Workspace"));
        add_script(rig.game, "Listener", source);
        rig.game.start_simulation();
        rig.frames(1);
    }
};
```

Then:

```cpp
TEST_CASE("EA1 a Connect handler gets each value the event was fired with", "[EA1]") {
    ProbeRig probe(R"(
        workspace.Probe.Fired:Connect(function(count, where, key, who)
            print("fired", count, where.X, where.Y, where.Z, key == Enum.KeyCode.W, who == workspace.Probe, who == nil)
        end)
    )");
    probe.rig.game.fire_event(probe.probe, "Fired", fired_args(instance_slot(probe.probe)));
    probe.rig.frames(1);
    INFO(probe.rig.runtime.last_error());
    const auto output = probe.rig.runtime.drain_output();
    REQUIRE(has_line(output, "fired\t3\t1\t2\t3\ttrue\ttrue\tfalse\n"));

    probe.rig.game.fire_event(probe.probe, "Fired", fired_args(nil_slot()));
    probe.rig.frames(1);
    REQUIRE(has_line(probe.rig.runtime.drain_output(), "fired\t3\t1\t2\t3\ttrue\tfalse\ttrue\n"));
}

TEST_CASE("EA2 Wait returns the fired values", "[EA2]") {
    ProbeRig probe(R"(
        task.spawn(function()
            local count, where = workspace.Probe.Fired:Wait()
            print("waited", count, where.Y)
        end)
    )");
    probe.rig.game.fire_event(probe.probe, "Fired", fired_args(nil_slot()));
    probe.rig.frames(2);
    INFO(probe.rig.runtime.last_error());
    REQUIRE(has_line(probe.rig.runtime.drain_output(), "waited\t3\t2\n"));
}

TEST_CASE("EA3 two events fired in one step each reach Lua with their own values", "[EA3]") {
    ProbeRig probe(R"(
        workspace.Probe.Fired:Connect(function(count)
            print("count", count)
        end)
    )");
    engine_core::EventArgs first = fired_args(nil_slot());
    engine_core::EventArgs second = fired_args(nil_slot());
    second[0] = number_slot(4);
    probe.rig.game.fire_event(probe.probe, "Fired", first);
    probe.rig.game.fire_event(probe.probe, "Fired", second);
    probe.rig.frames(1);
    const auto output = probe.rig.runtime.drain_output();
    REQUIRE(has_line(output, "count\t3\n"));
    REQUIRE(has_line(output, "count\t4\n"));
}

TEST_CASE("EA3b an event without arguments still reaches Lua with none", "[EA3b]") {
    ProbeRig probe(R"(
        workspace.Probe.Bare:Connect(function(...)
            print("bare", select("#", ...))
        end)
    )");
    probe.rig.game.fire_event(probe.probe, "Bare");
    probe.rig.frames(1);
    REQUIRE(has_line(probe.rig.runtime.drain_output(), "bare\t0\n"));
}
```

Workspace takes any class that is not an asset (Containment.cpp `placement_error`), so the probe can go there.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[EA1],[EA2],[EA3],[EA3b]"`
Expected: EA1 fails with `fired\tnil\t...` missing (the handler gets no values, so indexing `where.X` errors, and `last_error()` says `attempt to index nil`). EA2 and EA3 fail the same way. EA3b passes already.

- [ ] **Step 3: Implement the runtime helpers**

In `src/engine_core/ScriptRuntime.hpp`, after the `invoke_listener` declaration, add:

```cpp
    // An event with values: the listener gets each one, in order. Null gets none.
    void invoke_listener_args(Vm& vm, int ref, InstanceId script, std::uint32_t generation, const EventArgs* args);
```

After `void make_ready_number(Thread& thread, double result);` add:

```cpp
    // Resumes a Wait with an event's values as its results. Null resumes it with none.
    void make_ready_args(Thread& thread, const EventArgs* args);
```

ScriptRuntime.hpp already includes `Events.hpp` (line 3), so `EventArgs` is visible.

In `src/engine_core/ScriptRuntime.cpp`, inside `namespace engine_core {` before the anonymous namespace, declare the pusher ScriptBindings.cpp defines:

```cpp
void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id, std::uint32_t world);
```

In the anonymous namespace, add:

```cpp
// Pushes an event's values in order and says how many. The values are copied
// onto the stack, so the event may go once this returns.
int push_event_args(lua_State* state, ScriptRuntime* runtime, const EventArgs* args) {
    if (args == nullptr || args->empty()) {
        return 0;
    }
    const int count = static_cast<int>(args->size());
    lua_checkstack(state, count);
    for (const LuaSlot& slot : *args) {
        push_registered(state, runtime, slot, 0, 0);
    }
    return count;
}
```

After `ScriptRuntime::make_ready_number`, add:

```cpp
void ScriptRuntime::make_ready_args(Thread& thread, const EventArgs* args) {
    if (!unpark(thread)) {
        return;
    }
    thread.nargs = push_event_args(thread.co, this, args);
    ready(thread);
}
```

After `ScriptRuntime::invoke_listener`, add:

```cpp
void ScriptRuntime::invoke_listener_args(Vm& vm, int ref, InstanceId script, std::uint32_t generation,
                                         const EventArgs* args) {
    guarded(vm, [&] {
        Thread* thread = start_listener(vm, ref, script, generation);
        if (thread == nullptr) {
            return;
        }
        thread->nargs = push_event_args(thread->co, this, args);
        run_listener(*thread);
    });
}
```

- [ ] **Step 4: Route instance events through them**

In `src/engine_core/ScriptBindings.cpp`, `signal_connect`'s handler, replace:

```cpp
            } else if (kind == kSignalEvent) {
                runtime->invoke_listener(owner, held->ref, script, generation, nullptr, false, 0);
```

with:

```cpp
            } else if (kind == kSignalEvent) {
                runtime->invoke_listener_args(owner, held->ref, script, generation,
                                              runtime->game_->events().current_args());
```

In `signal_wait`'s handler, replace:

```cpp
                } else if (kind == kSignalEvent) {
                    runtime->make_ready(*waiting, nullptr);
```

with:

```cpp
                } else if (kind == kSignalEvent) {
                    runtime->make_ready_args(*waiting, runtime->game_->events().current_args());
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[EA1],[EA2],[EA3],[EA3b]"`
Expected: all pass.

- [ ] **Step 6: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/ScriptRuntime.hpp src/engine_core/ScriptRuntime.cpp src/engine_core/ScriptBindings.cpp sandbox/event_args_tests.cpp
git commit -m "Pass an event's values to its Lua handlers and to Wait"
```

---

### Task 4: UserInputService fires its InputObject as an event value

**Files:**
- Create: `src/engine_core/InputRecord.hpp`
- Modify: `src/engine_services/UserInputService.hpp:15-25` (struct moves out), `:91-96` (`record` goes), `:146-148` (members go)
- Modify: `src/engine_services/UserInputService.cpp:311-370` (`dispatch`, `record`, `reset`)
- Modify: `src/engine_core/LuaApi.hpp:21-37` (`LuaSlot`)
- Modify: `src/engine_core/ScriptBindings.cpp` (`push_registered` switch, :297-345; `signal_connect` and `signal_wait` input branches)
- Modify: `src/engine_core/PropertyReflection.cpp:140-166` (`same_slot` switch)
- Modify: `src/engine_core/ScriptRuntime.hpp:359-366`, `ScriptRuntime.cpp:1570-1598` (input-only helpers go)
- Modify: `src/engine_core/Events.hpp/.cpp` (`emit_payload` goes)
- Test: existing `sandbox/tests.cpp` UserInputService cases, `sandbox/scene_camera_tests.cpp`, `sandbox/plugin_tests.cpp`, plus one new test in `sandbox/event_args_tests.cpp`

**Interfaces:**
- Consumes: `emit_args`, `current_args` (Task 1). `invoke_listener_args`, `make_ready_args` (Task 3).
- Produces: `LuaSlot::Kind::InputObject` and `LuaSlot::input`. `engine_core/InputRecord.hpp`.

- [ ] **Step 1: Write the failing test**

The existing input tests are the proof that nothing a script sees changes. Add one test that pins the new path. In `sandbox/event_args_tests.cpp`:

```cpp
TEST_CASE("EA10 an InputObject travels as an event value", "[EA10]") {
    SimRole role;
    engine_core::Game game;
    // Posting and dispatching a key with a C++ listener on InputBegan
    // delivers the record as the first value and gameProcessed as the second.
    game.input().set_active(true);
    int seen_key = 0;
    bool seen_processed = true;
    game.input().signal(engine_core::UserInputService::Kind::Began)->connect(
        [&](engine_core::InstanceId, engine_core::Field) {
            const engine_core::EventArgs* args = game.events().current_args();
            REQUIRE(args != nullptr);
            REQUIRE(args->size() == 2);
            REQUIRE((*args)[0].kind == engine_core::LuaSlot::Kind::InputObject);
            seen_key = (*args)[0].input.key;
            seen_processed = (*args)[1].flag;
        });
    game.input().post_key(119, true);
    game.input().dispatch(game.events());
    game.events().drain();
    REQUIRE(seen_key == 119);
    REQUIRE_FALSE(seen_processed);
}
```

Add `#include "UserInputService.hpp"` to the test's includes. `set_active(true)` (UserInputService.hpp:69) is what `ScriptRuntime::attach` calls so the service keeps posts.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile error: `LuaSlot` has no member `input` and no kind `InputObject`.

- [ ] **Step 3: Move `InputRecord`**

Create `src/engine_core/InputRecord.hpp`, moving the struct and its comments unchanged from UserInputService.hpp:15-25:

```cpp
#pragma once

#include "types.hpp"

namespace engine_core {

// One input event as UserInputService records it, and as an InputObject holds it.
struct InputRecord {
    int type = 22;  // UserInputType.None
    int state = 4;  // UserInputState.None
    int key = 0;    // KeyCode.Unknown
    // Pointer position in the scene view, in points from its top-left corner.
    // z is 0, except for MouseWheel, where z of position is the wheel movement.
    Vec3 position{};
    Vec3 delta{};
    // True when the studio, not the game, took the event.
    bool processed = false;
};

}  // namespace engine_core
```

In `UserInputService.hpp`, delete the struct and add `#include "InputRecord.hpp"` after `#include "Events.hpp"`.

- [ ] **Step 4: `LuaSlot` holds an InputObject**

In `src/engine_core/LuaApi.hpp`, add `#include "InputRecord.hpp"` after `#include "types.hpp"`. In `LuaSlot`, change the enum and add the member after `Matrix4 transform{};`:

```cpp
    // A Vec2 (a Vector2) keeps its x and y in vec, with z 0. An InputObject is
    // only ever an event's value, never a property's.
    enum class Kind { Nil, Bool, Number, String, Instance, Vec3, Color, Matrix4, Signal, Enum, Vec2, InputObject };
```

```cpp
    Matrix4 transform{};
    // An InputObject's record.
    InputRecord input{};
```

In `ScriptBindings.cpp`'s `push_registered`, before `case LuaSlot::Kind::Signal: {`, add:

```cpp
    case LuaSlot::Kind::InputObject:
        push_input_object(state, slot.input);
        return;
```

In `PropertyReflection.cpp`'s `same_slot` switch, after the `Enum` case, add:

```cpp
    case LuaSlot::Kind::InputObject:
        return a.input.type == b.input.type && a.input.state == b.input.state && a.input.key == b.input.key &&
               a.input.position.x == b.input.position.x && a.input.position.y == b.input.position.y &&
               a.input.position.z == b.input.position.z && a.input.delta.x == b.input.delta.x &&
               a.input.delta.y == b.input.delta.y && a.input.delta.z == b.input.delta.z &&
               a.input.processed == b.input.processed;
```

- [ ] **Step 5: Fire through `emit_args`**

In `UserInputService.cpp`'s `dispatch`, replace the payload bookkeeping. The records become a local, and each signal fires with its values:

```cpp
void UserInputService::dispatch(EventQueue& events) {
    std::vector<InputRecord> records;
    {
        std::lock_guard<std::mutex> lock(mu_);
        records.swap(queue_);
        // Under the lock, so a lock that starts after this sees these records dispatched.
        delta_lock_starts_ = lock_starts_.load(std::memory_order_relaxed);
    }
    mouse_delta_ = Vec3{};
    for (const InputRecord& record : records) {
        mouse_ = record.position;
        // ... the existing body from `if (record.type == kMouseMovement)` through the
        // buttons_down_ update, unchanged ...
        Signal* target = signal(kind);
        if (target != nullptr && target->id().valid()) {
            LuaSlot input;
            input.kind = LuaSlot::Kind::InputObject;
            input.input = record;
            LuaSlot processed;
            processed.kind = LuaSlot::Kind::Bool;
            processed.flag = record.processed;
            events.emit_args(target->id(), 0, EventArgs{input, processed});
        }
    }
}
```

Keep the existing body between those two points as it is. Only the loop header, the `record` binding, and the emit change.

Delete `UserInputService::record` (UserInputService.cpp:353-358) and its declaration and comment (UserInputService.hpp:95-96). In `reset`, delete `dispatched_.clear();` and `first_payload_ = next_payload_;`. In the header, delete the members `dispatched_`, `first_payload_`, and `next_payload_` (:146-148). Update `dispatch`'s comment in the header:

```cpp
    // SimulationThread. Applies every queued record to the state the queries
    // read, then fires one signal event for each, carrying its InputObject and
    // whether the studio took it.
    void dispatch(EventQueue& events);
```

Then confirm nothing else uses them: `grep -n "dispatched_\|first_payload_\|next_payload_\|record(" src/engine_services/UserInputService.*` should print nothing about these.

- [ ] **Step 6: Merge the handler branches and remove the input-only helpers**

In `ScriptBindings.cpp`'s `signal_connect` handler, replace the input and event branches with one:

```cpp
            } else if (kind == kSignalInput || kind == kSignalEvent) {
                runtime->invoke_listener_args(owner, held->ref, script, generation,
                                              runtime->game_->events().current_args());
```

In `signal_wait`'s handler, likewise:

```cpp
                } else if (kind == kSignalInput || kind == kSignalEvent) {
                    runtime->make_ready_args(*waiting, runtime->game_->events().current_args());
```

Delete from `ScriptRuntime.hpp` the declarations, with their comments, of `invoke_listener_input`, `make_ready_input`, and `delivered_input`. Delete their definitions from `ScriptRuntime.cpp` (:1570-1598). If `struct InputRecord;` at ScriptRuntime.hpp:29 is no longer used there, delete it.

Delete `EventQueue::emit_payload` from Events.hpp (with its comment) and Events.cpp. First confirm it has no callers left: `grep -rn "emit_payload" src` should print only those two definitions.

- [ ] **Step 7: Build with no new warnings**

Run: `cmake --build build --target sandbox --parallel 2>&1 | grep -E "warning|error" | grep -v "_deps/"`
Expected: no output. A `-Wswitch` warning names a `switch` over `LuaSlot::Kind` this plan missed. Add an `InputObject` case to it, matching how that switch treats a kind it does not take (Gui.cpp:160 has a `default:` and needs nothing).

- [ ] **Step 8: Run the tests**

Run: `./build/sandbox "[EA10]"` and then `./build/sandbox`
Expected: EA10 passes, and the whole suite passes unchanged, including the UserInputService test in tests.cpp around :3784 (`began\tEnum.KeyCode.W...`, `waited\tW\tfalse`), `scene_camera_tests.cpp`, and `plugin_tests.cpp`.

- [ ] **Step 9: Build the studio and fly the camera**

Run: `make` and then `make run`.
Expected: the studio opens. In a scene view, hold the right mouse button and move the mouse: the camera turns. WASD, Q, and E move it. Release the button: the pointer comes back where it was. Quit the studio.

- [ ] **Step 10: Commit**

```bash
git add src/engine_core/InputRecord.hpp src/engine_core/LuaApi.hpp src/engine_core/ScriptBindings.cpp src/engine_core/PropertyReflection.cpp src/engine_core/ScriptRuntime.hpp src/engine_core/ScriptRuntime.cpp src/engine_core/Events.hpp src/engine_core/Events.cpp src/engine_services/UserInputService.hpp src/engine_services/UserInputService.cpp sandbox/event_args_tests.cpp
git commit -m "Fire UserInputService's signals with their InputObject as an event value"
```
