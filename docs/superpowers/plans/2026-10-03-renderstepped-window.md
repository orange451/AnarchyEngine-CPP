# RenderStepped in the Render Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `RunService.RenderStepped:Connect` and `:Wait()` run play and plugin scripts on the render thread inside the Prepare window, once per displayed frame with that frame's dt, so what a handler reads is what that frame draws.

**Architecture:** Delivery bypasses the EventQueue drain (forbidden in the window): connections from play and plugin VMs land on a second host signal in RunService, and a new `EventQueue::invoke_render` runs that signal's handlers synchronously from ScriptRuntime's render-phase job. Thread safety is the existing DataModel write lock, which already serializes the sim step against the Prepare window; the rule "Lua runs on SimulationThread" becomes "Lua runs while holding the write lock". Console-VM connections keep the sim-side delivery that exists today (fired at the next step with summed frame time).

**Tech Stack:** C++17, Luau, Catch2 (sandbox target), ThreadSanitizer build.

**Spec:** `docs/superpowers/specs/2026-10-03-renderstepped-window-design.md`

## Global Constraints

- The scheduling design is unchanged: no new threads, locks, or phases; the window job stays a plain render-phase job.
- Mac builds with Xcode 13 / libc++ 13: no post-C++17 library features.
- PreRender stays blocked to scripts.
- Comments follow the codebase voice: full sentences, say why, no change markers.
- All sandbox + engine-tests suites green after every task; commits per task on a branch off `main`.
- Rebuilding the studio means the `bundle-resources` target, never just `AnarchyStudio`.

## Review Focus

1. A handler that connects another RenderStepped handler mid-frame: the walk must not crash or run the new handler this same frame — pinned in Task 1.
2. A handler that disconnects its own connection while running: tombstone, no skip of the next slot — pinned in Task 1.
3. A play session stopped while a handler is parked in `task.wait`: the continuation must never resume into the new world — pinned in Task 4.
4. `print()` from a window handler: `append_output` is called from a thread it never saw before; output must arrive uncorrupted — pinned in Task 3.
5. A handler erroring every frame: console reports each frame, connection stays, engine lives — pinned in Task 3.

---

### Task 1: `EventQueue::invoke_render`

Synchronous invocation of a host signal's handlers, callable only on the render thread inside the prerender window. Tagged (play) slots are skippable as one group and go through the script gate; `once` slots tombstone after running; connects and disconnects from inside a handler are safe.

**Files:**
- Modify: `src/engine_core/Events.hpp` (EventQueue public section, after `host_signal`/`release_signal`)
- Modify: `src/engine_core/Events.cpp`
- Test: `sandbox/tests.cpp` (append; tag `[RW1]`)

**Interfaces:**
- Produces: `void EventQueue::invoke_render(Signal& signal, bool include_tagged);` — Task 3's window job calls it. Handlers receive `(InstanceId 0, Field::Name)`, as the sim-side fire does today.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/tests.cpp`:

```cpp
TEST_CASE("RW1 invoke_render runs a host signal's handlers in the window", "[RW1]") {
    SimRole role;
    engine_core::Game game;
    engine_core::EventQueue& events = game.events();
    engine_core::Signal signal;
    events.host_signal(&signal);
    int kept_runs = 0;
    int tagged_runs = 0;
    int once_runs = 0;
    engine_core::Connection self_made;
    engine_core::Connection self_gone =
        signal.connect_kept([&](engine_core::InstanceId, engine_core::Field) { self_gone.disconnect(); }, false);
    signal.connect_kept(
        [&](engine_core::InstanceId, engine_core::Field) {
            ++kept_runs;
            if (kept_runs == 1) {
                // A connect from inside a handler joins next frame, not this walk.
                self_made = signal.connect_kept(
                    [&](engine_core::InstanceId, engine_core::Field) { ++once_runs; }, false);
            }
        },
        false);
    signal.connect_scripted([&](engine_core::InstanceId, engine_core::Field) { ++tagged_runs; }, 7, 1, false);

    std::thread render([&] {
        engine_core::set_thread_role(engine_core::ThreadRole::Render);
        game.set_prerender_window(true);
        events.invoke_render(signal, false);  // paused: tagged skipped
        REQUIRE(kept_runs == 1);
        REQUIRE(tagged_runs == 0);
        REQUIRE(once_runs == 0);
        events.invoke_render(signal, true);
        REQUIRE(kept_runs == 2);
        REQUIRE(tagged_runs == 1);
        REQUIRE(once_runs == 1);
        game.set_prerender_window(false);
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    });
    render.join();
    events.release_signal(signal);
}
```

