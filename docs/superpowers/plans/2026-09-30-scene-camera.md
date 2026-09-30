# Scene Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fly the scene view's camera in edit mode, Roblox Studio style: WASD relative to the camera, Q/E down/up, right mouse button held locks the pointer and turns the camera. The camera control is a built-in Luau plugin.

**Architecture:** The engine gains the Roblox API the plugin needs: `Workspace.CurrentCamera`, `UserInputService.MouseBehavior` / `MouseDeltaSensitivity` / `GetMouseDelta()`, `RunService:IsRunning()`, and UserInputService input in edit mode. JadeFX gains a Scene pointer lock carried out by its GLFW host. The IDE loads `resources/plugins/*.luau` into the plugin VM and `GameView` drives `CurrentCamera` and the pointer lock.

**Tech Stack:** C++17 (MSVC on Windows), Luau, GLFW via JadeFX (sibling repo `../JadeFX_CPP`), Catch2 (`sandbox` target), JadeFX's own `jadefx-tests`.

**Spec:** `docs/superpowers/specs/2026-09-30-scene-camera-design.md`

## Global Constraints

- Roblox values: `Enum.MouseBehavior` is `Default` = 0, `LockCenter` = 1, `LockCurrentPosition` = 2.
- `MouseDeltaSensitivity` defaults to 1, is clamped to ≥ 0; a non-finite write is refused.
- `Workspace.CurrentCamera` is not saved, not undoable, and does not mark the place changed.
- A Camera's Transform writes are never undo steps, but still mark the place changed.
- Built-in plugin Scripts are unparented, not saved, not in the explorer, and not undo steps.
- The camera plugin does nothing while `RunService:IsRunning()` is true (play, including paused play).
- Code style: match the surrounding files: comment density and prose comments, `snake_case` in engine code, `camelCase` in `runner/`, `ide/` panes, and JadeFX.
- Before executing: the working tree on `main` already holds a lot of unrelated uncommitted work in files this plan touches (`ScriptRuntime.cpp`, `ScriptBindings.cpp`, `GameView.cpp`, and others). Ask the user to commit or stash it, then create a branch `scene-camera` in AnarchyEngine-CPP and in JadeFX_CPP. Commit steps below assume a clean tree.

Build and test commands (from each repo's root, PowerShell or Bash):

- AnarchyEngine sandbox: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC1]"` (any tag, or no tag for all).
- AnarchyEngine app: `cmake --build build --config Debug --target AnarchyEngine-CPP`.
- JadeFX: `cmake --build build --config Debug --target jadefx-tests` then `build/Debug/jadefx-tests.exe`.

## Review Focus

1. **A new or freshly opened place must not show as modified** just because the built-in plugin Scripts were created. Test SC14 pins it through `Project::place_fingerprint`.
2. **The right button released outside the view while locked** (or Alt-Tab mid-drag) must not leave the pointer locked. The focus loss ends the button, the plugin sees InputEnded, and the lock drops. Test SC15.
3. **Opening another place** must leave exactly one camera plugin registered, not two. Test SC13.
4. **A CurrentCamera that was destroyed** must make the plugin skip quietly, with no error spam in Output. Test SC16.
5. **Flying the camera for a while** must not bury real edits under hundreds of undo steps. Test SC11.

---

### Task 1: JadeFX Scene pointer lock

Repo: `../JadeFX_CPP`.

**Files:**
- Modify: `include/jadefx/scene/Scene.hpp` (public API near `noteWindowFocus`, members near `clipboardSet_`)
- Modify: `src/scene/Scene.cpp` (`noteWindowFocus`, new functions beside `setClipboardBridge`)
- Modify: `include/jadefx/stage/Stage.hpp`, `src/stage/Stage.cpp` (handler, `pushPointerDelta`, event type, bridge in `hookClipboard`)
- Modify: `src/platform/GlfwHost.hpp`, `src/platform/GlfwHost.cpp` (lock, `OnMove`, `OnButton`, `OnScroll`, `OnFocus`, `setCursor`)
- Modify: `src/application/Application.cpp:127` and `src/platform/UtilityWindow.cpp:122` (wire the handler)
- Create: `tests/pointer_lock_tests.cpp`
- Modify: `tests/layout_tests.cpp:1931,1980` (declare and call `RunPointerLockTests`), `CMakeLists.txt` (add the test file to `jadefx-tests`, beside `tests/cursor_tests.cpp`)

