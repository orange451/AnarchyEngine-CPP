# Collision Groups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `CollisionGroup` string on every PhysicsBase, a runtime `PhysicsService` with Whitelist/Blacklist group types, Box3D filter bits that honor it (welds, PlayerControllers, CanCollide, rays included), and `RaycastParams.CollisionGroup`.

**Architecture:** A pure `CollisionGroups` registry (engine_core) owned by the DataModel beside `Assemblies`, cleared at Play and Stop. `PhysicsWorld` turns a body's group and CanCollide into each shape's 64-bit category/mask, re-filtering every shape when the registry's revision moves. `PhysicsService` is a script service whose methods (PhysicsServiceBindings.cpp) call the registry.

**Tech Stack:** C++17 (MSVC 14.23, /W4), Box3D (filter bits, `b3Shape_SetFilter`), Luau bindings, Catch2 sandbox, the properties-tests harness.

**Spec:** `docs/superpowers/specs/2026-10-11-collision-groups-design.md`

## Global Constraints

- Group limit 62. Bit 62 is `kGroundBit`, bit 63 `kRayBit`. `Default` is bit 0, always registered, Blacklist by default.
- `CollisionGroup`: saved string on PhysicsBase, default `"Default"`, Properties group "Physics". No validation; an unregistered name behaves as `Default`.
- `collidable(a, b)`: an explicit pair setting wins; else true only when both are Blacklist.
- Error texts, verbatim: `Unknown collision group "<name>"; did you forget to call RegisterCollisionGroup?`, `Too many collision groups (the most is 62)`, `Default cannot be unregistered`, `Collision group name must not be empty`, `collisionType must be "Blacklist" or "Whitelist"`.
- PhysicsService state is not saved; DataModel clears it at `start_simulation` and `stop_simulation`.
- CanCollide false: mask is `kRayBit` alone, whatever the group.
- Studio's own rays (`RayFilter::collision_group` empty) see every group; `Workspace:Raycast` uses `"Default"` when params give none.
- Engine distances are "units"; never write "studs".
- Build: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <t> -- /m /v:minimal`. Test exes are not in ALL_BUILD; build them by name.

## Review Focus

1. A group registered after bodies already name it: they must re-filter at the next sync (spec §5), not only bodies made later.
2. Unregistering a group then registering another: the freed bit is reused, and stale explicit settings on that bit must be gone.
3. A welded assembly's member shapes live on one compound body: re-filtering must reach every member's shapes, not only the root's.
4. Terrain chunk shapes are made outside `make_shape`; they must take Default's filter, and re-filter when Default's pairs change.
5. A PlayerController's ground probe and its body's contacts must agree: it must not stand on what it passes through.

---

### Task 1: The `CollisionGroups` registry

**Files:**
- Create: `src/engine_core/CollisionGroups.hpp`, `src/engine_core/CollisionGroups.cpp`
- Create: `sandbox/collision_group_tests.cpp`
- Modify: `CMakeLists.txt` (engine_core sources after `src/engine_core/AssemblyPlay.cpp`; sandbox sources after `sandbox/assembly_play_tests.cpp`)

**Interfaces:**
- Produces: `engine_core::CollisionGroups` with `Type {Blacklist, Whitelist}`, `Info {name, type, list}`, `kMaxGroups = 62`, `kGroundBit`, `kRayBit`, `kDefault = "Default"`; `register_group(name, type)`, `unregister_group(name)`, `set_type(name, type)`, `set_collidable(a, b, bool)` each returning `std::optional<std::string>` (the refusal); `registered(name)`, `collidable(a, b)`, `groups()`, `reset()`, `category(name)`, `collides_with(name)`, `revision()`.

- [ ] **Step 1: Write the failing tests** — `sandbox/collision_group_tests.cpp`:

```cpp
// CollisionGroups: which groups PhysicsService has registered, their types,
// and whether two collide (docs/superpowers/specs/2026-10-11-collision-groups-design.md).

#include "CollisionGroups.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine_core::CollisionGroups;
using Type = CollisionGroups::Type;

TEST_CASE("CG1 Default alone, Blacklist, collides with itself", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE(groups.registered("Default"));
    REQUIRE(groups.collidable("Default", "Default"));
    REQUIRE(groups.category("Default") == 1u);
    REQUIRE((groups.collides_with("Default") & 1u) == 1u);
    // An unregistered name counts as Default.
    REQUIRE(groups.category("Nope") == 1u);
    REQUIRE(groups.collidable("Nope", "Default"));
}

TEST_CASE("CG2 two Blacklist groups collide unless set not to, both ways", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", Type::Blacklist));
    REQUIRE_FALSE(groups.register_group("B", Type::Blacklist));
    REQUIRE(groups.collidable("A", "B"));
    REQUIRE_FALSE(groups.set_collidable("A", "B", false));
    REQUIRE_FALSE(groups.collidable("A", "B"));
    REQUIRE_FALSE(groups.collidable("B", "A"));
    REQUIRE((groups.collides_with("A") & groups.category("B")) == 0u);
    REQUIRE((groups.collides_with("A") & groups.category("Default")) != 0u);
}

TEST_CASE("CG3 a Whitelist group touches only what it is set to, including groups made later",
          "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("Ghost", Type::Whitelist));
    REQUIRE_FALSE(groups.collidable("Ghost", "Default"));
    REQUIRE_FALSE(groups.set_collidable("Ghost", "Default", true));
    REQUIRE(groups.collidable("Ghost", "Default"));
    REQUIRE_FALSE(groups.register_group("Later", Type::Blacklist));
    REQUIRE_FALSE(groups.collidable("Ghost", "Later"));
    REQUIRE_FALSE(groups.collidable("Ghost", "Ghost"));
}

TEST_CASE("CG4 a type change keeps explicit settings; lists are derived", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", Type::Blacklist));
    REQUIRE_FALSE(groups.register_group("B", Type::Blacklist));
    REQUIRE_FALSE(groups.register_group("C", Type::Blacklist));
    REQUIRE_FALSE(groups.set_collidable("A", "B", false));
    REQUIRE_FALSE(groups.set_collidable("A", "C", true));
    auto list_of = [&](const char* name) {
        for (const CollisionGroups::Info& info : groups.groups()) {
            if (info.name == name) {
                return info.list;
            }
        }
        return std::vector<std::string>{"<missing>"};
    };
    REQUIRE(list_of("A") == std::vector<std::string>{"B"});
    REQUIRE_FALSE(groups.set_type("A", Type::Whitelist));
    REQUIRE_FALSE(groups.collidable("A", "B"));
    REQUIRE(groups.collidable("A", "C"));
    REQUIRE_FALSE(groups.collidable("A", "Default"));
    REQUIRE(list_of("A") == std::vector<std::string>{"C"});
    const std::vector<CollisionGroups::Info> all = groups.groups();
    REQUIRE(all.size() == 4);
    REQUIRE(all[0].name == "Default");
    REQUIRE(all[3].name == "C");
}