If `connect_scripted` without a script gate set fires tagged slots unconditionally, that is the intended behavior here (the gate is null, so every tag passes — see `set_script_gate`). If the walk delivers to `self_gone` before its own disconnect, adjust the test to assert only that nothing crashes and the second walk skips it.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target sandbox -j 10 && ./build/sandbox "[RW1]"`
Expected: compile error, `invoke_render` is not a member of `EventQueue`.

- [ ] **Step 3: Implement `invoke_render`**

Declaration in `Events.hpp`, after `release_signal`:

```cpp
    // Runs this host signal's handlers now, on RenderThread inside the prerender
    // window, where drain is forbidden. include_tagged false skips the slots a
    // script tagged, which is how a paused play session stays silent; the kept
    // slots, the console's and plugins', always run. The script gate still
    // guards the tagged slots it runs. Handlers may connect and disconnect.
    void invoke_render(Signal& signal, bool include_tagged);
```

Implementation in `Events.cpp`. Mirror `invoke`'s slot walk (Events.cpp:231) exactly — same generation checks, same `once` tombstoning, same `payload_`/`current_args_` save-and-restore with payload 0 and null args — but iterate the signal's own list (`signal.head_`, `ConnSlot::next`) instead of taking a queued event, snapshot each slot's `next` **before** calling its handler (a handler may disconnect anything, including itself), and skip a slot whose generation changed since the walk reached it. Skip tagged slots (`script != 0`) when `include_tagged` is false; when true, pass them through the script gate as `invoke` does. Guard the entry:

```cpp
void EventQueue::invoke_render(Signal& signal, bool include_tagged) {
    if (prerender_open_ == nullptr || !*prerender_open_) {
        contract_fail("invoke_render runs inside RenderStepped or PreRender");
    }
    ...
}
```

(`prerender_open_` is the pointer `watch_prerender` stored; use its actual member name.) New connections made during the walk append past the captured tail; do not deliver to a slot appended after the walk began — capture `signal.tail_` first and stop after it.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target sandbox -j 10 && ./build/sandbox "[RW1]"`
Expected: PASS. Then the full suite: `./build/sandbox` — all green.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/Events.hpp src/engine_core/Events.cpp sandbox/tests.cpp
git commit -m "Add EventQueue::invoke_render, window-side handler invocation"
```

---

### Task 2: RunService window signal and dt

**Files:**
- Modify: `src/engine_services/RunService.hpp`
- Modify: `src/engine_services/RunService.cpp`
- Test: `sandbox/tests.cpp` (tag `[RW2]`)

**Interfaces:**
- Produces: `Signal* RunService::window_signal();` (the signal play and plugin connections land on), `void RunService::set_window_dt(double dt);` (stores into `dt_[Phase::RenderStepped]` so the existing `dt(phase)` read works). `bind`/`release` register the new signal like the others.
- Consumes: nothing new.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("RW2 the window signal binds with RunService and carries its own dt", "[RW2]") {
    SimRole role;
    engine_core::Game game;
    engine_core::RunService service;
    service.bind(game.events());
    REQUIRE(service.window_signal() != nullptr);
    REQUIRE(service.window_signal()->id().valid());
    service.set_window_dt(0.004);
    REQUIRE(service.dt(engine_core::Phase::RenderStepped) == 0.004);
    service.release(game.events());
    REQUIRE_FALSE(service.window_signal()->id().valid());
}
```

Add `#include "RunService.hpp"` to tests.cpp if missing.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target sandbox -j 10`
Expected: compile error, no member `window_signal`.

- [ ] **Step 3: Implement**

In `RunService.hpp`, next to `render_stepped_`, add `Signal render_stepped_window_;` with the comment:

```cpp
    // The window signal: play and plugin RenderStepped connections land here and
    // are invoked on RenderThread inside the Prepare window, once per displayed
    // frame. render_stepped_ keeps the sim-side delivery for the console VM.
```

Public members:

```cpp
    // The signal window connections land on. Valid between bind and release.
    Signal* window_signal() { return &render_stepped_window_; }
    // The displayed frame's dt, which window handlers read through dt(RenderStepped).
    void set_window_dt(double dt) { dt_[static_cast<int>(Phase::RenderStepped)] = dt; }