**Interfaces:**
- Produces (used by Task 8's `GameView`):
  - `void jadefx::Scene::setPointerLocked(bool locked);`
  - `bool jadefx::Scene::isPointerLocked() const;`
  - `void jadefx::Scene::takePointerDelta(double& dx, double& dy);`
- Produces (host side): `Scene::setPointerLockBridge(std::function<void(bool)>)`, `Scene::notePointerDelta(double, double)`, `Stage::setPointerLockHandler(std::function<void(bool)>)`, `Stage::pushPointerDelta(double, double)`, `GlfwHost::setPointerLocked(bool)`.

- [ ] **Step 1: Write the failing test**

Create `tests/pointer_lock_tests.cpp`:

```cpp
#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// Without a host the lock is only recorded, and the motion still adds up.
void TestLockWithoutHost() {
    jadefx::Scene scene;
    Expect(!scene.isPointerLocked(), "a scene starts unlocked");
    scene.notePointerDelta(5, 5);
    double dx = -1;
    double dy = -1;
    scene.takePointerDelta(dx, dy);
    Expect(dx == 0 && dy == 0, "motion while unlocked is not kept");

    scene.setPointerLocked(true);
    Expect(scene.isPointerLocked(), "the lock is recorded");
    scene.notePointerDelta(3, -1);
    scene.notePointerDelta(2, 4);
    scene.takePointerDelta(dx, dy);
    Expect(dx == 5 && dy == 3, "motion while locked adds up");
    scene.takePointerDelta(dx, dy);
    Expect(dx == 0 && dy == 0, "a take starts the sum again");
}

// The bridge hears each change once, and a window focus loss ends the lock.
void TestBridgeAndFocus() {
    jadefx::Scene scene;
    std::vector<bool> calls;
    scene.setPointerLockBridge([&calls](bool locked) { calls.push_back(locked); });
    scene.setPointerLocked(true);
    scene.setPointerLocked(true);
    Expect(calls == std::vector<bool>{true}, "the bridge hears a lock once");
    scene.notePointerDelta(7, 0);
    scene.noteWindowFocus(false);
    Expect(!scene.isPointerLocked(), "losing the window's focus ends the lock");
    Expect(calls == std::vector<bool>{true, false}, "the bridge hears the unlock");
    double dx = -1;
    double dy = -1;
    scene.takePointerDelta(dx, dy);
    Expect(dx == 0 && dy == 0, "an unlock drops the motion not yet taken");
}

}  // namespace

int RunPointerLockTests() {
    TestLockWithoutHost();
    TestBridgeAndFocus();
    return gFailures;
}
```

In `tests/layout_tests.cpp`, beside `int RunCursorTests();` add `int RunPointerLockTests();`, and beside `gFailures += RunCursorTests();` add `gFailures += RunPointerLockTests();`. Add `tests/pointer_lock_tests.cpp` to the `jadefx-tests` source list in `CMakeLists.txt`, after `tests/cursor_tests.cpp`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Debug --target jadefx-tests`
Expected: compile error, `setPointerLocked` / `notePointerDelta` / `setPointerLockBridge` / `takePointerDelta` / `isPointerLocked` not members of `jadefx::Scene`.

- [ ] **Step 3: Implement the Scene lock**

In `include/jadefx/scene/Scene.hpp`, after `isWindowFocused()`:

```cpp
    // Pointer lock, for a view that turns a camera by the mouse's motion. While
    // locked the host hides the system pointer and holds it where it was, moves
    // are not delivered, and the motion adds up for takePointerDelta. Stage
    // passes the lock to its host through the bridge; with no bridge, as in a
    // test, the lock is only recorded. Losing the window's focus ends the lock.
    void setPointerLocked(bool locked);
    bool isPointerLocked() const { return pointerLocked_; }
    // The motion since the last take, in window points. Zero while unlocked.
    void takePointerDelta(double& dx, double& dy);
    // The host's side: Stage sets the bridge and forwards the motion.
    void setPointerLockBridge(std::function<void(bool)> bridge);
    void notePointerDelta(double dx, double dy);
```

Private members, beside `clipboardSet_`:

```cpp
    std::function<void(bool)> pointerLockBridge_;
    bool pointerLocked_ = false;
    double pointerDeltaX_ = 0;
    double pointerDeltaY_ = 0;
```

In `src/scene/Scene.cpp`, beside `setClipboardBridge`:

```cpp
void Scene::setPointerLocked(bool locked) {
    if (locked == pointerLocked_) {
        return;
    }
    pointerLocked_ = locked;
    pointerDeltaX_ = 0;
    pointerDeltaY_ = 0;
    if (pointerLockBridge_) {
        pointerLockBridge_(locked);
    }
}

void Scene::takePointerDelta(double& dx, double& dy) {
    dx = pointerDeltaX_;
    dy = pointerDeltaY_;
    pointerDeltaX_ = 0;
    pointerDeltaY_ = 0;
}

void Scene::setPointerLockBridge(std::function<void(bool)> bridge) { pointerLockBridge_ = std::move(bridge); }

void Scene::notePointerDelta(double dx, double dy) {
    if (!pointerLocked_) {
        return;
    }
    pointerDeltaX_ += dx;
    pointerDeltaY_ += dy;
}
```

At the top of `Scene::noteWindowFocus(bool focused)`, before its existing body:

```cpp
    // The system takes the pointer back when the window goes to the background.
    if (!focused) {
        setPointerLocked(false);
    }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --config Debug --target jadefx-tests` then `build/Debug/jadefx-tests.exe`
Expected: exit code 0, no `FAIL pointer`/`FAIL a scene`/`FAIL motion` lines.

- [ ] **Step 5: Bridge it through Stage**

In `include/jadefx/stage/Stage.hpp`, after `setCursorHandler`:

```cpp
    // Called when the scene locks or unlocks the pointer. The GLFW host captures it.
    void setPointerLockHandler(std::function<void(bool)> handler) { onPointerLock_ = std::move(handler); }
    // The host's pointer motion while locked, delivered to the scene with the other events.
    void pushPointerDelta(double dx, double dy);
```

Private member beside `onCursor_`: `std::function<void(bool)> onPointerLock_;`

In `src/stage/Stage.cpp`:
- Add `PointerDelta` to the `Event::Type` enum.
- Add, beside `pushMove`:

```cpp
void Stage::pushPointerDelta(double dx, double dy) {
    Event event;
    event.type = Event::Type::PointerDelta;
    event.x = dx;
    event.y = dy;
    events_.push_back(std::move(event));
}
```

- In `processEvents`' switch, beside `case Event::Type::Focus:`:

```cpp
            case Event::Type::PointerDelta:
                scene_->notePointerDelta(event.x, event.y);
                break;
```

- At the end of `Stage::hookClipboard()`, after the clipboard bridge:

```cpp
    scene_->setPointerLockBridge([this](bool locked) {
        if (onPointerLock_) {
            onPointerLock_(locked);
        }
    });
```

- [ ] **Step 6: Implement the lock in GlfwHost**

In `src/platform/GlfwHost.hpp`, public, after `setCursor`:

```cpp
    // Hides the pointer and holds it where it is. Moves become Stage::pushPointerDelta,
    // presses and scrolls are reported where the lock began, and cursor shapes wait
    // for the unlock, which puts the pointer back there.
    void setPointerLocked(bool locked);
    bool pointerLocked() const { return locked_; }
    // The motion since the last move while locked. Updates the last position.
    void lockedMove(double x, double y, double& dx, double& dy);
    // Where presses land: the lock's point while locked, else the cursor.
    void pointerPosition(double& x, double& y) const;
```

Private members:

```cpp
    bool locked_ = false;
    double lockX_ = 0;
    double lockY_ = 0;
    double lastX_ = 0;
    double lastY_ = 0;
    // The shape asked for last, applied again at the unlock.
    Cursor cursor_ = Cursor::Default;
```

In `src/platform/GlfwHost.cpp`:

```cpp
void GlfwHost::setPointerLocked(bool locked) {
    if (window_ == nullptr || locked == locked_) {
        return;
    }
    if (locked) {
        glfwGetCursorPos(window_, &lockX_, &lockY_);
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
        // Disabling may move the virtual cursor; motion is measured from where it is now.
        glfwGetCursorPos(window_, &lastX_, &lastY_);
        locked_ = true;
        return;
    }
    locked_ = false;
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    glfwSetCursorPos(window_, lockX_, lockY_);
    setCursor(cursor_);
}

void GlfwHost::lockedMove(double x, double y, double& dx, double& dy) {
    dx = x - lastX_;
    dy = y - lastY_;
    lastX_ = x;
    lastY_ = y;
}

void GlfwHost::pointerPosition(double& x, double& y) const {
    if (locked_) {
        x = lockX_;
        y = lockY_;
        return;
    }
    glfwGetCursorPos(window_, &x, &y);
}
```

At the top of `GlfwHost::setCursor`, after the `window_ == nullptr` check:

```cpp
    cursor_ = cursor;
    // Any mode change here would end a lock. The shape is applied at the unlock.
    if (locked_) {
        return;
    }
```

In the callbacks:
- `OnMove`: before the existing `stage->pushMove`, when `host->pointerLocked()`, call `host->lockedMove(x, y, dx, dy)`, then `stage->pushPointerDelta(dx, dy)` if either is nonzero, and return without `pushMove`.
- `OnButton` and `OnScroll`: replace `glfwGetCursorPos(window, &x, &y);` with `host->pointerPosition(x, y);`.
- `OnCursorEnter`: when `host->pointerLocked()`, return at the top. A captured pointer's enter and leave say nothing about hover.
- `OnFocus`: before `stage->pushWindowFocus(...)`, when `focused == GLFW_FALSE`, call `host->setPointerLocked(false);`. The Scene's own unlock, through the bridge, then finds it already unlocked.

- [ ] **Step 7: Wire the handler**

`src/application/Application.cpp`, after `stage.setCursorHandler(...)`:

```cpp
    stage.setPointerLockHandler([&](bool locked) { host.setPointerLocked(locked); });
```

`src/platform/UtilityWindow.cpp`, after the `setCursorHandler` block:

```cpp
    raw->stage_.setPointerLockHandler([raw](bool locked) {
        if (raw->host_) {
            raw->host_->glfw.setPointerLocked(locked);
        }
    });
```

- [ ] **Step 8: Build and run all JadeFX tests**

Run: `cmake --build build --config Debug --target jadefx-tests` then `build/Debug/jadefx-tests.exe`
Expected: exit code 0.

- [ ] **Step 9: Commit (JadeFX repo)**

```bash
git add include/jadefx/scene/Scene.hpp src/scene/Scene.cpp include/jadefx/stage/Stage.hpp src/stage/Stage.cpp src/platform/GlfwHost.hpp src/platform/GlfwHost.cpp src/application/Application.cpp src/platform/UtilityWindow.cpp tests/pointer_lock_tests.cpp tests/layout_tests.cpp CMakeLists.txt
git commit -m "Lock the pointer for a view that turns a camera by the mouse"
```

---

### Task 2: UserInputService mouse behavior, sensitivity, and delta (C++)

**Files:**
- Modify: `src/engine_datatypes/Enum.hpp`, `src/engine_datatypes/Enum.cpp` (new `MouseBehavior` enum)
- Modify: `src/engine_services/UserInputService.hpp`, `src/engine_services/UserInputService.cpp`
- Create: `sandbox/scene_camera_tests.cpp`
- Modify: `CMakeLists.txt` (add `sandbox/scene_camera_tests.cpp` to `add_executable(sandbox ...)`, and `target_compile_definitions(sandbox PRIVATE ANARCHY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")` after `target_link_libraries(sandbox ...)`)

**Interfaces:**
- Produces:
  - `const EnumType& engine_core::mouse_behavior_enum();`
  - `UserInputService::kMouseBehaviorDefault = 0`, `kLockCenter = 1`, `kLockCurrentPosition = 2`
  - `void post_mouse_delta(float dx, float dy, bool processed = false);` (any thread)
  - `Vec3 mouse_delta() const;` (SimulationThread; raw, unscaled sum of the latest dispatch; z = 0)
  - `int mouse_behavior() const; void set_mouse_behavior(int behavior);` (any thread, atomic)
  - `double mouse_delta_sensitivity() const; bool set_mouse_delta_sensitivity(double value);` (SimulationThread; false for non-finite)

- [ ] **Step 1: Write the failing tests**

Create `sandbox/scene_camera_tests.cpp`:

```cpp
// The scene camera: UserInputService's mouse lock and delta, edit-mode input,
// Workspace.CurrentCamera, and the built-in SceneCamera plugin.

#include "support.hpp"

#include "UserInputService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;
using engine_core::ScriptRuntime;
using engine_core::UserInputService;

std::vector<std::string> texts(const ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const ScriptRuntime::OutputLine& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

}  // namespace

TEST_CASE("SC1 locked mouse motion adds up for one step and leaves the location", "[SC1]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    input.post_mouse_move(10.f, 20.f);
    input.post_mouse_delta(3.f, -1.f);
    input.post_mouse_delta(2.f, 4.f);
    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 5.f);
    REQUIRE(input.mouse_delta().y == 3.f);
    REQUIRE(input.mouse_location().x == 10.f);
    REQUIRE(input.mouse_location().y == 20.f);

    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 0.f);
    REQUIRE(input.mouse_delta().y == 0.f);
}

TEST_CASE("SC2 MouseBehavior holds what was asked until the view loses focus", "[SC2]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    REQUIRE(input.mouse_behavior() == UserInputService::kMouseBehaviorDefault);
    input.set_mouse_behavior(UserInputService::kLockCurrentPosition);
    REQUIRE(input.mouse_behavior() == UserInputService::kLockCurrentPosition);
    input.post_focus_lost();
    REQUIRE(input.mouse_behavior() == UserInputService::kMouseBehaviorDefault);
}

TEST_CASE("SC3 MouseDeltaSensitivity is at least 0 and refuses what is not a number", "[SC3]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    REQUIRE(input.mouse_delta_sensitivity() == 1.0);
    REQUIRE(input.set_mouse_delta_sensitivity(2.5));
    REQUIRE(input.mouse_delta_sensitivity() == 2.5);
    REQUIRE(input.set_mouse_delta_sensitivity(-3.0));
    REQUIRE(input.mouse_delta_sensitivity() == 0.0);
    REQUIRE_FALSE(input.set_mouse_delta_sensitivity(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE(input.mouse_delta_sensitivity() == 0.0);
}
```

Add the file and the compile definition to `CMakeLists.txt` as listed under **Files**.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile errors, `post_mouse_delta`, `mouse_delta`, `mouse_behavior`, `kLockCurrentPosition`, `mouse_delta_sensitivity` not members of `UserInputService`.

- [ ] **Step 3: Add the enum**

`src/engine_datatypes/Enum.cpp`, after `kUserInputStates`:

```cpp
const EnumEntry kMouseBehaviors[] = {
    {"Default", 0}, {"LockCenter", 1}, {"LockCurrentPosition", 2},
};
```

After `kUserInputStateType`:

```cpp
const EnumType kMouseBehaviorType{"MouseBehavior", kMouseBehaviors, count_of(kMouseBehaviors)};
```

Append `&kMouseBehaviorType` to `kTypes`. Beside `user_input_state_enum()`:

```cpp
const EnumType& mouse_behavior_enum() { return kMouseBehaviorType; }
```

`src/engine_datatypes/Enum.hpp`, after `user_input_state_enum();`:

```cpp
const EnumType& mouse_behavior_enum();
```

- [ ] **Step 4: Add the service state**

`src/engine_services/UserInputService.hpp`: add `#include <atomic>`. After the `UserInputState values` constants:

```cpp
    // MouseBehavior values.
    static constexpr int kMouseBehaviorDefault = 0;
    static constexpr int kLockCenter = 1;
    static constexpr int kLockCurrentPosition = 2;
```

After `post_mouse_move`:

```cpp
    // Motion while the pointer is locked: a MouseMovement whose Delta is the
    // motion, at the mouse location as it was. The location does not move.
    void post_mouse_delta(float dx, float dy, bool processed = false);
```

Change the `post_focus_lost` comment to: `// The scene view lost keyboard focus: every key and button still down ends, and MouseBehavior goes back to Default, since the view let the pointer go.`

After `mouse_location()`:

```cpp
    // SimulationThread. The Delta of every MouseMovement in the latest dispatch,
    // added up and not scaled. GetMouseDelta scales it by the sensitivity.
    Vec3 mouse_delta() const { return mouse_delta_; }

    // Any thread. What scripts asked of the pointer. The scene view reads it
    // each paint and locks the pointer while it is not Default.
    int mouse_behavior() const { return mouse_behavior_.load(std::memory_order_relaxed); }
    void set_mouse_behavior(int behavior) { mouse_behavior_.store(behavior, std::memory_order_relaxed); }

    // SimulationThread. Clamped to 0 and up. False, changing nothing, when not finite.
    double mouse_delta_sensitivity() const { return mouse_delta_sensitivity_; }
    bool set_mouse_delta_sensitivity(double value);
```

Private, SimulationThread section, after `Vec3 mouse_{};`:

```cpp
    Vec3 mouse_delta_{};
    double mouse_delta_sensitivity_ = 1.0;
```

After `bool bound_ = false;`:

```cpp
    std::atomic<int> mouse_behavior_{kMouseBehaviorDefault};
```

`src/engine_services/UserInputService.cpp`: add `#include <cmath>`. After `post_mouse_move`:

```cpp
void UserInputService::post_mouse_delta(float dx, float dy, bool processed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_ || (dx == 0.f && dy == 0.f)) {
        return;
    }
    InputRecord record;
    record.type = kMouseMovement;
    record.state = kChange;
    record.position = posted_mouse_;
    record.delta = Vec3{dx, dy, 0.f};
    record.processed = processed;
    const bool merges = !queue_.empty() && queue_.back().type == kMouseMovement && queue_.back().processed == processed;
    if (!merges && queue_.size() >= kMaxQueued) {
        return;
    }
    push_locked(record);
}

bool UserInputService::set_mouse_delta_sensitivity(double value) {
    if (!std::isfinite(value)) {
        return false;
    }
    mouse_delta_sensitivity_ = std::max(0.0, value);
    return true;
}
```

In `post_focus_lost`, before the `if (!active_)` check:

```cpp
    set_mouse_behavior(kMouseBehaviorDefault);
```

In `dispatch`, right after `next_payload_ += dispatched_.size();`:

```cpp
    mouse_delta_ = Vec3{};
```

and inside the loop, after `mouse_ = record.position;`:

```cpp
        if (record.type == kMouseMovement) {
            mouse_delta_.x += record.delta.x;
            mouse_delta_.y += record.delta.y;
        }
```

In `reset()`, after `mouse_ = Vec3{};`:

```cpp
    mouse_delta_ = Vec3{};
    set_mouse_behavior(kMouseBehaviorDefault);
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC1],[SC2],[SC3]"`
Expected: `All tests passed (… assertions in 3 test cases)`.

- [ ] **Step 6: Run the whole sandbox**

Run: `build/Debug/sandbox.exe`
Expected: all pass. A script-analysis test that counts the Enum types may need the new `MouseBehavior` added to its expected list. If so, add it there.

- [ ] **Step 7: Commit**

```bash
git add src/engine_datatypes/Enum.hpp src/engine_datatypes/Enum.cpp src/engine_services/UserInputService.hpp src/engine_services/UserInputService.cpp sandbox/scene_camera_tests.cpp CMakeLists.txt
git commit -m "Keep MouseBehavior, MouseDeltaSensitivity, and locked mouse motion on UserInputService"
```

---

### Task 3: Input in edit mode

**Files:**
- Modify: `src/engine_core/ScriptRuntime.cpp` (`attach`, `on_stop`, `step_tools`)
- Modify: `src/engine_services/UserInputService.hpp` (class comment: posts are kept while a runtime is attached, not only in play)
- Modify: `src/runner/GameView.hpp` (class comment's last paragraph: the service now keeps input in edit mode too)
- Test: `sandbox/scene_camera_tests.cpp`

**Interfaces:**
- Consumes: `UserInputService::dispatch(EventQueue&)`, `set_active(bool)`.
- Produces: in edit mode, plugin and console connections to `InputBegan` / `InputChanged` / `InputEnded` fire on the next `step_tools`, and `IsKeyDown` / `GetMouseDelta` answer from that dispatch.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/scene_camera_tests.cpp`:

```cpp
namespace {

// GLFW's key numbers, which is what the scene view posts.
int key(char letter) { return UserInputService::key_code_from_glfw(static_cast<int>(letter)); }

// An unparented Script run as a plugin.
InstanceId add_plugin(ScriptRig& rig, const char* source) {
    engine_core::Script& script = rig.game.create<engine_core::Script>();
    rig.game.set_name(script.id(), "Plugin");
    script.set_source(source);
    REQUIRE(rig.runtime.register_plugin(script.id()));
    return script.id();
}

}  // namespace

TEST_CASE("SC4 a plugin hears keys in edit mode, and a focus loss ends them", "[SC4]") {
    ScriptRig rig;
    add_plugin(rig,
               "local uis = game:GetService('UserInputService')\n"
               "uis.InputBegan:Connect(function(input) print('began', input.KeyCode.Name) end)\n"
               "uis.InputEnded:Connect(function(input) print('ended', input.KeyCode.Name) end)\n"
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "  if uis:IsKeyDown(Enum.KeyCode.W) then print('held') end\n"
               "end)");
    rig.runtime.drain_output();

    rig.game.input().post_key(key('W'), true);
    rig.frames(1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"began\tW\n", "held\n"});

    rig.game.input().post_focus_lost();
    rig.frames(1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"ended\tW\n"});
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC4]"`
Expected: FAIL. The output is empty because the service is inactive in edit mode and nothing dispatches.

- [ ] **Step 3: Implement**

In `ScriptRuntime::attach`, after `game.input().bind(game.events());`:

```cpp
    // Input is kept whenever a runtime is attached: plugins hear it in edit mode.
    game.input().set_active(true);
```

In `ScriptRuntime::on_stop`, replace `game_->input().set_active(false);` with:

```cpp
        // Still active, for the plugins; this drops what the session left queued.
        game_->input().set_active(true);
```

Replace `ScriptRuntime::step_tools` with:

```cpp
void ScriptRuntime::step_tools(double dt) {
    if (game_ == nullptr) {
        return;
    }
    // While the play VM is closed no play step fires Heartbeat or drains, so this does.
    // During play, and while a play session is paused, the play step's own do that.
    const bool own_step = !open_;
    const bool tools_open = console_.state != nullptr || plugin_.state != nullptr;
    if (!own_step && !tools_open) {
        return;
    }
    assert_lua_thread();
    if (dt < 0) {
        dt = 0;
    }
    if (own_step) {
        // Edit-mode input, dispatched even with no tool VM open so the queue never
        // holds presses from before a plugin loaded. Its events drain before Heartbeat's.
        game_->input().dispatch(game_->events());
        if (!tools_open) {
            game_->events().drain();
            return;
        }
        run_service_.fire(game_->events(), Phase::Heartbeat, dt);
        game_->events().drain();
    }
    step_side(console_, dt);
    step_side(plugin_, dt);
    if (own_step) {
        game_->events().drain();
    }
}
```

Update the two comments listed under **Files**. The UserInputService comment should say: posts are kept while a script runtime is attached; the play step dispatches them before PreAnimation, and in edit mode the tool step dispatches them before Heartbeat.

- [ ] **Step 4: Run the test to verify it passes**

Run: `build/Debug/sandbox.exe "[SC4]"`
Expected: PASS.

- [ ] **Step 5: Run the whole sandbox and engine tests**

Run: `build/Debug/sandbox.exe` and `cmake --build build --config Debug --target engine-tests` then `build/Debug/engine-tests.exe`
Expected: all pass. A test that relied on input being dropped in edit mode is now wrong, because the spec changes that rule. Update it to the new rule and mention it in the commit message.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/ScriptRuntime.cpp src/engine_services/UserInputService.hpp src/runner/GameView.hpp sandbox/scene_camera_tests.cpp
git commit -m "Deliver scene view input to plugins in edit mode"
```

---

### Task 4: Lua API — MouseBehavior, MouseDeltaSensitivity, GetMouseDelta, IsRunning

**Files:**
- Modify: `src/engine_services/UserInputService.cpp` (fields in `register_input_service_lua`)
- Modify: `src/engine_core/ScriptBindings.hpp` (declare `service_newindex`, `input_get_mouse_delta`, `run_is_running`)
- Modify: `src/engine_core/ScriptBindings.cpp` (`service_index`, new functions, `register_script_methods`)
- Modify: `src/engine_core/ScriptRuntime.cpp` (service metatable gets `__newindex`)
- Modify: `src/engine_core/LuaApi.cpp` (docs, beside the UserInputService and RunService entries)
- Test: `sandbox/scene_camera_tests.cpp`

**Interfaces:**
- Consumes: Task 2's `mouse_behavior()`, `set_mouse_behavior(int)`, `mouse_delta()`, `mouse_delta_sensitivity()`, `set_mouse_delta_sensitivity(double)`, `mouse_behavior_enum()`.
- Produces (Lua): `UserInputService.MouseBehavior` (EnumItem, writable), `UserInputService.MouseDeltaSensitivity` (number, writable), `UserInputService:GetMouseDelta()` → Vector2, `RunService:IsRunning()` → boolean.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/scene_camera_tests.cpp`:

```cpp
TEST_CASE("SC5 scripts read and write MouseBehavior", "[SC5]") {
    ScriptRig rig;
    rig.runtime.run_chunk(
        "local uis = game:GetService('UserInputService')\n"
        "print(uis.MouseBehavior == Enum.MouseBehavior.Default)\n"
        "uis.MouseBehavior = Enum.MouseBehavior.LockCurrentPosition\n"
        "print(uis.MouseBehavior.Name)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\n", "LockCurrentPosition\n"});
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);

    rig.runtime.run_chunk("game:GetService('UserInputService').MouseBehavior = 'Nope'");
    const ScriptRuntime::OutputBatch refused = rig.runtime.drain_output();
    REQUIRE(refused.lines.size() == 1);
    REQUIRE(refused.lines[0].kind == ScriptRuntime::OutputKind::Error);
}

TEST_CASE("SC6 GetMouseDelta is the step's motion times MouseDeltaSensitivity", "[SC6]") {
    ScriptRig rig;
    add_plugin(rig,
               "local uis = game:GetService('UserInputService')\n"
               "uis.MouseDeltaSensitivity = 2\n"
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "  local d = uis:GetMouseDelta()\n"
               "  if d.Magnitude > 0 then print(d.X, d.Y) end\n"
               "end)");
    rig.runtime.drain_output();
    rig.game.input().post_mouse_delta(3.f, 4.f);
    rig.frames(2);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"6\t8\n"});
}

TEST_CASE("SC7 RunService:IsRunning is true only in a play session", "[SC7]") {
    ScriptRig rig;
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"false\n"});
    // Test clears the Output, so both lines below are printed after it.
    rig.game.start_simulation();
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    rig.game.stop_simulation();
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\n", "false\n"});
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC5],[SC6],[SC7]"`
Expected: FAIL with errors such as `unknown UserInputService member` and `attempt to call a nil value` / `unknown RunService member`.

- [ ] **Step 3: Declare the fields**

In `register_input_service_lua` (`UserInputService.cpp`), add read and write functions above it:

```cpp
bool read_sensitivity(DataModel& world, DataModel&, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Number;
    out.number = world.input().mouse_delta_sensitivity();
    return true;
}

bool write_sensitivity(DataModel& world, DataModel&, LuaSlot& in) {
    if (in.kind != LuaSlot::Kind::Number || !world.input().set_mouse_delta_sensitivity(in.number)) {
        in.error = "MouseDeltaSensitivity must be a number";
        return false;
    }
    return true;
}
```

Add these to its `fields` array:

```cpp
        // No read: ScriptBindings pushes and checks the EnumItem itself.
        lua_property("MouseBehavior", "EnumItem", true, nullptr, nullptr),
        lua_property("MouseDeltaSensitivity", "number", true, read_sensitivity, write_sensitivity),
```

`UserInputService.cpp` must reach `DataModel::input()`. Include `DataModel.hpp` if it isn't pulled in already.

- [ ] **Step 4: Bind them**

`ScriptBindings.hpp`, beside `service_index`: `static int service_newindex(lua_State* state);`. Beside `input_get_mouse_location`: `static int input_get_mouse_delta(lua_State* state);` and `static int run_is_running(lua_State* state);`.

`ScriptBindings.cpp`, in `service_index`, right after `if (field->method) { ... }`:

```cpp
    if (service->kind == kUserInputServiceKind && std::strcmp(field->name, "MouseBehavior") == 0) {
        ScriptRuntime* runtime = runtime_from(state);
        const int behavior = runtime != nullptr && runtime->game_ != nullptr ? runtime->game_->input().mouse_behavior()
                                                                             : UserInputService::kMouseBehaviorDefault;
        push_enum_item(state, mouse_behavior_enum(), behavior);
        return 1;
    }
```

After `service_index`:

```cpp
int ScriptBindings::service_newindex(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* service = static_cast<ServiceUd*>(luaL_checkudata(state, 1, kServiceMeta));
        const char* key = luaL_checkstring(state, 2);
        const char* class_name =
            service->kind >= 0 && service->kind < kServiceKinds ? kServiceClasses[service->kind] : "";
        const LuaField* field = lua_class_find(class_name, key != nullptr ? key : "");
        if (field == nullptr || field->method || !field->writable) {
            luaL_error(state, "%s cannot be assigned to", key != nullptr ? key : "");
        }
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            luaL_error(state, "%s is not available here", class_name);
        }
        if (service->kind == kUserInputServiceKind && std::strcmp(field->name, "MouseBehavior") == 0) {
            runtime->game_->input().set_mouse_behavior(check_enum_arg(state, 3, mouse_behavior_enum()));
            return 0;
        }
        LuaSlot slot;
        if (lua_isnumber(state, 3)) {
            slot.kind = LuaSlot::Kind::Number;
            slot.number = lua_tonumber(state, 3);
        } else if (lua_isboolean(state, 3)) {
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = lua_toboolean(state, 3) != 0;
        }
        if (field->write == nullptr || !field->write(*runtime->game_, *runtime->game_, slot)) {
            luaL_error(state, "%s", slot.error.empty() ? "invalid value" : slot.error.c_str());
        }
        return 0;
    });
}
```

Beside `input_get_mouse_location`:

```cpp
int ScriptBindings::input_get_mouse_delta(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        Vec2 delta{0.f, 0.f};
        if (input != nullptr) {
            const Vec3 raw = input->mouse_delta();
            const float scale = static_cast<float>(input->mouse_delta_sensitivity());
            delta = Vec2{raw.x * scale, raw.y * scale};
        }
        push_vector2(state, delta);
        return 1;
    });
}

int ScriptBindings::run_is_running(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        ScriptRuntime* runtime = runtime_from(state);
        lua_pushboolean(state, runtime != nullptr && runtime->vm_open() ? 1 : 0);
        return 1;
    });
}
```

In `register_script_methods`, add to the `input` array:

```cpp
        lua_method("GetMouseDelta", "Vector2", reinterpret_cast<void*>(&ScriptBindings::input_get_mouse_delta)),
```

and after the UserInputService registration:

```cpp
    // RunService.cpp declares the class, its signals, and the service.
    const LuaField run[] = {
        lua_method("IsRunning", "boolean", reinterpret_cast<void*>(&ScriptBindings::run_is_running)),
    };
    register_lua_class("RunService", nullptr, run, 1);
```

(`register_lua_class` with an existing name adds members, as `Selection` and `UserInputService` already show.)

`ScriptRuntime.cpp`, where `service_mt` is built, before `lua_setreadonly(state, service_mt, 1);`:

```cpp
    lua_pushcfunction(state, &ScriptBindings::service_newindex, "newindex");
    lua_setfield(state, service_mt, "__newindex");
```

- [ ] **Step 5: Document them**

`LuaApi.cpp`, after the `TouchEnabled` doc line:

```cpp
    add("UserInputService", "MouseBehavior",
        "What the pointer does. LockCurrentPosition or LockCenter hides it and holds it in the focused scene view, and "
        "GetMouseDelta reports its motion. Default frees it. Losing the view's focus sets Default.",
        "EnumItem", false, {});
    add("UserInputService", "MouseDeltaSensitivity", "Scales GetMouseDelta. 1 by default, never below 0.", "number",
        false, {});
    add("UserInputService", "GetMouseDelta",
        "How far the mouse moved in the latest step, in points, times MouseDeltaSensitivity. It keeps reporting while "
        "the pointer is locked.",
        "Vector2", false, {});
```

After the `RenderStepped` doc line:

```cpp
    add("RunService", "IsRunning", "True while a play session is open, paused or not. False in edit mode.", "boolean",
        false, {});
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC5],[SC6],[SC7]"`, then `build/Debug/sandbox.exe` for all
Expected: all pass. A doc-coverage test, or the LuauComplete tests (`tests/LuauCompleteTest.cpp`, target `engine-tests`), may need the new members. Run `build/Debug/engine-tests.exe` too.

- [ ] **Step 7: Commit**

```bash
git add src/engine_services/UserInputService.cpp src/engine_core/ScriptBindings.hpp src/engine_core/ScriptBindings.cpp src/engine_core/ScriptRuntime.cpp src/engine_core/LuaApi.cpp sandbox/scene_camera_tests.cpp
git commit -m "Give scripts MouseBehavior, MouseDeltaSensitivity, GetMouseDelta, and RunService:IsRunning"
```

---

### Task 5: Workspace.CurrentCamera

**Files:**
- Modify: `src/engine_services/SceneService.hpp` (Workspace members)
- Modify: `src/engine_services/SceneService.cpp` (implementation and Lua field)
- Modify: `src/engine_core/Project.cpp` (`clear_world` clears it; include `SceneService.hpp` if absent)
- Modify: `src/engine_core/LuaApi.cpp` (doc)
- Test: `sandbox/scene_camera_tests.cpp`

**Interfaces:**
- Produces:
  - `InstanceId engine_core::Workspace::current_camera() const;` (0 when unset, dead, or not a Camera)
  - `bool engine_core::Workspace::set_current_camera(InstanceId id);` (0 clears; false when `id` is live and not a Camera)
  - Lua: `workspace.CurrentCamera` (Camera?, writable)

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/scene_camera_tests.cpp` (add `#include "Camera.hpp"`, `#include "Project.hpp"`, `#include "SceneService.hpp"` at the top):

```cpp
namespace {

engine_core::Workspace& workspace_service(engine_core::DataModel& game) {
    auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    return *workspace;
}

engine_core::Camera& add_camera(engine_core::DataModel& game) {
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), workspace_of(game));
    return camera;
}

}  // namespace

TEST_CASE("SC8 CurrentCamera holds a Camera and forgets it when it is gone", "[SC8]") {
    ScriptRig rig;
    engine_core::Workspace& workspace = workspace_service(rig.game);
    engine_core::Camera& camera = add_camera(rig.game);
    REQUIRE(workspace.current_camera() == 0);
    REQUIRE(workspace.set_current_camera(camera.id()));
    REQUIRE(workspace.current_camera() == camera.id());
    REQUIRE_FALSE(workspace.set_current_camera(workspace_of(rig.game)));
    REQUIRE(workspace.current_camera() == camera.id());

    rig.runtime.drain_output();
    rig.runtime.run_chunk("print(workspace.CurrentCamera.ClassName)\nworkspace.CurrentCamera = nil\nprint(workspace.CurrentCamera)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"Camera\n", "nil\n"});

    REQUIRE(workspace.set_current_camera(camera.id()));
    rig.game.destroy(camera.id());
    REQUIRE(workspace.current_camera() == 0);
}

TEST_CASE("SC9 a script cannot make CurrentCamera anything but a Camera", "[SC9]") {
    ScriptRig rig;
    rig.runtime.run_chunk("workspace.CurrentCamera = workspace");
    const ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].kind == ScriptRuntime::OutputKind::Error);
}

TEST_CASE("SC10 CurrentCamera is not an edit: no undo step, not saved, cleared by a new place", "[SC10]") {
    ScriptRig rig;
    engine_core::Camera& camera = add_camera(rig.game);
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    const std::uint64_t revision = rig.game.authored_revision();
    const std::uint64_t fingerprint = engine_core::Project::place_fingerprint(rig.game);

    REQUIRE(workspace_service(rig.game).set_current_camera(camera.id()));
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE(rig.game.authored_revision() == revision);
    REQUIRE(engine_core::Project::place_fingerprint(rig.game) == fingerprint);

    engine_core::Project::reset_place(rig.game);
    REQUIRE(workspace_service(rig.game).current_camera() == 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile errors, `current_camera` / `set_current_camera` not members of `Workspace`.

- [ ] **Step 3: Implement**

`SceneService.hpp`, replace the `Workspace` class:

```cpp
// Instances here render in the game. Anything may go in it.
// CurrentCamera is the Camera the studio's scene view last used: set when you
// press in a view or pick its camera. It is session state: never saved, never
// an undo step, 0 once that Camera is gone, and cleared when the place is
// rebuilt. A script may set it too; the views do not follow a script's write.
class Workspace : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;

    InstanceId current_camera() const;
    // 0 clears it. False, changing nothing, when id is a live instance that is not a Camera.
    bool set_current_camera(InstanceId id);

private:
    InstanceId current_camera_ = 0;
};
```

`SceneService.cpp`:

```cpp
InstanceId Workspace::current_camera() const {
    if (current_camera_ == 0 || !alive(current_camera_)) {
        return 0;
    }
    const DataModel* target = instance(current_camera_);
    return target != nullptr && lua_class_inherits(target->class_name(), "Camera") ? current_camera_ : 0;
}