TEST_CASE("CG5 refusals change nothing", "[collision-group]") {
    CollisionGroups groups;
    const std::uint64_t before = groups.revision();
    REQUIRE(groups.register_group("", Type::Blacklist) == std::string("Collision group name must not be empty"));
    REQUIRE(groups.unregister_group("Default") == std::string("Default cannot be unregistered"));
    REQUIRE(groups.set_type("X", Type::Whitelist) ==
            std::string("Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(groups.set_collidable("Default", "X", false) ==
            std::string("Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(groups.revision() == before);
    for (int i = 1; i < CollisionGroups::kMaxGroups; ++i) {
        REQUIRE_FALSE(groups.register_group("G" + std::to_string(i), Type::Blacklist));
    }
    REQUIRE(groups.register_group("One too many", Type::Blacklist) ==
            std::string("Too many collision groups (the most is 62)"));
    // Registering an existing name again changes nothing, not even its type.
    REQUIRE_FALSE(groups.register_group("G1", Type::Whitelist));
    REQUIRE(groups.collidable("G1", "Default"));
}

TEST_CASE("CG6 unregistering frees the bit and its settings; reset leaves Default", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", Type::Blacklist));
    const std::uint64_t a_bit = groups.category("A");
    REQUIRE_FALSE(groups.set_collidable("A", "Default", false));
    REQUIRE_FALSE(groups.unregister_group("A"));
    REQUIRE_FALSE(groups.registered("A"));
    REQUIRE(groups.category("A") == 1u);
    REQUIRE_FALSE(groups.register_group("B", Type::Blacklist));
    REQUIRE(groups.category("B") == a_bit);
    REQUIRE(groups.collidable("B", "Default"));
    const std::uint64_t before = groups.revision();
    groups.reset();
    REQUIRE(groups.revision() != before);
    REQUIRE(groups.groups().size() == 1);
}
```

- [ ] **Step 2: Add the test file to CMake and run it to verify it fails**

Add `sandbox/collision_group_tests.cpp` after `sandbox/assembly_play_tests.cpp` in the sandbox list, and `src/engine_core/CollisionGroups.cpp` after `src/engine_core/AssemblyPlay.cpp`. Create an empty `CollisionGroups.cpp` (`#include "CollisionGroups.hpp"`) and a header with the class declaration from Step 3 so the test compiles; build `sandbox`.
Run: `build/Release/sandbox.exe "[collision-group]"`
Expected: FAIL to link (unresolved `CollisionGroups::...`) — the declarations exist, the definitions do not.

- [ ] **Step 3: Write the implementation**

`src/engine_core/CollisionGroups.hpp`:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {

// The collision groups PhysicsService keeps while the place runs
// (docs/superpowers/specs/2026-10-11-collision-groups-design.md): each
// registered group's type and filter bit, and the explicit pair settings.
// Pure data: PhysicsWorld turns it into Box3D filter bits. Not saved;
// DataModel resets it at Play and Stop. Simulation thread, under the
// DataModel lock.
class CollisionGroups {
public:
    // Blacklist: collides with every group but those set not to. Whitelist:
    // with none but those set to.
    enum class Type { Blacklist, Whitelist };
    struct Info {
        std::string name;
        Type type = Type::Blacklist;
        // The exceptions to its type: for a Blacklist group, the groups it is
        // set not to collide with; for a Whitelist group, those it is set to.
        std::vector<std::string> list;
    };

    static constexpr int kMaxGroups = 62;
    static constexpr std::uint64_t kGroundBit = std::uint64_t{1} << 62;
    static constexpr std::uint64_t kRayBit = std::uint64_t{1} << 63;
    static constexpr const char* kDefault = "Default";

    CollisionGroups();

    // Each returns why it refused, changing nothing. Registering a name that
    // is registered already changes nothing, not even its type.
    std::optional<std::string> register_group(const std::string& name, Type type);
    std::optional<std::string> unregister_group(const std::string& name);
    std::optional<std::string> set_type(const std::string& name, Type type);
    std::optional<std::string> set_collidable(const std::string& a, const std::string& b, bool collidable);

    bool registered(std::string_view name) const;
    // An explicit setting decides; else true only when both are Blacklist.
    // An unregistered name counts as Default.
    bool collidable(std::string_view a, std::string_view b) const;
    // In registration order, Default first.
    std::vector<Info> groups() const;
    // Default alone, Blacklist, with no explicit settings.
    void reset();

    // Filter bits. An unregistered name counts as Default.
    std::uint64_t category(std::string_view name) const;
    // The category bits of every group name collides with.
    std::uint64_t collides_with(std::string_view name) const;

    // Moves on every change, reset included.
    std::uint64_t revision() const { return revision_; }

private:
    struct Slot {
        std::string name;  // empty: free
        Type type = Type::Blacklist;
        std::uint64_t order = 0;
    };
    int bit_of(std::string_view name) const;
    int resolved(std::string_view name) const;
    bool pair(int a, int b) const;
    void changed();

    std::array<Slot, kMaxGroups> slots_{};
    // Per bit: the bits with an explicit setting, and those settings' values.
    std::array<std::uint64_t, kMaxGroups> explicit_{};
    std::array<std::uint64_t, kMaxGroups> value_{};
    // Per bit: collides_with, kept current by changed().
    std::array<std::uint64_t, kMaxGroups> mask_{};
    std::uint64_t next_order_ = 0;
    std::uint64_t revision_ = 0;
};

}  // namespace engine_core
```

`src/engine_core/CollisionGroups.cpp`:

```cpp
#include "CollisionGroups.hpp"

#include <algorithm>

namespace engine_core {
namespace {

std::uint64_t bit(int index) { return std::uint64_t{1} << index; }

std::string unknown(const std::string& name) {
    return "Unknown collision group \"" + name + "\"; did you forget to call RegisterCollisionGroup?";
}

}  // namespace

CollisionGroups::CollisionGroups() { reset(); }

void CollisionGroups::reset() {
    slots_ = {};
    explicit_ = {};
    value_ = {};
    next_order_ = 0;
    slots_[0].name = kDefault;
    slots_[0].order = next_order_++;
    changed();
}

int CollisionGroups::bit_of(std::string_view name) const {
    if (name.empty()) {
        return -1;
    }
    for (int i = 0; i < kMaxGroups; ++i) {
        if (slots_[static_cast<std::size_t>(i)].name == name) {
            return i;
        }
    }
    return -1;
}

int CollisionGroups::resolved(std::string_view name) const {
    const int found = bit_of(name);
    return found >= 0 ? found : 0;
}

bool CollisionGroups::pair(int a, int b) const {
    const std::size_t ia = static_cast<std::size_t>(a);
    if ((explicit_[ia] & bit(b)) != 0) {
        return (value_[ia] & bit(b)) != 0;
    }
    return slots_[ia].type == Type::Blacklist && slots_[static_cast<std::size_t>(b)].type == Type::Blacklist;
}

void CollisionGroups::changed() {
    for (int i = 0; i < kMaxGroups; ++i) {
        std::uint64_t mask = 0;
        if (!slots_[static_cast<std::size_t>(i)].name.empty()) {
            for (int j = 0; j < kMaxGroups; ++j) {
                if (!slots_[static_cast<std::size_t>(j)].name.empty() && pair(i, j)) {
                    mask |= bit(j);
                }
            }
        }
        mask_[static_cast<std::size_t>(i)] = mask;
    }
    ++revision_;
}

std::optional<std::string> CollisionGroups::register_group(const std::string& name, Type type) {
    if (name.empty()) {
        return std::string("Collision group name must not be empty");
    }
    if (bit_of(name) >= 0) {
        return std::nullopt;
    }
    int free = -1;
    for (int i = 0; i < kMaxGroups && free < 0; ++i) {
        if (slots_[static_cast<std::size_t>(i)].name.empty()) {
            free = i;
        }
    }
    if (free < 0) {
        return std::string("Too many collision groups (the most is 62)");
    }
    const std::size_t slot = static_cast<std::size_t>(free);
    slots_[slot].name = name;
    slots_[slot].type = type;
    slots_[slot].order = next_order_++;
    explicit_[slot] = 0;
    value_[slot] = 0;
    for (int i = 0; i < kMaxGroups; ++i) {
        explicit_[static_cast<std::size_t>(i)] &= ~bit(free);
        value_[static_cast<std::size_t>(i)] &= ~bit(free);
    }
    changed();
    return std::nullopt;
}

std::optional<std::string> CollisionGroups::unregister_group(const std::string& name) {
    if (name == kDefault) {
        return std::string("Default cannot be unregistered");
    }
    const int found = bit_of(name);
    if (found < 0) {
        return std::nullopt;
    }
    slots_[static_cast<std::size_t>(found)] = Slot{};
    explicit_[static_cast<std::size_t>(found)] = 0;
    value_[static_cast<std::size_t>(found)] = 0;
    for (int i = 0; i < kMaxGroups; ++i) {
        explicit_[static_cast<std::size_t>(i)] &= ~bit(found);
        value_[static_cast<std::size_t>(i)] &= ~bit(found);
    }
    changed();
    return std::nullopt;
}

std::optional<std::string> CollisionGroups::set_type(const std::string& name, Type type) {
    const int found = bit_of(name);
    if (found < 0) {
        return unknown(name);
    }
    slots_[static_cast<std::size_t>(found)].type = type;
    changed();
    return std::nullopt;
}

std::optional<std::string> CollisionGroups::set_collidable(const std::string& a, const std::string& b,
                                                           bool collidable) {
    const int ia = bit_of(a);
    if (ia < 0) {
        return unknown(a);
    }
    const int ib = bit_of(b);
    if (ib < 0) {
        return unknown(b);
    }
    for (const auto& [from, to] : {std::pair<int, int>{ia, ib}, std::pair<int, int>{ib, ia}}) {
        explicit_[static_cast<std::size_t>(from)] |= bit(to);
        if (collidable) {
            value_[static_cast<std::size_t>(from)] |= bit(to);
        } else {
            value_[static_cast<std::size_t>(from)] &= ~bit(to);
        }
    }
    changed();
    return std::nullopt;
}

bool CollisionGroups::registered(std::string_view name) const { return bit_of(name) >= 0; }

bool CollisionGroups::collidable(std::string_view a, std::string_view b) const {
    return pair(resolved(a), resolved(b));
}

std::vector<CollisionGroups::Info> CollisionGroups::groups() const {
    std::vector<int> order;
    for (int i = 0; i < kMaxGroups; ++i) {
        if (!slots_[static_cast<std::size_t>(i)].name.empty()) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return slots_[static_cast<std::size_t>(a)].order < slots_[static_cast<std::size_t>(b)].order;
    });
    std::vector<Info> out;
    for (const int i : order) {
        const Slot& slot = slots_[static_cast<std::size_t>(i)];
        Info info;
        info.name = slot.name;
        info.type = slot.type;
        for (const int j : order) {
            const bool set = (explicit_[static_cast<std::size_t>(i)] & bit(j)) != 0;
            const bool value = (value_[static_cast<std::size_t>(i)] & bit(j)) != 0;
            if (set && value == (slot.type == Type::Whitelist)) {
                info.list.push_back(slots_[static_cast<std::size_t>(j)].name);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::uint64_t CollisionGroups::category(std::string_view name) const { return bit(resolved(name)); }

std::uint64_t CollisionGroups::collides_with(std::string_view name) const {
    return mask_[static_cast<std::size_t>(resolved(name))];
}

}  // namespace engine_core
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/Release/sandbox.exe "[collision-group]"`
Expected: PASS, 6 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/CollisionGroups.hpp src/engine_core/CollisionGroups.cpp sandbox/collision_group_tests.cpp CMakeLists.txt
git commit -m "Collision groups: the registry, with Whitelist and Blacklist types"
```

---

### Task 2: `CollisionGroup` on PhysicsBase, and the DataModel's registry

**Files:**
- Modify: `src/engine_instances/PhysicsBase.hpp`, `src/engine_instances/PhysicsBase.cpp`
- Modify: `src/engine_core/DataModel.hpp` (accessor beside `assemblies()`), `src/engine_core/DataModel.cpp`, `src/engine_core/DataModelState.hpp` (member beside `Assemblies assemblies;`), `src/engine_core/DataModelPlace.cpp` (reset in `start_simulation`, `stop_simulation`)
- Modify: `src/engine_core/LuaApi.cpp` (doc beside `add("PhysicsBase", "CanCollide", ...)`)
- Test: `sandbox/physics_tests.cpp` (P43), `tests/PropertiesTest.cpp` (TestClassGroups)

**Interfaces:**
- Consumes: Task 1's `CollisionGroups`.
- Produces: `PhysicsBase::collision_group() const -> const std::string&`, `PhysicsBase::set_collision_group(const std::string&) -> std::optional<std::string>` (marks `kDirtyFilter`); `DataModel::collision_groups() const -> CollisionGroups&`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/physics_tests.cpp`:

```cpp
TEST_CASE("P43 every PhysicsBase has CollisionGroup, saved, Default; Play and Stop clear the groups",
          "[physics][collision-group]") {
    for (const char* name : {"PhysicsBase", "PhysicsObject", "PlayerController", "Brush"}) {
        INFO(name);
        REQUIRE(engine_core::lua_class_find(name, "CollisionGroup") != nullptr);
    }
    SimRole role;
    engine_core::Game game;
    PhysicsObject& body = game.create<PhysicsObject>();
    REQUIRE(body.collision_group() == "Default");
    REQUIRE_FALSE(body.set_collision_group("Crates"));
    engine_core::PropertyBag saved;
    body.save_properties(saved);
    const engine_core::JsonValue* value = engine_core::bag_find(saved, "CollisionGroup");
    REQUIRE(value != nullptr);
    REQUIRE(value->as_string() == "Crates");

    REQUIRE_FALSE(game.collision_groups().register_group("Crates", engine_core::CollisionGroups::Type::Whitelist));
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(game.collision_groups().registered("Crates"));
    REQUIRE_FALSE(game.collision_groups().register_group("Crates", engine_core::CollisionGroups::Type::Whitelist));
    game.stop_simulation();
    REQUIRE_FALSE(game.collision_groups().registered("Crates"));
}
```

In `tests/PropertiesTest.cpp` `TestClassGroups`, beside the CanCollide-era rows added for GameObject:

```cpp
    for (const std::vector<std::string>& sheet : {brush, player, body}) {
        Expect(Has(sheet, "Physics:CollisionGroup"), "every body has CollisionGroup, in Physics");
    }
```

(`player` and `body` are the PlayerController and PhysicsObject sheets already made there.)

- [ ] **Step 2: Run to verify they fail**

Run: build `sandbox`; `build/Release/sandbox.exe "P43*"`
Expected: compile failure on `collision_group` / `collision_groups` (members do not exist yet). Add the declarations of Step 3 with empty bodies returning defaults first if a behavioral RED is wanted; then expected FAIL at the `lua_class_find` check.

- [ ] **Step 3: Implement**

`PhysicsBase.hpp`: in the class comment's property table add

```
// CollisionGroup   string    "Default". Which PhysicsService collision group
//                            it is in; an unregistered name behaves as Default.
```

and members:

```cpp
    const std::string& collision_group() const { return collision_group_; }
    std::optional<std::string> set_collision_group(const std::string& group);
    ...
    std::string collision_group_ = "Default";
```

`PhysicsBase.cpp`:

```cpp
std::optional<std::string> PhysicsBase::set_collision_group(const std::string& group) {
    require_gameplay_thread(*this);
    if (group == collision_group_) {
        return std::nullopt;
    }
    LuaSlot previous;
    previous.kind = LuaSlot::Kind::String;
    previous.text = collision_group_;
    collision_group_ = group;
    dirty_ |= kDirtyFilter;
    LuaSlot next;
    next.kind = LuaSlot::Kind::String;
    next.text = group;
    note_property_change("CollisionGroup", previous, next);
    return std::nullopt;
}
```

`on_reuse`: `collision_group_ = "Default";`. Lua helpers beside `read_can_collide`:

```cpp
bool read_collision_group(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = body->collision_group();
    return true;
}

bool write_collision_group(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    return body != nullptr && refuse(in, body->set_collision_group(in.text));
}
```

Registration, after CanCollide: `lua_saved_property("CollisionGroup", "string", read_collision_group, write_collision_group, "\"Default\""),`.

`DataModelState.hpp`: `#include "CollisionGroups.hpp"` and, after `Assemblies assemblies;`:

```cpp
    // PhysicsService's collision groups; DataModel::collision_groups. Not saved.
    CollisionGroups collision_groups;
```

`DataModel.hpp` beside `assemblies()`: `CollisionGroups& collision_groups() const;` (forward-declare `class CollisionGroups;`). `DataModel.cpp`: `CollisionGroups& DataModel::collision_groups() const { return state_->collision_groups; }`.

`DataModelPlace.cpp`: in `start_simulation`, after `state_->assemblies.capture_play(*this);`:

```cpp
    // Each run starts with Default alone; scripts register the rest.
    state_->collision_groups.reset();
```

In `stop_simulation`, after `restore_place_unlocked();`: `state_->collision_groups.reset();`.

`LuaApi.cpp` after the PhysicsBase CanCollide doc:

```cpp
    add("PhysicsBase", "CollisionGroup",
        "Which PhysicsService collision group this body is in. A name no script has registered behaves as "
        "Default until one does.",
        "string", false, {});
```

- [ ] **Step 4: Run to verify they pass**

Run: `build/Release/sandbox.exe "P43*"`; build and run `build/Release/properties-tests.exe`.
Expected: PASS; "properties tests passed".

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances/PhysicsBase.hpp src/engine_instances/PhysicsBase.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelState.hpp src/engine_core/DataModelPlace.cpp src/engine_core/LuaApi.cpp sandbox/physics_tests.cpp tests/PropertiesTest.cpp
git commit -m "Physics: CollisionGroup on every body, and the DataModel's collision groups, cleared at Play and Stop"
```

---

### Task 3: Filter bits in the physics world

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp` (`RayFilter::collision_group`), `src/engine_core/PhysicsWorld.cpp`
- Test: `sandbox/physics_tests.cpp` (P44 to P50)

**Interfaces:**
- Consumes: `DataModel::collision_groups()`, `CollisionGroups::category/collides_with/revision/kGroundBit/kRayBit`, `PhysicsBase::collision_group()`.
- Produces: `RayFilter::collision_group` (`std::string`, empty: every group).

- [ ] **Step 1: Write the failing tests** — append to `sandbox/physics_tests.cpp`:

```cpp
namespace {

using engine_core::CollisionGroups;

void groups_apart(engine_core::DataModel& game, const char* a, const char* b) {
    REQUIRE_FALSE(game.collision_groups().register_group(a, CollisionGroups::Type::Blacklist));
    REQUIRE_FALSE(game.collision_groups().register_group(b, CollisionGroups::Type::Blacklist));
    REQUIRE_FALSE(game.collision_groups().set_collidable(a, b, false));
}

}  // namespace

TEST_CASE("P44 two boxes in groups set apart pass through each other; set back, they rest",
          "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& under = rig.body(at(0.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& over = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(under.set_collision_group("Under"));
    REQUIRE_FALSE(over.set_collision_group("Over"));
    rig.play();
    groups_apart(rig.game, "Under", "Over");
    rig.seconds(2.0);
    // Through the anchored box, onto the floor.
    REQUIRE(near(y_of(over.transform()), 0.5f, 0.05f));
    REQUIRE_FALSE(rig.game.collision_groups().set_collidable("Under", "Over", true));
    REQUIRE_FALSE(over.set_transform(at(0.f, 3.f, 0.f)));
    rig.seconds(2.0);
    REQUIRE(near(y_of(over.transform()), 1.5f, 0.05f));
}

TEST_CASE("P45 a body naming a group before it is registered is Default until it is", "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(box.set_collision_group("Ghosts"));
    rig.play();
    rig.seconds(1.5);
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
    REQUIRE_FALSE(rig.game.collision_groups().register_group("Ghosts", CollisionGroups::Type::Whitelist));
    rig.seconds(1.5);
    REQUIRE(y_of(box.transform()) < -2.f);
}

TEST_CASE("P46 changing CollisionGroup during play takes effect at the next step", "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    REQUIRE_FALSE(rig.game.collision_groups().register_group("Ghosts", CollisionGroups::Type::Whitelist));
    rig.seconds(1.5);
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
    REQUIRE_FALSE(box.set_collision_group("Ghosts"));
    rig.seconds(1.5);
    REQUIRE(y_of(box.transform()) < -2.f);
}

TEST_CASE("P47 a group apart from itself: welded members stay one body; separate ones pass through",
          "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& a = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(1.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::WeldConstraint& weld = rig.game.create<engine_core::WeldConstraint>();
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = a.id();
    REQUIRE_FALSE(weld.set_pv_instance1(slot));
    slot.id = b.id();
    REQUIRE_FALSE(weld.set_pv_instance2(slot));
    rig.game.set_parent(weld.id(), workspace_of(rig.game));
    PhysicsObject& lower = rig.body(at(5.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& upper = rig.body(at(5.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    for (PhysicsObject* body : {&a, &b, &lower, &upper}) {
        REQUIRE_FALSE(body->set_collision_group("Loners"));
    }
    rig.play();
    REQUIRE_FALSE(rig.game.collision_groups().register_group("Loners", CollisionGroups::Type::Blacklist));
    REQUIRE_FALSE(rig.game.collision_groups().set_collidable("Loners", "Loners", false));
    rig.seconds(2.0);
    REQUIRE(rig.physics.body_key(a.id()) == rig.physics.body_key(b.id()));
    REQUIRE(near(y_of(a.transform()), 0.5f, 0.05f));
    REQUIRE(near(y_of(upper.transform()), 0.5f, 0.05f));
}

TEST_CASE("P48 a welded assembly's members each collide by their own group", "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    // The bottom member passes through the floor; the top one stops on it.
    PhysicsObject& bottom = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& top = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::WeldConstraint& weld = rig.game.create<engine_core::WeldConstraint>();
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = bottom.id();
    REQUIRE_FALSE(weld.set_pv_instance1(slot));
    slot.id = top.id();
    REQUIRE_FALSE(weld.set_pv_instance2(slot));
    rig.game.set_parent(weld.id(), workspace_of(rig.game));
    REQUIRE_FALSE(bottom.set_collision_group("Ghosts"));
    rig.play();
    REQUIRE_FALSE(rig.game.collision_groups().register_group("Ghosts", CollisionGroups::Type::Whitelist));
    rig.seconds(3.0);
    REQUIRE(near(y_of(top.transform()), 0.5f, 0.05f));
    REQUIRE(near(y_of(bottom.transform()), -1.5f, 0.05f));
}

TEST_CASE("P49 a PlayerController stands only on what its group collides with", "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& platform = rig.body(at(0.f, 5.f, 0.f), Vec3{10.f, 1.f, 10.f}, true);
    REQUIRE_FALSE(platform.set_collision_group("Platforms"));
    engine_core::PlayerController& player = rig.controller(at(0.f, 6.f, 0.f));
    REQUIRE_FALSE(player.set_collision_group("Players"));
    rig.play();
    groups_apart(rig.game, "Platforms", "Players");
    rig.seconds(3.0);
    REQUIRE(y_of(player.transform()) < 1.f);
}

TEST_CASE("P50 rays: by group from the physics world, every group for Studio; Stop clears the groups",
          "[physics][collision-group]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, true);
    REQUIRE_FALSE(box.set_collision_group("Boxes"));
    REQUIRE_FALSE(box.set_can_collide(false));
    rig.play();
    groups_apart(rig.game, "Boxes", "Rays");
    engine_core::RayFilter studio;
    engine_core::RayFilter grouped;
    grouped.collision_group = "Rays";
    const auto all = rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, studio);
    REQUIRE((all && all->instance == box.id()));
    const auto by_group = rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, grouped);
    REQUIRE((by_group && by_group->instance != box.id()));
    rig.game.stop_simulation();
    const auto after = rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, grouped);
    REQUIRE((after && after->instance == box.id()));
}
```

(`#include "Constraint.hpp"` at the top of physics_tests.cpp if absent.)

- [ ] **Step 2: Run to verify they fail**

Run: build sandbox (compile error on `RayFilter::collision_group` until Step 3 adds the field: add the field alone first), then `build/Release/sandbox.exe "[collision-group]"`.
Expected: P44 to P50 FAIL (groups ignored); CG1 to CG6 and P43 pass.

- [ ] **Step 3: Implement** in `PhysicsWorld.cpp` (and the field in `PhysicsWorld.hpp`):

`PhysicsWorld.hpp` `RayFilter`:

```cpp
    // Which collision group the ray is in: it hits only bodies whose group
    // collides with it. Empty: every group (the studio's own rays).
    std::string collision_group;
```

`PhysicsWorld.cpp`: `#include "CollisionGroups.hpp"`. Replace the CanCollide block (`kSolidBits`, `kGhostBits`, `kQueryBits`, `shape_filter(bool)`, `ray_filter()`, `ground_filter()`) with:

```cpp
// Filter bits (docs/superpowers/specs/2026-10-11-collision-groups-design.md
// §4). A shape's category is its body's group's bit; its mask, every group it
// collides with, plus the ground and ray query bits. CanCollide false: the
// ray bit alone, so it touches nothing, whatever its group.
b3Filter shape_filter(const CollisionGroups& groups, std::string_view group, bool can_collide) {
    b3Filter filter = b3DefaultFilter();
    filter.categoryBits = groups.category(group);
    filter.maskBits = can_collide
                          ? groups.collides_with(group) | CollisionGroups::kGroundBit | CollisionGroups::kRayBit
                          : CollisionGroups::kRayBit;
    return filter;
}

// A ray in group (empty: every group) hits what that group collides with,
// CanCollide or not.
b3QueryFilter ray_filter(const CollisionGroups& groups, const std::string& group) {
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = CollisionGroups::kRayBit;
    filter.maskBits = group.empty() ? B3_DEFAULT_MASK_BITS : groups.collides_with(group);
    return filter;
}

// A PlayerController's ground: what can collide and its group collides with.
b3QueryFilter ground_filter(const CollisionGroups& groups, const std::string& group) {
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = CollisionGroups::kGroundBit;
    filter.maskBits = groups.collides_with(group);
    return filter;
}
```

In `Impl`: members

```cpp
    // The DataModel's collision groups, set at each sync, and the revision
    // whose filters the shapes have.
    const CollisionGroups* groups = nullptr;
    std::uint64_t groups_seen = ~std::uint64_t{0};
```

and a member function replacing the static `set_filters`:

```cpp
    b3Filter filter_of(const PhysicsBase& object) const {
        return shape_filter(*groups, object.collision_group(), object.can_collide());
    }

    // CanCollide or CollisionGroup changed: the shapes stay; only what they
    // touch changes. Contacts are made again, so a pair that stopped
    // colliding lets go at once.
    void set_filters(const PhysicsBase& object, const Body& record) {
        const b3Filter filter = filter_of(object);
        for (const b3ShapeId shape : record.shapes) {
            b3Shape_SetFilter(shape, filter, true);
        }
    }

    // The groups changed: every body's shapes and every terrain chunk.
    void refilter_all(DataModel& game) {
        for (auto& [id, record] : bodies) {
            if (const auto* object = dynamic_cast<const PhysicsBase*>(game.instance(id))) {
                set_filters(*object, record);
            }
        }
        const b3Filter terrain = shape_filter(*groups, CollisionGroups::kDefault, true);
        for (auto& [id, body] : terrain_bodies) {
            for (auto& [coord, chunk] : body.chunks) {
                if (b3Shape_IsValid(chunk.shape)) {
                    b3Shape_SetFilter(chunk.shape, terrain, true);
                }
            }
        }
    }
```

In `sync`, first lines:

```cpp
        groups = &game.collision_groups();
        if (!b3World_IsValid(world) || generation != game.world_generation()) {
            begin(game.world_generation());
        }
        if (groups->revision() != groups_seen) {
            groups_seen = groups->revision();
            refilter_all(game);
        }
```

(and in `begin`, reset `groups_seen = ~std::uint64_t{0};` is unnecessary: a new world has no shapes.)

Shape defs: terrain `def.filter = shape_filter(*groups, CollisionGroups::kDefault, true);`; Brush `def.filter = filter_of(brush);`; PlayerController `def.filter = filter_of(controller);`; PhysicsObject `def.filter = filter_of(object);`. Probe: both `ground_filter()` calls become `ground_filter(*groups, controller.collision_group())`. Raycast: `ray_filter(*groups, filter.collision_group)`. The two `set_filters(object, record)` call sites keep their form (now the member).

- [ ] **Step 4: Run to verify they pass**

Run: `build/Release/sandbox.exe "[collision-group]"`, then `build/Release/sandbox.exe "[cancollide],BP2*"`.
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp sandbox/physics_tests.cpp
git commit -m "Physics: collision groups as Box3D filter bits, for welds, controllers, and rays"
```

---

### Task 4: `PhysicsService` and `RaycastParams.CollisionGroup` in Lua

**Files:**
- Create: `src/engine_core/PhysicsServiceBindings.cpp`
- Modify: `src/engine_core/ScriptBindings.hpp` (`kServiceClasses` append `"PhysicsService"`; declare `static void link_physics_service_methods();`), `src/engine_core/ScriptBindings.cpp` (call it beside `link_wireframe_methods()`), `src/engine_core/RaycastBindings.cpp`, `src/engine_core/LuaApi.cpp` (docs), `CMakeLists.txt` (engine_core source; sandbox test)
- Create: `sandbox/collision_group_lua_tests.cpp`

**Interfaces:**
- Consumes: `DataModel::collision_groups()`, `RayFilter::collision_group`.
- Produces: Lua `game:GetService("PhysicsService")` with the spec §3 methods; `RaycastParams.CollisionGroup`.

- [ ] **Step 1: Write the failing tests** — `sandbox/collision_group_lua_tests.cpp`:

```cpp
// PhysicsService's collision groups and RaycastParams.CollisionGroup, from Luau.

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

struct GroupsRig : ScriptRig {
    engine_core::PhysicsWorld physics;
    GroupsRig() { game.set_physics(&physics); }
    ~GroupsRig() { game.set_physics(nullptr); }

    std::string run(const char* source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        frames(1);
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }
};

}  // namespace

TEST_CASE("CGL1 PhysicsService registers groups and answers whether they collide", "[collision-group][lua]") {
    GroupsRig rig;
    const std::string out = rig.run(R"(
        local P = game:GetService("PhysicsService")
        P:RegisterCollisionGroup("A")
        P:RegisterCollisionGroup("W", "Whitelist")
        print(P:IsCollisionGroupRegistered("A"), P:IsCollisionGroupRegistered("Nope"), P:GetMaxCollisionGroups())
        print(P:CollisionGroupsAreCollidable("A", "Default"), P:CollisionGroupsAreCollidable("W", "A"))
        P:CollisionGroupSetCollidableBulk("W", { "A", "Default" }, true)
        print(P:CollisionGroupsAreCollidable("W", "A"), P:CollisionGroupsAreCollidable("W", "Default"))
        P:SetCollisionGroupCollisionType("A", "Whitelist")
        local groups = P:GetRegisteredCollisionGroups()
        print(#groups, groups[1].name, groups[3].name, groups[3].type, table.concat(groups[3].list, ","))
        P:UnregisterCollisionGroup("A")
        print(P:IsCollisionGroupRegistered("A"))
    )");
    INFO(out);
    REQUIRE(has(out, "true\tfalse\t62\n"));
    REQUIRE(has(out, "true\tfalse\n"));
    REQUIRE(has(out, "true\ttrue\n"));
    REQUIRE(has(out, "3\tDefault\tW\tWhitelist\tA,Default\n"));
    REQUIRE(has(out, "false\n"));
}

TEST_CASE("CGL2 PhysicsService raises on bad calls", "[collision-group][lua]") {
    GroupsRig rig;
    const std::string out = rig.run(R"(
        local P = game:GetService("PhysicsService")
        print(pcall(function() P:SetCollisionGroupCollisionType("X", "Whitelist") end))
        print(pcall(function() P:RegisterCollisionGroup("Y", "Greylist") end))
        print(pcall(function() P:UnregisterCollisionGroup("Default") end))
    )");
    INFO(out);
    REQUIRE(has(out, "Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(has(out, "collisionType must be \"Blacklist\" or \"Whitelist\""));
    REQUIRE(has(out, "Default cannot be unregistered"));
}

TEST_CASE("CGL3 RaycastParams.CollisionGroup filters what a ray hits", "[collision-group][lua][raycast]") {
    GroupsRig rig;
    PhysicsObject& floor = rig.game.create<PhysicsObject>();
    REQUIRE_FALSE(floor.set_transform(at(0.f, -0.5f, 0.f)));
    REQUIRE_FALSE(floor.set_size(Vec3{40.f, 1.f, 40.f}));
    floor.set_anchored(true);
    rig.game.set_name(floor.id(), "Floor");
    rig.game.set_parent(floor.id(), workspace_of(rig.game));
    PhysicsObject& box = rig.game.create<PhysicsObject>();
    REQUIRE_FALSE(box.set_transform(at(0.f, 3.f, 0.f)));
    REQUIRE_FALSE(box.set_size(Vec3{2.f, 2.f, 2.f}));
    box.set_anchored(true);
    REQUIRE_FALSE(box.set_collision_group("Boxes"));
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    const std::string out = rig.run(R"(
        local P = game:GetService("PhysicsService")
        P:RegisterCollisionGroup("Boxes")
        P:RegisterCollisionGroup("Rays")
        P:CollisionGroupSetCollidable("Boxes", "Rays", false)
        local p = RaycastParams.new()
        print(p.CollisionGroup)
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
        p.CollisionGroup = "Rays"
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
    )");
    INFO(out);
    REQUIRE(has(out, "Default\nBox\nFloor\n"));
}
```

Add `sandbox/collision_group_lua_tests.cpp` to the sandbox sources.

- [ ] **Step 2: Run to verify they fail**

Run: build sandbox; `build/Release/sandbox.exe "[collision-group][lua]"`
Expected: FAIL ("unknown service" from GetService; "CollisionGroup is not a valid member of RaycastParams").

- [ ] **Step 3: Implement**

`ScriptBindings.hpp`: `kServiceClasses` becomes `{"RunService", "Selection", "UserInputService", "ChangeHistoryService", "PhysicsService"}` (appended: earlier kinds keep their numbers); declare `static void link_physics_service_methods();` beside `link_camera_methods`. `ScriptBindings.cpp`: `ScriptBindings::link_physics_service_methods();` after `link_wireframe_methods();`.

`src/engine_core/PhysicsServiceBindings.cpp`:

```cpp
// PhysicsService: the collision groups scripts register while the place runs
// (docs/superpowers/specs/2026-10-11-collision-groups-design.md §3). The
// registry is the DataModel's (CollisionGroups); this is its Lua face.

#include "ScriptBindings.hpp"

#include "CollisionGroups.hpp"
#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"

#include "lua.h"
#include "lualib.h"

#include <cstring>
#include <optional>
#include <string>

namespace engine_core {

using namespace script_internal;

namespace {

CollisionGroups& groups_of(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    ScriptRuntime* runtime = runtime_from(state);
    if (runtime == nullptr || runtime->game_ == nullptr) {
        luaL_error(state, "PhysicsService is not available");
    }
    return runtime->game_->collision_groups();
}

CollisionGroups::Type check_type(lua_State* state, int index, bool optional) {
    if (optional && lua_isnoneornil(state, index)) {
        return CollisionGroups::Type::Blacklist;
    }
    const char* text = luaL_checkstring(state, index);
    if (std::strcmp(text, "Blacklist") == 0) {
        return CollisionGroups::Type::Blacklist;
    }
    if (std::strcmp(text, "Whitelist") == 0) {
        return CollisionGroups::Type::Whitelist;
    }
    luaL_error(state, "collisionType must be \"Blacklist\" or \"Whitelist\"");
}

const char* type_name(CollisionGroups::Type type) {
    return type == CollisionGroups::Type::Whitelist ? "Whitelist" : "Blacklist";
}

void raise(lua_State* state, const std::optional<std::string>& refused) {
    if (refused) {
        luaL_error(state, "%s", refused->c_str());
    }
}

int register_group(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string name = luaL_checkstring(state, 2);
        raise(state, groups.register_group(name, check_type(state, 3, true)));
        return 0;
    });
}

int unregister_group(lua_State* state) {
    return lua_guard(state, [&] {
        raise(state, groups_of(state).unregister_group(luaL_checkstring(state, 2)));
        return 0;
    });
}

int set_type(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string name = luaL_checkstring(state, 2);
        raise(state, groups.set_type(name, check_type(state, 3, false)));
        return 0;
    });
}

int set_collidable(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string a = luaL_checkstring(state, 2);
        const std::string b = luaL_checkstring(state, 3);
        luaL_checktype(state, 4, LUA_TBOOLEAN);
        raise(state, groups.set_collidable(a, b, lua_toboolean(state, 4) != 0));
        return 0;
    });
}

int set_collidable_bulk(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string name = luaL_checkstring(state, 2);
        luaL_checktype(state, 3, LUA_TTABLE);
        luaL_checktype(state, 4, LUA_TBOOLEAN);
        const bool collidable = lua_toboolean(state, 4) != 0;
        const int count = lua_objlen(state, 3);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, 3, i);
            const std::string other = luaL_checkstring(state, -1);
            lua_pop(state, 1);
            raise(state, groups.set_collidable(name, other, collidable));
        }
        return 0;
    });
}