```

In `RunService.cpp`, `bind` gains `events.host_signal(&render_stepped_window_);` and `release` gains `events.release_signal(render_stepped_window_);`. `signal(Phase)` is unchanged — it still returns `render_stepped_` for RenderStepped, the console path.

Update the class comment in `RunService.hpp`: replace the paragraph beginning "RenderStepped is the engine phase on RenderThread, but its script signal fires on SimulationThread" with:

```cpp
// RenderStepped reaches play and plugin scripts on RenderThread, inside the
// Prepare window, once per displayed frame with that frame's dt: ScriptRuntime's
// render job invokes window_signal's handlers directly, since the queue cannot
// drain there. The console VM cannot join the window (its command line enters it
// without the write lock), so its connections stay on the sim-side signal:
// note_frame counts each frame, and the next step fires it with their summed time.
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build/sandbox "[RW2]"` then `./build/sandbox` — all green.

- [ ] **Step 5: Commit**

```bash
git add src/engine_services/RunService.hpp src/engine_services/RunService.cpp sandbox/tests.cpp
git commit -m "Give RunService a window signal for render-step delivery"
```

---

### Task 3: Window delivery for Connect

Play and plugin `RenderStepped:Connect` handlers run in the window. This task relaxes `assert_lua_thread`, routes connections, adds the window job, the pause flag, and rewrites S51.

**Files:**
- Modify: `src/engine_core/ScriptRuntime.hpp` (members + `set_render_paused`), `src/engine_core/ScriptRuntime.cpp` (`attach`, `assert_lua_thread`, new `render_step`), `src/engine_core/ScriptBindings.cpp` (`signal_connect` routing), `src/engine_core/LuaApi.cpp` (doc line ~1162)
- Modify: `sandbox/support.hpp` (`ScriptRig::render` opens the window)
- Test: `sandbox/tests.cpp` (rewrite S51; add `[RW3]`)

**Interfaces:**
- Consumes: `invoke_render` (Task 1), `window_signal`/`set_window_dt` (Task 2).
- Produces: `void ScriptRuntime::render_step(double dt);` (the render-phase job body), `void ScriptRuntime::set_render_paused(bool paused);` (Task 6's Engine hook), `bool in_render_window_` (member ScriptBindings reads; Task 4 and 5 branch on it).

- [ ] **Step 1: Update the rig and rewrite S51 as the failing test**

In `sandbox/support.hpp`, `ScriptRig::render` opens the window around the phase, as the engine's Prepare does:

```cpp
    // One rendered frame: RenderStepped run on a thread of its own in the render
    // role, inside the prerender window, as the engine's render thread runs it.
    void render(double dt = 1.0 / 60.0) {
        std::thread render_thread([&] {
            engine_core::set_thread_role(engine_core::ThreadRole::Render);
            game.set_prerender_window(true);
            scheduler.run_phase(engine_core::Phase::RenderStepped, dt);
            game.set_prerender_window(false);
            engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
        });
        render_thread.join();
    }
```

Replace S51's body in `sandbox/tests.cpp`:

```cpp
TEST_CASE("S51 a play RenderStepped handler runs in the window, once per frame, with the frame's dt",
          "[S51]") {
    ScriptRig rig;
    add_script(rig.game, "Watch", R"(
        game:GetService("RunService").RenderStepped:Connect(function(dt)
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
            print("from the window")
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    REQUIRE(rig.runtime.global_is_nil("n"));  // no frame drawn yet

    rig.render(0.004);
    double n = 0;
    double dt = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);  // delivered in the window, before any sim step
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.004) < 1e-9);

    rig.render(0.008);
    rig.render(0.016);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 3);  // once per frame, not summed into a step
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.016) < 1e-9);

    play_step(rig, 0.05);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 3);  // the step adds nothing
    REQUIRE(rig.runtime.last_error().empty());
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    int prints = 0;
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
        if (line.text.find("from the window") != std::string::npos) {
            ++prints;
        }
    }
    REQUIRE(prints == 3);  // append_output survived the render thread
}
```

Add `[RW3]`, the error-every-frame case:

```cpp
TEST_CASE("RW3 a handler that errors reports every frame and stays connected", "[RW3]") {
    ScriptRig rig;
    add_script(rig.game, "Bad", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.n = (_G.n or 0) + 1
            error("window boom")
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    rig.render(0.016);
    double n = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 2);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    int errors = 0;
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error &&
            line.text.find("window boom") != std::string::npos) {
            ++errors;
        }
    }
    REQUIRE(errors == 2);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target sandbox -j 10 && ./build/sandbox "[S51],[RW3]"`
Expected: S51 FAILS — `n` is nil after `rig.render` (delivery still waits for the sim step). RW3 fails likewise. If instead Lua aborts on `assert_lua_thread`, that is the same missing feature; proceed.

- [ ] **Step 3: Implement window delivery**

`ScriptRuntime.hpp`: in the public section, after `drop_render_frames`:

```cpp
    // Pauses window delivery for play handlers: a paused session's signals wait,
    // but plugins keep their render step. The engine calls it from pause and
    // resume on whatever thread holds pause_mu_, so it is atomic.
    void set_render_paused(bool paused) { render_paused_.store(paused, std::memory_order_relaxed); }