bool Workspace::set_current_camera(InstanceId id) {
    if (id != 0) {
        const DataModel* target = alive(id) ? instance(id) : nullptr;
        if (target == nullptr || !lua_class_inherits(target->class_name(), "Camera")) {
            return false;
        }
    }
    current_camera_ = id;
    return true;
}
```

In the anonymous namespace, above `register_scene_service_lua`:

```cpp
bool read_current_camera(DataModel&, DataModel& object, LuaSlot& out) {
    auto* workspace = dynamic_cast<Workspace*>(&object);
    if (workspace == nullptr) {
        return false;
    }
    out.id = workspace->current_camera();
    out.kind = out.id != 0 ? LuaSlot::Kind::Instance : LuaSlot::Kind::Nil;
    return true;
}

// Not an edit: no history, no change to the place.
bool write_current_camera(DataModel&, DataModel& object, LuaSlot& in) {
    auto* workspace = dynamic_cast<Workspace*>(&object);
    if (workspace == nullptr) {
        return false;
    }
    const bool shaped = in.kind == LuaSlot::Kind::Nil || in.kind == LuaSlot::Kind::Instance;
    if (!shaped || !workspace->set_current_camera(in.kind == LuaSlot::Kind::Instance ? in.id : 0)) {
        in.error = "CurrentCamera must be a Camera";
        return false;
    }
    return true;
}
```

In `register_scene_service_lua`, replace `register_lua_class("Workspace", "SceneService", nullptr, 0);` with:

```cpp
    const LuaField workspace[] = {
        lua_property("CurrentCamera", "Camera?", true, read_current_camera, write_current_camera),
    };
    register_lua_class("Workspace", "SceneService", workspace, 1);