int are_collidable(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        lua_pushboolean(state, groups.collidable(luaL_checkstring(state, 2), luaL_checkstring(state, 3)) ? 1 : 0);
        return 1;
    });
}

int is_registered(lua_State* state) {
    return lua_guard(state, [&] {
        lua_pushboolean(state, groups_of(state).registered(luaL_checkstring(state, 2)) ? 1 : 0);
        return 1;
    });
}

int registered_groups(lua_State* state) {
    return lua_guard(state, [&] {
        const std::vector<CollisionGroups::Info> groups = groups_of(state).groups();
        lua_createtable(state, static_cast<int>(groups.size()), 0);
        int index = 1;
        for (const CollisionGroups::Info& info : groups) {
            lua_createtable(state, 0, 3);
            lua_pushlstring(state, info.name.data(), info.name.size());
            lua_setfield(state, -2, "name");
            lua_pushstring(state, type_name(info.type));
            lua_setfield(state, -2, "type");
            lua_createtable(state, static_cast<int>(info.list.size()), 0);
            int at = 1;
            for (const std::string& other : info.list) {
                lua_pushlstring(state, other.data(), other.size());
                lua_rawseti(state, -2, at++);
            }
            lua_setfield(state, -2, "list");
            lua_rawseti(state, -2, index++);
        }
        return 1;
    });
}