```

In the private section, after `lua_depth_`:

```cpp
    // The render job that runs window handlers. RenderThread, inside Prepare.
    void render_step(double dt);
    // True while render_step invokes handlers. Bindings branch on it: a Wait on
    // the window signal resumes here, and a refused write raises instead of
    // deferring silently.
    bool in_render_window_ = false;
    std::atomic<bool> render_paused_{false};
```

(`#include <atomic>` if absent.) In `ScriptRuntime.cpp`, `attach` replaces the note-frame lambda:

```cpp
    // On RenderThread this runs window handlers and counts the frame for the
    // console fallback. Scripts in the window hold the same write lock the sim
    // step holds, so the two never run Lua at once.
    phase_jobs_.push_back(scheduler.bind(Phase::RenderStepped, [this](double dt) { render_step(dt); }));
```

The job:

```cpp
void ScriptRuntime::render_step(double dt) {
    run_service_.note_frame(dt);
    if (game_ == nullptr) {
        return;
    }
    Signal* window = run_service_.window_signal();
    if (window == nullptr || !window->id().valid()) {
        return;
    }
    // A paused play session's handlers wait for resume; plugins keep stepping.
    const bool include_play = open_ && !play_.closing && !render_paused_.load(std::memory_order_relaxed);
    run_service_.set_window_dt(dt);
    in_render_window_ = true;
    game_->events().invoke_render(*window, include_play);
    in_render_window_ = false;
}
```

`assert_lua_thread` learns the window:

```cpp
void ScriptRuntime::assert_lua_thread() const {
    // Lua runs wherever the DataModel write lock is held: the simulation step,
    // a paused edit, or the render thread inside the Prepare window.
    if (game_ != nullptr && game_->prerender_window() && thread_role() == ThreadRole::Render) {
        return;
    }
    if (thread_role() == ThreadRole::Render) {
        contract_fail("Lua runs on SimulationThread");
    }
    if (game_ != nullptr && !game_->on_gameplay_thread()) {
        contract_fail("Lua runs on SimulationThread");
    }
    if (game_ != nullptr && game_->prerender_window()) {
        contract_fail("Lua runs on SimulationThread");
    }
}
```

`ScriptBindings.cpp`, in `signal_connect`, after `Signal* signal = &signal_of(state, *runtime, *ud);`: route window VMs. The run-service branch is the `else` of the kind checks; RenderStepped's phase tag identifies it:

```cpp
        if (ud->kind != kSignalChanged && ud->kind != kSignalEvent && ud->kind != kSignalInput &&
            static_cast<Phase>(ud->phase) == Phase::RenderStepped &&
            vm.kind != ScriptRuntime::VmKind::Console) {
            // Play and plugin handlers run in the render window; the console VM
            // keeps the sim-side delivery, since its command line enters it
            // without the write lock.
            signal = runtime->run_service_.window_signal();
        }
```

(Use the actual constant names for the kinds — `kSignalChanged` etc. from `ScriptBindings.hpp`.) The existing `connect_scripted`/`connect_kept` split below needs no change. The handler lambda already reads `runtime->run_service_.dt(phase)`; `set_window_dt` feeds it.

`LuaApi.cpp` (~line 1162), the doc text becomes:

```cpp
    add("RunService", "RenderStepped",
        "Fires every displayed frame, before the frame is drawn. The handler runs in the render step: what it "
        "reads is what the frame draws, and a visual write lands in that frame. The argument dt is the frame's "
        "time in seconds.",
        "Signal", false, {});
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build/sandbox "[S51],[RW3],[RW1],[RW2]"` then the full `./build/sandbox` and `cmake --build build --target engine-tests -j 10 && ./build/engine-tests`.
Expected: all green except S52 and S53, which still assume sim-side delivery for play scripts. Adjust only their delivery expectations to the window (a handler now fires per `rig.render`, not per step) so the suite stays green; both are properly rewritten in Tasks 4 and 6. Do not delete assertions.