```

If `lua_class_inherits` or `LuaSlot` are not declared through the current includes, include `LuaApi.hpp` (already included) and `DataModel.hpp`.

`Project.cpp`, at the end of `clear_world` (before `clear_extras(world, 0);`):

```cpp
    // The camera a view last used belongs to the place being cleared.
    if (auto* workspace = dynamic_cast<Workspace*>(world.instance(world.scene_service("Workspace")))) {
        workspace->set_current_camera(0);
    }
```

`LuaApi.cpp`, beside other instance-property docs (search for an `add("Workspace"` or `add("Camera"` line and put it there; if none, put it after the RunService block):

```cpp
    add("Workspace", "CurrentCamera",
        "The Camera the studio's scene view last used, set when you click in a view or pick its camera. Nil when that "
        "Camera is gone. Not saved.",
        "Camera", false, {});
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC8],[SC9],[SC10]"`
Expected: PASS. If a write to a service's property is refused by the generic instance `__newindex` (SC8/SC9 error text such as "cannot modify a service"), find that guard in `ScriptBindings.cpp`'s instance newindex. Let writable registered properties through on services, and keep refusing `Name` and `Parent` there.

- [ ] **Step 5: Run the whole sandbox, then commit**

Run: `build/Debug/sandbox.exe`, `build/Debug/engine-tests.exe`
Expected: all pass. A Properties-panel or project round-trip test may now list `CurrentCamera` among Workspace properties. It must not be written to files: confirm the project writer only saves `lua_saved_property` fields.

```bash
git add src/engine_services/SceneService.hpp src/engine_services/SceneService.cpp src/engine_core/Project.cpp src/engine_core/LuaApi.cpp sandbox/scene_camera_tests.cpp
git commit -m "Add Workspace.CurrentCamera, the Camera a scene view last used"
```

---

### Task 6: A Camera's moves are not undo steps

**Files:**
- Modify: `src/engine_instances/GameObject.hpp` (virtual hook)
- Modify: `src/engine_instances/Camera.hpp` (override, class comment)
- Modify: `src/engine_core/DataModel.cpp:574-596` (`apply_transform`)
- Test: `sandbox/scene_camera_tests.cpp`

**Interfaces:**
- Produces: `virtual bool engine_core::GameObject::transform_in_history() const;` (true by default; Camera returns false)

- [ ] **Step 1: Write the failing test**

Append to `sandbox/scene_camera_tests.cpp` (add `#include "Matrix4.hpp"` if `matrix4_translation` isn't visible):

```cpp
TEST_CASE("SC11 moving a Camera marks the place changed but is not an undo step", "[SC11]") {
    ScriptRig rig;
    engine_core::Camera& camera = add_camera(rig.game);
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    const std::uint64_t revision = rig.game.authored_revision();

    for (int step = 1; step <= 50; ++step) {
        camera.set_transform(engine_core::matrix4_translation(0.f, 0.f, static_cast<float>(step)));
    }
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE(rig.game.authored_revision() != revision);

    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    rig.game.history().end_gesture();
    REQUIRE(rig.game.history().can_undo().first);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC11]"`
Expected: FAIL at the first `REQUIRE_FALSE(... can_undo ...)`.

- [ ] **Step 3: Implement**

`GameObject.hpp`, public, after `set_linear_velocity`:

```cpp
    // False for a class whose moves are a viewpoint, not content: its Transform
    // writes still mark the place changed, so they save, but are not undo steps.
    virtual bool transform_in_history() const { return true; }
```

`Camera.hpp`, public, after `class_name()`:

```cpp
    // Flying the camera is looking around, not editing.
    bool transform_in_history() const override { return false; }
```

Add to the Camera class comment: `Its Transform writes are not undo steps (transform_in_history), though they save.`

`DataModel.cpp` `apply_transform`, replace `record_transform(id, previous, transform);` with:

```cpp
    if (target->transform_in_history()) {
        record_transform(id, previous, transform);
    } else {
        mark_authored_dirty(id);
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `build/Debug/sandbox.exe "[SC11]"`, then `build/Debug/sandbox.exe` and `build/Debug/engine-tests.exe`
Expected: all pass. A history test that undoes a Camera move is now wrong by the spec. Change it to use a plain GameObject.

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances/GameObject.hpp src/engine_instances/Camera.hpp src/engine_core/DataModel.cpp sandbox/scene_camera_tests.cpp
git commit -m "Keep Camera moves out of undo history"
```

---

### Task 7: The plugin loader and the SceneCamera plugin

**Files:**
- Create: `src/ide/PluginLoader.hpp`, `src/ide/PluginLoader.cpp`
- Create: `resources/plugins/SceneCamera.luau`
- Modify: `CMakeLists.txt` (add `src/ide/PluginLoader.cpp` to `STUDIO_CORE_SOURCES`, after `src/ide/McpToolSpecs.cpp`)
- Test: `sandbox/scene_camera_tests.cpp`

**Interfaces:**
- Consumes: `ScriptRuntime::register_plugin/unregister_plugin`, `Workspace::set_current_camera`, all of Tasks 2–6's Lua API, `ide::find_resource(const std::string&)` from `IdeResources.hpp`.
- Produces:

```cpp
namespace ide {
struct PluginFile { std::string name; std::string source; };
inline constexpr const char* kBuiltinPlugins[] = {"plugins/SceneCamera.luau"};
std::vector<PluginFile> read_builtin_plugins(std::vector<std::string>& errors);
bool read_plugin_file(const std::filesystem::path& path, PluginFile& out, std::string& error);
class PluginLoader {
public:
    std::size_t load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, const std::vector<PluginFile>& files);
    const std::vector<engine_core::InstanceId>& loaded() const { return loaded_; }
private:
    std::vector<engine_core::InstanceId> loaded_;
};
}
```

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/scene_camera_tests.cpp` (add `#include "ide/PluginLoader.hpp"` and `#include <filesystem>` at the top; the include path is `src/`, as `ide/IdePane.hpp` is included elsewhere):

```cpp
namespace {

ide::PluginFile scene_camera_file() {
    ide::PluginFile file;
    std::string error;
    REQUIRE(ide::read_plugin_file(std::filesystem::path(ANARCHY_SOURCE_DIR) / "resources/plugins/SceneCamera.luau", file,
                                  error));
    return file;
}

// A Camera at the origin looking down -Z, the view's current one, and the plugin loaded.
struct CameraRig : ScriptRig {
    ide::PluginLoader loader;
    InstanceId camera = 0;

    CameraRig() {
        camera = add_camera(game).id();
        REQUIRE(workspace_service(game).set_current_camera(camera));
        REQUIRE(loader.load(game, runtime, {scene_camera_file()}) == 1);
        runtime.drain_output();
    }

    engine_core::Matrix4 transform() { return dynamic_cast<engine_core::Camera*>(game.instance(camera))->transform(); }
    engine_core::Vec3 position() {
        const engine_core::Matrix4 m = transform();
        return {m.m[12], m.m[13], m.m[14]};
    }
    engine_core::Vec3 look() {
        const engine_core::Matrix4 m = transform();
        return {-m.m[8], -m.m[9], -m.m[10]};
    }
};

}  // namespace

TEST_CASE("SC12 W moves the camera where it looks, and E lifts it", "[SC12]") {
    CameraRig rig;
    rig.game.input().post_key(key('W'), true);
    rig.frames(1, 0.5);
    REQUIRE(std::abs(rig.position().z + 8.f) < 1e-3f);  // 16 studs/s for half a second, down -Z
    rig.game.input().post_key(key('W'), false);
    rig.game.input().post_key(key('E'), true);
    rig.frames(1, 0.25);
    REQUIRE(std::abs(rig.position().y - 4.f) < 1e-3f);
    REQUIRE(rig.runtime.drain_output().lines.empty());
}

TEST_CASE("SC13 the right button locks the pointer and the locked motion turns the camera", "[SC13]") {
    CameraRig rig;
    rig.game.input().post_mouse_button(1, true, 50.f, 50.f);
    rig.frames(1);
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);

    // Right is +X from a camera looking down -Z.
    rig.game.input().post_mouse_delta(100.f, 0.f);
    rig.frames(1);
    REQUIRE(rig.look().x > 0.3f);

    // Far up: the pitch stops at 89 degrees.
    rig.game.input().post_mouse_delta(0.f, -100000.f);
    rig.frames(1);
    REQUIRE(rig.look().y > 0.99f);
    REQUIRE(std::asin(std::min(1.f, rig.look().y)) <= 89.01f * 3.14159265f / 180.f);

    rig.game.input().post_mouse_button(1, false, 50.f, 50.f);
    rig.frames(1);
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kMouseBehaviorDefault);

    // Loading again, as after opening a place, leaves one plugin.
    REQUIRE(rig.loader.load(rig.game, rig.runtime, {scene_camera_file()}) == 1);
    REQUIRE(rig.runtime.plugins().size() == 1);
}

TEST_CASE("SC14 loading the built-in plugins is not an edit to the place", "[SC14]") {
    ScriptRig rig;
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    const std::uint64_t fingerprint = engine_core::Project::place_fingerprint(rig.game);
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {scene_camera_file()}) == 1);
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE(engine_core::Project::place_fingerprint(rig.game) == fingerprint);
    REQUIRE(rig.game.get_children(workspace_of(rig.game)).empty());
}

TEST_CASE("SC15 losing focus mid-turn lets the pointer go", "[SC15]") {
    CameraRig rig;
    rig.game.input().post_mouse_button(1, true, 50.f, 50.f);
    rig.frames(1);
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);
    rig.game.input().post_focus_lost();
    rig.frames(1);
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kMouseBehaviorDefault);
}

TEST_CASE("SC16 with no CurrentCamera, or in play, the plugin leaves the camera alone", "[SC16]") {
    CameraRig rig;
    const engine_core::Vec3 before = rig.position();
    rig.game.start_simulation();
    rig.game.input().post_key(key('W'), true);
    rig.frames(3);
    REQUIRE(rig.position().z == before.z);
    rig.game.stop_simulation();
    rig.runtime.drain_output();

    rig.game.destroy(rig.camera);
    rig.game.input().post_key(key('S'), true);
    rig.frames(3);
    REQUIRE(rig.runtime.drain_output().lines.empty());
}
```

`SC14` checks `get_children(workspace)` is empty on a bare rig. If `ScriptRig` starts with children in Workspace, compare the count before and after instead.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile error, `ide/PluginLoader.hpp` not found.

- [ ] **Step 3: Write the loader**

`src/ide/PluginLoader.hpp`:

```cpp
#pragma once

#include "types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
class ScriptRuntime;
}  // namespace engine_core

namespace ide {

// One built-in plugin: a Script's name and its source.
struct PluginFile {
    std::string name;
    std::string source;
};

// The studio's own plugins, under resources/.
inline constexpr const char* kBuiltinPlugins[] = {"plugins/SceneCamera.luau"};

// Reads a .luau file. The name is the file's stem. False, with why, when it cannot be read.
bool read_plugin_file(const std::filesystem::path& path, PluginFile& out, std::string& error);
// Reads each of kBuiltinPlugins. One that cannot be found or read is left out, and why goes to errors.
std::vector<PluginFile> read_builtin_plugins(std::vector<std::string>& errors);

// Runs plugin files in the plugin VM, each as an unparented Script: not in the
// explorer, not saved, and not an undo step. The place is rebuilt by New and
// Open, which destroys those Scripts, so the studio loads again after each.
class PluginLoader {
public:
    // SimulationThread. Unregisters and destroys what the last load made, then
    // makes and registers one Script per file. Returns how many registered.
    std::size_t load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                     const std::vector<PluginFile>& files);
    const std::vector<engine_core::InstanceId>& loaded() const { return loaded_; }

private:
    std::vector<engine_core::InstanceId> loaded_;
};

}  // namespace ide
```

`src/ide/PluginLoader.cpp`:

```cpp
#include "PluginLoader.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "IdeResources.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"

#include <fstream>
#include <sstream>

namespace ide {

bool read_plugin_file(const std::filesystem::path& path, PluginFile& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not read " + path.u8string();
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out.name = path.stem().u8string();
    out.source = text.str();
    return true;
}

std::vector<PluginFile> read_builtin_plugins(std::vector<std::string>& errors) {
    std::vector<PluginFile> files;
    for (const char* relative : kBuiltinPlugins) {
        const std::filesystem::path path = find_resource(relative);
        PluginFile file;
        std::string error;
        if (path.empty()) {
            errors.push_back(std::string("no ") + relative + " in resources");
        } else if (!read_plugin_file(path, file, error)) {
            errors.push_back(error);
        } else {
            files.push_back(std::move(file));
        }
    }
    return files;
}

std::size_t PluginLoader::load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                               const std::vector<PluginFile>& files) {
    // None of this is the user's edit.
    engine_core::ChangeHistoryService& history = game.history();
    const bool recording = history.enabled();
    history.set_enabled(false);
    for (engine_core::InstanceId id : loaded_) {
        scripts.unregister_plugin(id);
        if (game.alive(id)) {
            game.destroy(id);
        }
    }
    loaded_.clear();
    std::size_t registered = 0;
    for (const PluginFile& file : files) {
        engine_core::Script& script = game.create<engine_core::Script>();
        game.set_name(script.id(), file.name);
        script.set_source(file.source);
        loaded_.push_back(script.id());
        if (scripts.register_plugin(script.id())) {
            ++registered;
        }
    }
    history.set_enabled(recording);
    return registered;
}

}  // namespace ide
```

Adjust the includes to how other `ide/` core files reach engine headers (for example `src/ide/McpTools.cpp` includes `"ScriptRuntime.hpp"` directly). If `DataModel::history()` is spelled differently, use the accessor `sandbox/history_tests.cpp` uses, `game.history()`.

- [ ] **Step 4: Write the plugin**

`resources/plugins/SceneCamera.luau`:

```lua
-- Flies the scene view's camera in edit mode. Hold the right mouse button in a
-- scene view to lock the pointer and turn. W, A, S, and D move where the camera
-- faces; E and Q move straight up and down. In play the game owns the camera,
-- so this does nothing.
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")

local MOVE_SPEED = 16 -- studs per second
local TURN_SPEED = 0.004 -- radians per point of mouse motion
local MAX_PITCH = math.rad(89)

UserInputService.InputBegan:Connect(function(input)
	if input.UserInputType == Enum.UserInputType.MouseButton2 and not RunService:IsRunning() then
		UserInputService.MouseBehavior = Enum.MouseBehavior.LockCurrentPosition
	end
end)

UserInputService.InputEnded:Connect(function(input)
	if input.UserInputType == Enum.UserInputType.MouseButton2 then
		UserInputService.MouseBehavior = Enum.MouseBehavior.Default
	end
end)

-- 1, -1, or 0, from a pair of keys.
local function axis(positive, negative)
	local value = 0
	if UserInputService:IsKeyDown(positive) then
		value += 1
	end
	if UserInputService:IsKeyDown(negative) then
		value -= 1
	end
	return value
end

RunService.Heartbeat:Connect(function(dt)
	local camera = workspace.CurrentCamera
	if camera == nil or RunService:IsRunning() then
		return
	end
	local transform = camera.Transform
	local rotation = transform.Rotation
	local turned = false
	if UserInputService.MouseBehavior ~= Enum.MouseBehavior.Default then
		local delta = UserInputService:GetMouseDelta()
		if delta.Magnitude > 0 then
			local pitch, yaw = transform:ToOrientation()
			yaw -= delta.X * TURN_SPEED
			pitch = math.clamp(pitch - delta.Y * TURN_SPEED, -MAX_PITCH, MAX_PITCH)
			rotation = Matrix4.fromOrientation(pitch, yaw, 0)
			turned = true
		end
	end
	local move = rotation.LookVector * axis(Enum.KeyCode.W, Enum.KeyCode.S)
		+ rotation.RightVector * axis(Enum.KeyCode.D, Enum.KeyCode.A)
		+ Vector3.new(0, axis(Enum.KeyCode.E, Enum.KeyCode.Q), 0)
	local moved = move.Magnitude > 0
	-- A still camera is not written, so an idle view does not mark the place changed.
	if not turned and not moved then
		return
	end
	local position = transform.Position
	if moved then
		position += move.Unit * MOVE_SPEED * dt
	end
	camera.Transform = Matrix4.new(position) * rotation
end)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[SC12],[SC13],[SC14],[SC15],[SC16]"`
Expected: PASS. If SC14's fingerprint changes, `Project::place_fingerprint` is counting parentless instances. Change it to walk only from the root (`game` and its descendants), since unparented instances are not part of the place. Add that change to this task's commit.

- [ ] **Step 6: Run everything, then commit**

Run: `build/Debug/sandbox.exe`, `build/Debug/engine-tests.exe`
Expected: all pass.

```bash
git add src/ide/PluginLoader.hpp src/ide/PluginLoader.cpp resources/plugins/SceneCamera.luau CMakeLists.txt sandbox/scene_camera_tests.cpp
git commit -m "Load built-in plugins, starting with SceneCamera, which flies the scene view's camera"
```

---

### Task 8: Wire the studio — CurrentCamera, pointer lock, plugin loading

**Files:**
- Modify: `src/runner/GameView.hpp`, `src/runner/GameView.cpp`
- Modify: `src/ide/IdeLayout.hpp` (member and method), `src/ide/IdeLayout.cpp` (constructor), `src/ide/IdeLayoutProject.cpp` (`new_place`, `open_project_at`)
- Modify: `src/engine_core/README.md` or `src/engine_services/README.md` (one line: UserInputService works in edit mode; Workspace.CurrentCamera), and `README.md` (the controls: right-drag to turn, WASD/QE to fly)

**Interfaces:**
- Consumes: Task 1's `Scene::setPointerLocked / isPointerLocked / takePointerDelta`; Task 2's `mouse_behavior`, `set_mouse_behavior`, `post_mouse_delta`; Task 5's `Workspace::set_current_camera`; Task 7's `ide::PluginLoader`, `ide::read_builtin_plugins`.

- [ ] **Step 1: GameView sets CurrentCamera**

`GameView.hpp`, private: `void noteCurrentCamera();` and `void syncPointerLock();`, and a member `bool pointerLocked_ = false;`. Update the class comment's input paragraph:

```cpp
// Keys and the mouse over this view go to the place's UserInputService, in edit
// mode and in play. A press here takes keyboard focus and makes this view's
// Camera the Workspace's CurrentCamera; losing focus ends whatever was still
// held. While a script sets MouseBehavior to a lock and this view has focus,
// the pointer is locked in it and its motion goes to GetMouseDelta.
```

`GameView.cpp`: `#include "SceneService.hpp"`.

```cpp
void GameView::noteCurrentCamera() {
    if (engine_ == nullptr || cameraId_ == 0) {
        return;
    }
    engine_->on_simulation([camera = cameraId_](engine_core::DataModel& game) {
        if (auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(game.scene_service("Workspace")))) {
            workspace->set_current_camera(camera);
        }
    });
}
```

Call it in `handleMousePressed`, right after `requestFocus();`, and in the combo box's `setOnAction` lambda after `linkCamera(...)`.

- [ ] **Step 2: GameView drives the pointer lock**

```cpp
void GameView::syncPointerLock() {
    jadefx::Scene* scene = getScene();
    if (game_ == nullptr || scene == nullptr) {
        return;
    }
    engine_core::UserInputService& input = game_->input();
    if (pointerLocked_ && !scene->isPointerLocked()) {
        // The scene let the pointer go, as when the window lost focus. Scripts hear it.
        pointerLocked_ = false;
        input.set_mouse_behavior(engine_core::UserInputService::kMouseBehaviorDefault);
        return;
    }
    const bool wanted = isFocused() && input.mouse_behavior() != engine_core::UserInputService::kMouseBehaviorDefault;
    if (wanted != pointerLocked_) {
        scene->setPointerLocked(wanted);
        pointerLocked_ = wanted;
    }
    if (pointerLocked_) {
        double dx = 0;
        double dy = 0;
        scene->takePointerDelta(dx, dy);
        input.post_mouse_delta(static_cast<float>(dx), static_cast<float>(dy));
    }
}
```

Call `syncPointerLock();` at the top of `renderContent`, after `notePaint();`.

At the top of `sceneChanged`, before its early return:

```cpp
    // A lock belongs to the window the view is leaving.
    if (pointerLocked_) {
        if (previous != nullptr && !previous->isTearingDown()) {
            previous->setPointerLocked(false);
        }
        pointerLocked_ = false;
    }
```

- [ ] **Step 3: The studio loads the plugins**

`IdeLayout.hpp`: `#include "PluginLoader.hpp"`. Private member `PluginLoader plugins_;` and method `void load_plugins();` with the comment `// Runs the built-in plugins again. The place was just made, opened, or rebuilt.`

`IdeLayout.cpp`:

```cpp
void IdeLayout::load_plugins() {
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    std::vector<std::string> errors;
    const std::vector<PluginFile> files = read_builtin_plugins(errors);
    for (const std::string& error : errors) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error, "Plugin: " + error);
    }
    run_now([&](engine_core::DataModel& game) { plugins_.load(game, scripts, files); });
}
```

Call `load_plugins();` at the end of the `IdeLayout` constructor. It must run after `runner_.prepare()`, so the runtime is attached, and after the initial place exists. Also call it at the end of `new_place()` (after `mark_saved();`) and of `open_project_at()` (after `mark_saved();`). Plugin loading is not an edit (SC14), so the order relative to `mark_saved` doesn't change the title. Keep it after `mark_saved` so a mistake would show up as a modified title.

If the constructor runs before the initial place is built, move that call to wherever the first place is created, or to `IdeLayout::attach` of the main stage. Either way it runs once, before the user can interact.

- [ ] **Step 4: Build the studio**

Run: `cmake --build build --config Debug --target AnarchyEngine-CPP`
Expected: builds with no new warnings (`/W4`).

- [ ] **Step 5: Run every test suite**

Run: `build/Debug/sandbox.exe`, `build/Debug/engine-tests.exe`, `build/Debug/studio-tests.exe`, `build/Debug/mcp-tests.exe`
Expected: all pass. A studio test that counts Output lines at startup must still see no plugin errors.

- [ ] **Step 6: Verify in the studio by hand**

Launch `build/Debug/AnarchyEngine-CPP.exe` (use the `run` skill to drive and screenshot it if available) and check:
1. The Output has no `Plugin:` errors, and the title shows no unsaved mark.
2. Click in the Scene View, then hold W: the camera flies forward. S, A, D, E, and Q move as named.
3. Hold the right button and move the mouse: the pointer disappears and the view turns (mouse right turns right, mouse up looks up, and it stops at straight up). Release: the pointer reappears where it was pressed.
4. While right-dragging, Alt-Tab away and back: the pointer is free and the camera no longer turns.
5. Click in a script editor and type `wasd`: the camera does not move.
6. Test (play): WASD does nothing to the camera. Stop: flying works again.
7. File > New, then fly: works (plugin re-registered). Edit > Undo does not undo camera moves.

- [ ] **Step 7: Update the READMEs, then commit**

Add to `README.md` in the studio controls section: `Scene View: right-drag to look around, W/A/S/D to fly, E/Q to rise and sink.`. Add to `src/engine_services/README.md` in the UserInputService line: it works in edit mode for plugins, plus MouseBehavior and GetMouseDelta. Mention Workspace.CurrentCamera there.

```bash
git add src/runner/GameView.hpp src/runner/GameView.cpp src/ide/IdeLayout.hpp src/ide/IdeLayout.cpp src/ide/IdeLayoutProject.cpp README.md src/engine_services/README.md
git commit -m "Fly the scene view's camera with the built-in SceneCamera plugin"
```