int max_groups(lua_State* state) {
    return lua_guard(state, [&] {
        groups_of(state);
        lua_pushnumber(state, CollisionGroups::kMaxGroups);
        return 1;
    });
}

ANARCHY_LUA_REGISTER(register_physics_service_lua) {
    const LuaField methods[] = {
        lua_method("RegisterCollisionGroup", "nil", reinterpret_cast<void*>(&register_group)),
        lua_method("UnregisterCollisionGroup", "nil", reinterpret_cast<void*>(&unregister_group)),
        lua_method("SetCollisionGroupCollisionType", "nil", reinterpret_cast<void*>(&set_type)),
        lua_method("CollisionGroupSetCollidable", "nil", reinterpret_cast<void*>(&set_collidable)),
        lua_method("CollisionGroupSetCollidableBulk", "nil", reinterpret_cast<void*>(&set_collidable_bulk)),
        lua_method("CollisionGroupsAreCollidable", "boolean", reinterpret_cast<void*>(&are_collidable)),
        lua_method("IsCollisionGroupRegistered", "boolean", reinterpret_cast<void*>(&is_registered)),
        lua_method("GetRegisteredCollisionGroups", "table", reinterpret_cast<void*>(&registered_groups)),
        lua_method("GetMaxCollisionGroups", "number", reinterpret_cast<void*>(&max_groups)),
    };
    register_lua_class("PhysicsService", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
    register_lua_service("PhysicsService");
}

}  // namespace