Note on runaway handlers: no new guard is needed — window handlers resume through `resume_one`, which already runs under the interrupt budget (`kScriptTimeout`), so a `while true do end` errors as it does on the sim side. Confirm by reading `interrupt` and `resume_one` while here; if the budget turns out to be armed somewhere window resumes do not pass, arm it in `render_step` the same way.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ScriptRuntime.hpp src/engine_core/ScriptRuntime.cpp src/engine_core/ScriptBindings.cpp src/engine_core/LuaApi.cpp sandbox/support.hpp sandbox/tests.cpp
git commit -m "Run play and plugin RenderStepped handlers in the render window"
```

---

### Task 4: Wait and yields in the window

`RenderStepped:Wait()` resumes in the window with the frame dt; `task.wait` inside a handler parks it and the continuation runs on the sim side; Stop strands neither.

**Files:**
- Modify: `src/engine_core/ScriptBindings.cpp` (`signal_wait` routing + window resume), `src/engine_core/ScriptRuntime.hpp`/`.cpp` (resume helper)
- Test: `sandbox/tests.cpp` (rewrite S52; add `[RW4]`, `[RW5]`)

**Interfaces:**
- Consumes: `in_render_window_`, `window_signal` (Tasks 2–3).
- Produces: `void ScriptRuntime::resume_waiting_now(Thread& thread, double dt);` — pushes dt and resumes the thread immediately; used only from window handlers.

- [ ] **Step 1: Write the failing tests**

Rewrite S52:

```cpp
TEST_CASE("S52 RenderStepped:Wait resumes in the window with the frame's dt", "[S52]") {
    ScriptRig rig;
    add_script(rig.game, "Waiter", R"(
        while true do
            local dt = game:GetService("RunService").RenderStepped:Wait()
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
        end
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    play_step(rig, 0.05);
    REQUIRE(rig.runtime.global_is_nil("n"));
    rig.render(0.004);
    rig.render(0.008);
    double n = 0;
    double dt = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 2);  // one resume per frame, no sim step between
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.008) < 1e-9);
    REQUIRE(rig.runtime.last_error().empty());
}
```

Add:

```cpp
TEST_CASE("RW4 task.wait in a window handler resumes the continuation on the sim side", "[RW4]") {
    ScriptRig rig;
    add_script(rig.game, "Yielder", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.before = (_G.before or 0) + 1
            task.wait(0.01)
            _G.after = (_G.after or 0) + 1
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    double before = 0;
    REQUIRE(rig.runtime.global_number("before", before));
    REQUIRE(before == 1);
    REQUIRE(rig.runtime.global_is_nil("after"));  // parked, not run in the window
    play_step(rig, 0.05);  // the step wakes the sleep
    double after = 0;
    REQUIRE(rig.runtime.global_number("after", after));
    REQUIRE(after == 1);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("RW5 a continuation parked at Stop never resumes into the restored world", "[RW5]") {
    ScriptRig rig;
    add_script(rig.game, "Yielder", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            task.wait(0.01)
            _G.leaked = true
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    rig.game.stop_simulation();
    rig.frames(4, 0.05);
    rig.render(0.016);
    REQUIRE(rig.runtime.global_is_nil("leaked"));
    REQUIRE(rig.runtime.last_error().empty());
}
```

(If `global_is_nil` after `stop_simulation` reads the closed play VM and cannot answer, assert instead that `drain_output` holds no error lines and the engine lives — the play VM's close already releases its threads.)

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target sandbox -j 10 && ./build/sandbox "[S52],[RW4],[RW5]"`
Expected: S52 FAILS — `n` stays nil after renders because the Wait handler only marks the thread ready and nothing resumes it until a sim step. RW4/RW5 may pass already; if they do, note it and keep them.

- [ ] **Step 3: Implement window resume**

`ScriptRuntime.hpp`, private, next to `make_ready_number`:

```cpp
    // Pushes dt and resumes the thread now. Window Wait handlers use it: in the
    // window there is no later drain to resume a ready thread, so the resume is
    // the delivery.
    void resume_waiting_now(Thread& thread, double dt);
```

`ScriptRuntime.cpp`:

```cpp
void ScriptRuntime::resume_waiting_now(Thread& thread, double dt) {
    make_ready_number(thread, dt);
    resume_one(thread);
}
```

If `make_ready_number` moves the thread onto the ready list, take it back off before `resume_one`, the way the drain-side resume does — read `make_ready_number` and `resume_one` first and reuse their bookkeeping rather than inventing new state.

`ScriptBindings.cpp`, `signal_wait`: route the signal exactly as `signal_connect` did in Task 3 (same `if`, same comment, before `thread->park = ...`). In the handler lambda, branch:

```cpp
            runtime->guarded(*waiting->vm, [&] {
                if (kind == kSignalChanged) {
                    runtime->make_ready(*waiting, changed_name(field, runtime->game_->events().payload()));
                } else if (kind == kSignalInput || kind == kSignalEvent) {
                    runtime->make_ready_args(*waiting, runtime->game_->events().current_args());
                } else if (runtime->in_render_window_) {
                    runtime->resume_waiting_now(*waiting, runtime->run_service_.dt(phase));
                } else {
                    runtime->make_ready_number(*waiting, runtime->run_service_.dt(phase));
                }
            });
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build/sandbox "[S52],[RW4],[RW5]"` then full `./build/sandbox`.
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ScriptRuntime.hpp src/engine_core/ScriptRuntime.cpp src/engine_core/ScriptBindings.cpp sandbox/tests.cpp
git commit -m "Resume RenderStepped:Wait in the window; yields park to the sim side"
```

---

### Task 5: Window write semantics

A visual write from a handler lands in that frame's snapshot; a refused write raises a catchable Lua error instead of deferring silently.

**Files:**
- Modify: `src/engine_core/ScriptBindings.cpp` (`instance_newindex` at :413, `service_newindex` at :841)
- Test: `sandbox/tests.cpp` (`[RW6]`, `[RW7]`)

**Interfaces:**
- Consumes: `in_render_window_` (Task 3), `DataModel::has_deferred_violation()` / `take_deferred_violation()` (exist).
- Produces: nothing new for later tasks.

- [ ] **Step 1: Write the failing tests**

`[RW6]` follows T21's pump pattern (sandbox/tests.cpp:1082) with a script writing in the window. Reuse T21's setup lines for the pump verbatim; the shape:

```cpp
TEST_CASE("RW6 a window handler's visual write lands in that frame's snapshot", "[RW6]") {
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Mover");
    // Mirror T21: mark the part visual-only so a render-step write is path B.
    // Copy the exact call T21 uses for that (set_visual_only or the property).
    add_script(rig.game, "Push", R"(
        local part = workspace:FindFirstChild("Mover")
        game:GetService("RunService").RenderStepped:Connect(function()
            local t = part.Transform
            t.Position = Vector3.new(5, 0, 0)
            part.Transform = t
        end)
    )");
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    rig.game.start_simulation();
    play_step(rig, 0.05);
    std::thread render([&] {
        engine_core::set_thread_role(engine_core::ThreadRole::Render);
        pump.begin_prerender_window(rig.game);
        rig.scheduler.run_phase(engine_core::Phase::RenderStepped, 0.016);
        pump.end_prerender_window(rig.game);
        pump.take_changes(rig.game);
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    });
    render.join();
    pump.finish_copy();
    pump.publish();
    const engine_core::VisualInstance* vis = pump.find(part.id());
    REQUIRE(vis != nullptr);
    REQUIRE(std::fabs(vis->world.m[12] - 5.0f) < 1e-4f);  // this frame, not the next
    REQUIRE(rig.runtime.last_error().empty());
}
```

(`begin_prerender_window` sets the flag `ScriptRig::render` otherwise sets; do not set both. If T21 shows extra pump or thread-id setup — `set_thread_ids`, `set_threads_running` — copy it; the authorize path needs the render thread registered when `threads_running` is on, and if T21 runs with threads_running off, authorize admits everything and this test still proves the snapshot timing, which is its point.)

```cpp
TEST_CASE("RW7 a write the window refuses raises a Lua error the handler can catch", "[RW7]") {
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Solid");
    // Threads on and this thread registered as render, so authorize refuses a
    // non-visual write from the window.
    add_script(rig.game, "Refused", R"(
        local part = workspace:FindFirstChild("Solid")
        game:GetService("RunService").RenderStepped:Connect(function()
            local ok, err = pcall(function()
                part.Simulated = true
            end)
            _G.ok = ok
            _G.err = tostring(err)
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    std::thread render([&] {
        engine_core::set_thread_role(engine_core::ThreadRole::Render);
        rig.game.set_thread_ids(std::thread::id(), std::this_thread::get_id());
        rig.game.set_threads_running(true);
        rig.game.set_prerender_window(true);
        rig.scheduler.run_phase(engine_core::Phase::RenderStepped, 0.016);
        rig.game.set_prerender_window(false);
        rig.game.set_threads_running(false);
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    });
    render.join();
    bool ok = true;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE_FALSE(ok);  // the pcall caught it: the refusal is a Lua error, not an abort
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE_FALSE(rig.game.take_deferred_violation());  // consumed by the Lua error, not left for the engine
}
```

Pick a property for the refused write that truly fails authorize (not `visual_only`-tagged); if `Simulated` routes through a command queue instead, use the property T24 or the S9-era tests used for a render-side refusal, reading `DataModel.cpp`'s authorize callers to choose one that reaches `reject_write`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target sandbox -j 10 && ./build/sandbox "[RW6],[RW7]"`
Expected: RW6 may already pass (path B machinery is older than this plan) — if so it is a pin, keep it. RW7 FAILS: `ok` is true or the violation is left deferred, because nothing turns the deferral into a Lua error.

- [ ] **Step 3: Implement the refusal check**

In `ScriptBindings.cpp`, in `instance_newindex` and `service_newindex`, after the write completes and before returning, inside the existing `lua_guard`:

```cpp
        // In the window, authorize refuses silently and defers the violation for
        // the engine to count. A script deserves the message instead: consume the
        // deferral and raise, so pcall catches it and the frame is not charged
        // with a contract.
        if (runtime->in_render_window_ && runtime->game_->has_deferred_violation()) {
            runtime->game_->take_deferred_violation();
            luaL_error(state, "%s cannot be written in a render step", key);
        }
```

(`key` is the property name both functions already hold; match the local's real name. If `take_deferred_violation` can be taught to return the deferred message cheaply, prefer raising that text; otherwise this generic line stands.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build/sandbox "[RW6],[RW7]"` then full `./build/sandbox`.
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ScriptBindings.cpp sandbox/tests.cpp
git commit -m "Raise refused window writes as Lua errors; pin same-frame snapshot writes"
```

---

### Task 6: Pause, Stop, plugins, and the console fallback

**Files:**
- Modify: `src/engine_core/Engine.cpp` (`pause`, `resume`)
- Test: `sandbox/tests.cpp` (rework S53 into the console test; add `[RW8]`), `sandbox/plugin_tests.cpp` or `scene_camera_tests.cpp` for the plugin case if a rig fits better there

**Interfaces:**
- Consumes: `set_render_paused` (Task 3).

- [ ] **Step 1: Write the failing tests**

`[RW8]`, pause and Stop for play handlers, plugins running through:

```cpp
TEST_CASE("RW8 pause silences play handlers, Stop removes them, plugins run through", "[RW8]") {
    ScriptRig rig;
    add_script(rig.game, "Watch", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.n = (_G.n or 0) + 1
        end)
    )");
    rig.runtime.run_chunk(R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.plugin_like = (_G.plugin_like or 0) + 1
        end)
    )");
    // run_chunk is the console VM: its connection takes the sim-side fallback,
    // so this test drives a real plugin if the loader rig is cheap here;
    // otherwise assert the console half in RW9 and keep this test to play.
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    double n = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
    rig.runtime.set_render_paused(true);
    rig.render(0.016);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);  // paused: silent
    rig.runtime.set_render_paused(false);
    rig.render(0.016);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 2);
    rig.game.stop_simulation();
    rig.render(0.016);
    rig.frames(1, 0.05);
    // The play VM closed; its connection is gone and nothing errors.
    REQUIRE(rig.runtime.last_error().empty());
}
```

For the plugin half, use the `CameraRig` pattern (scene_camera_tests.cpp:299, `PluginLoader`) if SceneCamera itself proves it — its `RenderStepped:Connect` camera moving in edit mode across `rig.render()` frames IS the plugin test; add to `scene_camera_tests.cpp`:

```cpp
TEST_CASE("SC14 the scene camera moves per rendered frame in edit mode", "[SC14]") {
    CameraRig rig;
    rig.game.input().post_key(key('W'), true);
    rig.frames(1, 0.0);           // input reaches the plugin VM
    rig.render(0.25);             // one rendered frame moves the camera
    rig.render(0.25);
    REQUIRE(std::abs(rig.position().z + 8.f) < 1e-1f);  // 16 studs/s for half a second
}
```

(Adjust `CameraRig::frames` from the earlier merge: it prepends `render(dt)` per step — with window delivery this double-moves; rework it to plain `ScriptRig::frames` and move the `render` calls into the tests that need them, keeping SC12/SC13 green with the same totals.)

Rework S53 into the console fallback:

```cpp
TEST_CASE("S53 a console connection falls back to sim-side delivery with summed dt", "[S53]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        game:GetService("RunService").RenderStepped:Connect(function(dt)
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
        end)
    )");
    rig.render(0.01);
    rig.render(0.02);
    rig.frames(1, 0.05);  // step_tools fires the fallback with the summed time
    double n = 0;
    double dt = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.03) < 1e-6);
}
```

(Console globals: if `run_chunk` globals are not visible to `global_number`, read them back with another `run_chunk` printing into output, as other console tests do — follow the pattern at tests.cpp:2806.)

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target sandbox -j 10 && ./build/sandbox "[RW8],[S53],[SC14],[SC12],[SC13]"`
Expected: RW8 fails only if pause does not silence (it should pass from Task 3's flag — then it is a pin); S53 passes if the fallback survived Tasks 2–3 intact (a pin); SC14 fails until `CameraRig` is reworked. At least one must fail; if all pass, re-check that S53's play-script version was actually replaced.

- [ ] **Step 3: Wire the engine**

`Engine.cpp`:

```cpp
void Engine::pause() {
    {
        std::lock_guard<std::mutex> guard(pause_mu_);
        paused_ = true;
    }
    // A paused session's window handlers wait with its signals.
    scripts_->set_render_paused(true);
    // Paused sounds wait where they are; the first step after resume plays them on.
    audio_.suspend();
}
```

In `resume()`, after `paused_ = false;` block: `scripts_->set_render_paused(false);` (the existing `drop_render_frames()` call stays, first). In `stop()` nothing: the play VM close already removes play connections.

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build/sandbox "[RW8],[S53],[SC14],[SC12],[SC13]"` then full `./build/sandbox && ./build/engine-tests`.
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/Engine.cpp sandbox/tests.cpp sandbox/scene_camera_tests.cpp
git commit -m "Pause window delivery with the session; prove plugin and console paths"
```

---

### Task 7: Docs, TSan, and the live proof

**Files:**
- Modify: `src/engine_core/README.md` (path B row), `src/engine_core/TaskScheduler.hpp` (header comment), `src/engine_core/DataModel.hpp` (:114 comment if it still says scripts cannot run in the window)
- No new tests; this task is verification.

- [ ] **Step 1: Update the comments that state the old rule**

README path table, row B becomes: `| B | RenderStepped or PreRender, visual-only or ForceSimWrite — including script RenderStepped handlers, which run here | DataModel, and this frame's snapshot. |` and add one sentence to the Prepare paragraph: "Play and plugin RenderStepped handlers run inside this window; the write lock that serializes Prepare against the sim step is what makes Lua safe here." Sweep `TaskScheduler.hpp`, `DataModel.hpp`, `DataModelState.hpp` for comments asserting Lua or scripts never run on RenderThread and amend only those that are now false.

- [ ] **Step 2: Full suites and ThreadSanitizer**

Run:
```bash
cmake --build build --target sandbox engine-tests -j 10 && ./build/sandbox && ./build/engine-tests
cmake --build build-tsan --target sandbox --parallel 10
for t in RW1 RW3 RW4 RW5 RW6 RW7 RW8 S51 S52 S53 SC14; do ./build-tsan/sandbox "[$t]"; done
```
Expected: all green; no `ThreadSanitizer: data race` lines. (T21/T24 misbehave under TSan on `main` already; they are out of scope.)

- [ ] **Step 3: The live proof**

```bash
cmake --build build --target bundle-resources -j 10
open build/AnarchyStudio.app
```
In the studio: open the CannonVsPigs project (`~/Documents/AnarchyEngineProjects/CannonVsPigs`), Test, fire a cannonball — the camera must track the ball with no trailing gap. In edit mode, right-mouse WASD must be smooth. Report what was seen either way; do not claim it from the tests alone.

- [ ] **Step 4: Commit**

```bash
git add src/engine_core/README.md src/engine_core/TaskScheduler.hpp src/engine_core/DataModel.hpp src/engine_core/DataModelState.hpp
git commit -m "Document the render window running script handlers"
```

---

### Task 8: Window script writes authorize as sim writes

Live use found the gap the rigs hid: with engine threads running, `authorize` (DataModel.cpp:~306) admits a window write only for `visual_only`-tagged instances, so SceneCamera's `camera.Transform = ...` raised "Transform cannot be written in a render step" in the real studio (sandbox rigs run with `threads_running` false, where authorize admits everything). Ruling: window Lua writes authorize like sim writes.

**Files:**
- Modify: `src/engine_core/DataModel.hpp`/`.cpp` (window-script flag + authorize), `src/engine_core/DataModelState.hpp` (the flag), `src/engine_core/ScriptRuntime.cpp` (`render_step` sets it around `invoke_render`), `src/engine_core/README.md` (path-B row)
- Test: `sandbox/tests.cpp` (new `[RW9]`; rework `[RW7]`)

**Interfaces:**
- Produces: `DataModel::set_window_script(bool)` (or equivalent on the window begin/end path) — set only by the render thread while it holds the write lock inside the window.

Steps (same TDD rhythm as the other tasks):
1. Failing test RW9, using RW7's harness (threads_running on, render thread registered, write lock held, window open): an UNTAGGED part's `Transform` written from a window handler succeeds — no Lua error, the DataModel shows the new transform, the pump's published snapshot for that frame shows it, and no deferred violation is left. Also write a non-visual scriptable property (pick one that reaches a plain reflected setter, e.g. the part's `Name` via core field or a registered property that calls `authorize` — read the setters and choose one that would have been refused before) and assert it sticks.
2. Watch RW9 fail with the current refusal.
3. Implement: a `window_script` flag beside `prerender_window` in DataModelState, set/cleared by `ScriptRuntime::render_step` around `invoke_render` through a small DataModel method; `authorize`'s render-thread branch returns true when the window is open and the flag is set, before the visual-only check. Transform writes from that context take the ForceSimWrite route so they land in this frame's snapshot (trace `apply_transform`/`visual_target` and pass the force flag from the script context rather than changing the C++ callers). Keep Task 5's refusal net untouched.
4. Rework RW7: its refusal premise is gone for ordinary properties; repurpose it to assert the old refusal case now succeeds end to end while pcall sees no error, and leave one comment that the raise-on-refusal net is currently unexercised by design.
5. Full suites; update the README path-B row to say script window writes are unrestricted and recorded `PreRenderDataModel`; commit.