void ScriptBindings::link_physics_service_methods() {}

}  // namespace engine_core
```

`RaycastBindings.cpp`: `RaycastParamsUd` gains `std::string collision_group = "Default";`. `params_newindex`: before the error,

```cpp
    if (std::strcmp(key, "CollisionGroup") == 0) {
        params->collision_group = luaL_checkstring(state, 3);
        return 0;
    }
```

`raycast_params_index`: before the error,

```cpp
    if (std::strcmp(key, "CollisionGroup") == 0) {
        lua_pushlstring(state, params->collision_group.data(), params->collision_group.size());
        return 1;
    }
```

`workspace_raycast`: `filter.collision_group = "Default";` after `RayFilter filter;`, and inside the params branch `filter.collision_group = params->collision_group;`. Registration: `lua_property("CollisionGroup", "string", true, nullptr, nullptr),` in the RaycastParams fields.

`LuaApi.cpp`: one `add("PhysicsService", <method>, ...)` per method in spec §3 (params with `P(...)`, return types as registered), and

```cpp
    add("RaycastParams", "CollisionGroup",
        "The ray's collision group: it hits only bodies whose group collides with this one. \"Default\" at first.",
        "string", false, {});
```

- [ ] **Step 4: Run to verify they pass**

Run: `build/Release/sandbox.exe "[collision-group]"`, `build/Release/sandbox.exe "[raycast]"`; build and run `build/Release/engine-tests.exe` (Luau completion lists services).
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsServiceBindings.cpp src/engine_core/ScriptBindings.hpp src/engine_core/ScriptBindings.cpp src/engine_core/RaycastBindings.cpp src/engine_core/LuaApi.cpp sandbox/collision_group_lua_tests.cpp CMakeLists.txt
git commit -m "PhysicsService: collision groups from scripts, and RaycastParams.CollisionGroup"
```

---

### Task 5: Full verification

- [ ] **Step 1:** Release `sandbox`, whole run: `build/Release/sandbox.exe`. Expected: all pass (A34 "did not settle" is a known timing flake: rerun it alone).
- [ ] **Step 2:** Build and run `properties-tests`, `engine-tests`, `studio-tests`, `mcp-tests`. Expected: pass. (explorer-tests has 6 failures on main that predate this work; note, do not fix.)
- [ ] **Step 3:** Build Release `AnarchyStudio` and `AnarchyPlayer`.
- [ ] **Step 4:** Update `src/engine_instances/README.md`'s PhysicsBase paragraph: "holding Transform, Velocity, Anchored, CanCollide, CollisionGroup, Mass, and LinearDamping", and one sentence that groups live in PhysicsService (CollisionGroups.hpp). Commit `"Docs: CollisionGroup and PhysicsService in the instances README"`.
