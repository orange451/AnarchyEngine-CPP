# One Engine Loop, with Physics Interpolation: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the engine's SimulationThread/RenderThread pair with one engine loop (sim step, then the RenderStepped window, then the snapshot copy), and draw simulated bodies blended through their last physics substep, so a RenderStepped camera following a moving body stays steady.

**Architecture:** `Engine` runs one thread whose pass is `wait → run_frame(wall)`. `run_frame` holds one DataModel write lock across the sim step and the window. Rules that used to ask "which thread" ask "is the window open". A new `PoseBlend` (owned by `DataModelState`) captures each body's pose before and after the last substep. The window swaps in `lerp(before, after, alpha) * inverse(after) * current` with quiet setters, and swaps the true poses back after the copy.

**Tech Stack:** C++20, MSVC, CMake, Catch2 (the `sandbox` test target), Box3D, flecs, Luau.

**Spec:** `docs/superpowers/specs/2026-10-12-single-engine-loop-design.md`

## Global Constraints

- Engine distances are "units". Never write "studs" in code, comments, or docs.
- Name engine APIs after the engine, not Roblox.
- The engine's one thread has `ThreadRole::Simulation`. `ThreadRole::Render` and `Phase::PostRender` are removed.
- Pacing: wait for the window's paint (`note_client_frame`) and fall back to 16.7 ms (`kSimulationClientFallback`). A paced engine (`set_pace_hz`) waits for its slot.
- Interpolation latency is at most one physics substep (1/240 s). The sim step, Heartbeat, and physics read only true poses. Only the window sees blended ones.
- A write a script makes in the window wins over the swap-out.
- Match the surrounding code's comment density and voice: full sentences, "units", no Roblox names in new APIs.
- Build: `cmake --build build --config Debug --target sandbox`, then run `build/Debug/sandbox.exe "<test name or [tag]>"`. Before the final task's sign-off, also build Release (`cmake --build build --config Release`), because "rebuild" means `build/Release/AnarchyStudio.exe` too.
- Windows test pitfalls: `sleep_for` has ~15.6 ms ticks, so use deadline spin loops for timing. Don't assert tight wall-clock rates.

## Review Focus

1. **An IDE edit while playing or paused** (`on_simulation` from the UI thread, `perform_paused_edit`) must still run under the lock and never deadlock against the merged loop, which no longer releases `pause_mu_` in the middle of a frame. Task 1 adds a test.
2. **The editor camera while stopped** (SceneCamera's RenderStepped) must still run once per paint, not once per 16 ms tool tick. Task 1 adds a test.
3. **A player turning** (PlayerInput writes `controller.Transform` every Heartbeat after the substeps) must still blend, not drop out. That is why the blend rewinds the *current* pose instead of requiring `current == after`. Task 3 adds a test.
4. **A body destroyed or Stop pressed between frames** must leave no stale entry that writes into a reused slot. Entries are cleared at play start and Stop, and ids are generation-checked. Task 3 adds a test.
5. **A C++ render job** (not Lua) writing a non-visual-only instance in the window must still be refused with a deferred violation, not allowed because "it's the sim thread now". Task 1 adds a test.

---

## File Structure

- Modify `src/engine_core/Engine.{hpp,cpp}`: one `engine_loop`, a public `run_frame(double wall)`, folded pace setters, no `render_loop`, no `RenderStepTime`.
- Modify `src/engine_core/DataModel.{hpp,cpp}` and `DataModelState.hpp`: `set_engine_thread`, window-keyed checks, `pose_blend()`, and the quiet `store_visual_transform` / `spatial_transform`.
- Modify `src/engine_core/TaskScheduler.cpp`, `Events.cpp`, `ScriptRuntime.cpp`, `SnapshotPump.cpp`: role checks become window checks.
- Modify `src/engine_core/types.hpp`, `Contract.cpp`, `ScriptBindings.cpp`: drop `ThreadRole::Render` and `Phase::PostRender`.
- Create `src/engine_core/PoseBlend.{hpp,cpp}`: capture, blend math, swap in and out.
- Modify `src/engine_instances/PhysicsBase.{hpp,cpp}`: record in `store_simulated`, add the quiet `store_pose`.
- Modify `src/engine_core/PhysicsWorld.cpp` (`simulate`): open and close the capture around step, pull, and place_held when armed.
- Modify `src/engine_core/DataModelPlace.cpp`: clear the blend at play start and Stop.
- Modify `src/runner/Runner.cpp`: the folded pace calls.
- Modify the tests in `sandbox/tests.cpp`, `sandbox/support.hpp`, `sandbox/billboard_tests.cpp`, and `sandbox/profiler_tests.cpp`. Create `sandbox/pose_blend_tests.cpp` and add it to `CMakeLists.txt`'s sandbox list.
- Update the docs: `src/engine_core/README.md`, header comments, and a note in the 2026-10-03 window spec.

---

### Task 1: One engine loop

**Files:**
- Modify: `src/engine_core/Engine.hpp`, `src/engine_core/Engine.cpp` (`render_loop` 536–685 and `simulation_loop` 291–533 merge into `engine_loop` and `run_frame`)
- Modify: `src/engine_core/DataModel.hpp:219-224`, `src/engine_core/DataModel.cpp:183-221, 296-306, 327-331, 376-400, 676-681, 741-744`, `src/engine_core/DataModelState.hpp:158-162`
- Modify: `src/engine_core/TaskScheduler.cpp:121-136, 423-430`, `src/engine_core/Events.cpp:299-304, 447-449, 461-466`, `src/engine_core/ScriptRuntime.cpp:658-673`, `src/engine_core/SnapshotPump.cpp:162-165`
- Modify: `src/engine_core/types.hpp:27-40, 88-95`, `src/engine_core/Contract.cpp:12-19`, `src/engine_core/ScriptBindings.cpp:806-807`
- Modify: `src/runner/Runner.cpp:37-40`
- Test: `sandbox/tests.cpp`, `sandbox/support.hpp:121-160`, `sandbox/billboard_tests.cpp:125-132`, `sandbox/profiler_tests.cpp:65-66`

**Interfaces:**
- Produces:
  - `void Engine::run_frame(double wall)`. One frame on the calling thread: the sim step when resumed, the stopped tick when paused, then the window and the copy. Tests call it on an Engine that was never started.
  - `Engine::set_pace_hz(double)` and `Engine::set_client_sync(bool)`. The four split setters are deleted.
  - `Engine::simulation_thread_id()` returns the engine thread. `render_thread_id()` is deleted.
  - `DataModel::set_engine_thread(std::thread::id)` replaces `set_thread_ids`.
  - `DataModel::engine_thread() const -> bool` is true when threads aren't running or the caller is the engine thread.
  - `render_thread_id()` is deleted from DataModel.
  - Inside `run_frame` the window block is a private `void Engine::run_window(double dt)`. Task 3 adds its swap calls there.

- [ ] **Step 1: Rewrite the two-thread tests to state the one-loop behavior (they fail to compile until Step 3)**

Replace T1, T2, T3, T4, T22, and the three `[pace]` tests in `sandbox/tests.cpp` (lines 103–195, 1247–1258, 1416–1516) with the following. Delete T3 and T4 outright: overlapping sim and present is gone by design.

```cpp
TEST_CASE("the engine runs on one thread", "[T1]") {
    engine_core::Engine engine;
    std::atomic<std::thread::id> heartbeat_thread{};
    std::atomic<std::thread::id> render_thread{};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        heartbeat_thread.store(std::this_thread::get_id());
    });
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) {
        render_thread.store(std::this_thread::get_id());
    });
    engine.start();
    engine.resume();
    wait_until([&] { return heartbeat_thread.load() != std::thread::id{} && render_thread.load() != std::thread::id{}; });
    REQUIRE(engine.simulation_thread_id() != std::thread::id{});
    REQUIRE(heartbeat_thread.load() == engine.simulation_thread_id());
    REQUIRE(render_thread.load() == engine.simulation_thread_id());
    engine.stop();
}

TEST_CASE("DataModel write during Perform is rejected", "[T2]") {
    engine_core::Engine engine;
    struct Probe : engine_core::IRenderer {
        engine_core::GameObject* object = nullptr;
        std::atomic<int> hits{0};
        std::string message;
        void perform(const engine_core::VisualSnapshot&) override {
            if (hits.load() != 0) {
                return;
            }
            try {
                object->set_transform(T0());
            } catch (const engine_core::ContractViolation& ex) {
                message = ex.what();
                hits.store(1);
            }
        }
        void present() override {}
    } probe;
    probe.object = &engine.datamodel().create_game_object();
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return probe.hits.load() == 1; });
    // Perform runs after the lock is released: the write has no lock to hold.
    REQUIRE(probe.message.find("DataModelLock") != std::string::npos);
    engine.stop();
}
```

> T2's exact expected text comes from whatever `set_transform` raises without the lock today when called from an unlocked thread. Before writing the REQUIRE, run the test once and copy the message's distinctive fragment. If an unlocked write is *not* refused (other threads normally queue, path E), assert instead that the GameObject's transform is unchanged after `engine.stop()`, and rename the case "a write during Perform does not land in the frame".

```cpp
TEST_CASE("RenderStepped runs every pass while paused", "[T22]") {
    engine_core::Engine engine;
    std::atomic<int> stepped{0};
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) { stepped.fetch_add(1); });
    engine.start();
    wait_until([&] { return stepped.load() > 2; });
    REQUIRE(engine.paused());
    REQUIRE(engine.sim_frame_count() == 0);
    engine.stop();
}

TEST_CASE("a paused engine with client sync runs the window once per paint", "[pace]") {
    engine_core::Engine engine;
    engine.set_pace_hz(0.0);
    engine.set_client_sync(true);
    std::atomic<int> stepped{0};
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) { stepped.fetch_add(1); });
    engine.start();
    wait_until([&] { return stepped.load() > 0; });
    // Paints every 4 ms, a 250 Hz window, well above the 16 ms tool tick.
    const int first = stepped.load();
    const auto paced_at = std::chrono::steady_clock::now();
    auto next_paint = paced_at;
    for (int i = 0; i < 60; ++i) {
        next_paint += std::chrono::milliseconds(4);
        while (std::chrono::steady_clock::now() < next_paint) {
            std::this_thread::yield();
        }
        engine.note_client_frame();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - paced_at).count();
    const double hz = static_cast<double>(stepped.load() - first) / seconds;
    engine.stop();
    INFO("windows a second: " << hz);
    REQUIRE(hz > 120.0);
}
```

Keep the old "a paced simulation steps at the rate it is given" test, changing only `set_simulation_pace_hz(60.0)` to `set_pace_hz(60.0)`. Keep "client sync steps an uncapped simulation with each paint", replacing its four setter lines with `engine.set_pace_hz(0.0); engine.set_client_sync(true);`. Delete "client sync keeps an uncapped render loop with the window": the case above covers it. At line 5413, replace the two setters with `engine.set_pace_hz(240);`. In `sandbox/profiler_tests.cpp:65-66`, replace them with `engine.set_pace_hz(60.0);`.

Add the Review Focus tests 1 and 5:

```cpp
TEST_CASE("an edit from another thread while playing lands under the lock", "[edit]") {
    engine_core::Engine engine;
    engine_core::GameObject& object = engine.datamodel().create_game_object();
    const engine_core::InstanceId id = object.id();
    engine.start();
    engine.resume();
    wait_until([&] { return engine.sim_frame_count() > 2; });
    std::atomic<bool> ran{false};
    engine.on_simulation([&](engine_core::DataModel& game) {
        REQUIRE_FALSE(game.game_object(id)->set_transform(T1()));
        ran.store(true);
    });
    wait_until([&] { return ran.load(); });
    engine.pause();
    std::atomic<bool> paused_ran{false};
    engine.on_simulation([&](engine_core::DataModel& game) {
        REQUIRE_FALSE(game.game_object(id)->set_transform(T0()));
        paused_ran.store(true);
    });
    REQUIRE(paused_ran.load());
    engine.stop();
    REQUIRE(near(engine.datamodel().game_object(id)->transform(), T0()));
}

TEST_CASE("a C++ render job writing a plain part is refused in the window", "[T21]") {
    engine_core::Engine engine;
    engine_core::GameObject& object = engine.datamodel().create_game_object();
    std::atomic<int> tries{0};
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) {
        if (tries.fetch_add(1) == 0) {
            (void)object.set_transform(T1());
        }
    });
    engine.start();
    wait_until([&] { return tries.load() > 2 && engine.contract_count() > 0; });
    engine.stop();
    REQUIRE_FALSE(near(object.transform(), T1()));
}
```

In T21 (`sandbox/tests.cpp:1094`) and T23 (1260), delete the PostRender halves (the `Phase::PostRender` binds and their REQUIREs), and change each `ThreadRole::Render` assertion to `ThreadRole::Simulation`. In T23, the drain refusal now reads "EventQueue::drain during RenderStepped or PreRender" (the window check), not "SimulationThread". Update that REQUIRE's substring to `"during RenderStepped"`.

- [ ] **Step 2: Rewrite `ScriptRig::render` to run the window on the calling thread**

In `sandbox/support.hpp`, replace `render()`'s body. Keep its doc comment's first sentence, and replace the rest with "on this thread, as Engine::run_window runs it".

```cpp
    void render(double dt = 1.0 / 60.0) {
        const std::thread::id saved_engine = game.simulation_thread_id();
        const bool saved_running = game.threads_running();
        bool violated = false;
        const char* reason = nullptr;
        game.set_engine_thread(std::this_thread::get_id());
        game.set_threads_running(true);
        {
            engine_core::DataModelLock lock(game, engine_core::DataModelLock::Write);
            game.set_prerender_window(true);
            scheduler.run_phase(engine_core::Phase::RenderStepped, dt);
            game.set_prerender_window(false);
        }
        violated = game.take_deferred_violation(&reason);
        game.set_threads_running(saved_running);
        game.set_engine_thread(saved_engine);
        if (violated) {
            ++render_violations;
            last_render_violation = reason != nullptr ? reason : "";
        }
    }
```

In `sandbox/billboard_tests.cpp:125-132`, delete the two `set_thread_role(ThreadRole::Render)`/restore lines around `override_visual`. The window flag alone admits it now. Grep `sandbox/` for every other `set_thread_ids(` (tests.cpp 4161–4173, 5003–5014, 5060–5072, 5257, 5517–5530) and change each to `set_engine_thread(<the id they passed as render>)`. Those tests set up a render thread to run a window, and that thread is now the engine thread. Where a test spawned a `std::thread` only to be "the render thread", run its body inline on the test thread instead.

- [ ] **Step 3: Make the DataModel rules window-keyed**

`DataModelState.hpp:158-159`: replace `simulation_thread` and `render_thread` with one `std::thread::id engine_thread{};`.

`DataModel.hpp:219-224`: replace `set_thread_ids` and `render_thread_id` with:

```cpp
    // The engine's one thread: the sim step and the render window both run on it.
    void set_engine_thread(std::thread::id engine);
    // True while threads are not running, or on the engine thread.
    bool engine_thread() const;
```

`simulation_thread_id()` returns `state_->engine_thread`.

`DataModel.cpp`, replacing the bodies shown in the spec's "Thread rules" row:

```cpp
void DataModel::set_engine_thread(std::thread::id engine) { state_->engine_thread = engine; }

std::thread::id DataModel::simulation_thread_id() const { return state_->engine_thread; }

bool DataModel::engine_thread() const {
    return !state_->threads_running || std::this_thread::get_id() == state_->engine_thread;
}

bool DataModel::gameplay_thread() const {
    if (!state_->threads_running) {
        return true;
    }
    const std::thread::id self = std::this_thread::get_id();
    // The engine thread is the sim outside the window; inside it, only a
    // script (mutation_thread) or a visual-only write (authorize) may write.
    return (self == state_->engine_thread && !state_->prerender_window) || self == state_->edit_owner;
}

bool DataModel::mutation_thread() const {
    if (gameplay_thread()) {
        return true;
    }
    // A script running in the render window: the engine holds the write lock
    // across the whole frame, and copies only after the window's phases.
    // A C++ render job runs with window_script false and keeps today's limits.
    return std::this_thread::get_id() == state_->engine_thread && state_->prerender_window &&
           state_->window_script;
}
```

`unlock_write` (296–306): change `self != state_->simulation_thread && self != state_->render_thread` to `self != state_->engine_thread`.

`current_origin`:

```cpp
WriteOrigin DataModel::current_origin() const {
    if (state_->threads_running && state_->prerender_window &&
        std::this_thread::get_id() == state_->engine_thread) {
        return WriteOrigin::PreRenderDataModel;
    }
    return WriteOrigin::Simulation;
}
```

`authorize`:

```cpp
bool DataModel::authorize(const Slot& part, bool force_sim_write) {
    if (!state_->threads_running) {
        return true;
    }
    const std::thread::id self = std::this_thread::get_id();
    if (self == state_->edit_owner) {
        return true;
    }
    if (self == state_->engine_thread && state_->prerender_window) {
        // A write made while Lua runs in the window (a RenderStepped handler,
        // or a Wait resumption there) authorizes as a sim write would, before
        // the visual_only check: SceneCamera's `camera.Transform = ...` needs
        // it. A C++ render job outside Lua still needs visual_only or ForceSimWrite.
        if (force_sim_write || state_->window_script ||
            has_tag(ecs_world(), part.entity, state_->ecs_ids.visual_only)) {
            return true;
        }
        return reject_write("render-step DataModel write requires visual_only or ForceSimWrite");
    }
    return true;
}
```

`destroy` (676–681): replace the `render_thread` check with:

```cpp
        if (std::this_thread::get_id() == state_->engine_thread && state_->prerender_window) {
            contract_fail("destroy from a render job");
        }
```

`queues_visual_write` (741–744):

```cpp
bool DataModel::queues_visual_write() const {
    // A paused edit writes now, like the engine thread. Any other thread enqueues.
    return state_->threads_running && !gameplay_thread() && std::this_thread::get_id() != state_->engine_thread;
}
```

- [ ] **Step 4: Scheduler, events, scripts, pump**

`types.hpp`: delete `PostRender` from `Phase`, make `kPhaseCount = static_cast<int>(Phase::PreRender) + 1`, and rewrite the per-enumerator comments to say "the engine thread, in the sim step" and "the engine thread, in the window, before the copy". Delete `ThreadRole::Render` (lines 88–95) and its name in `Contract.cpp:12-19`. `ScriptBindings.cpp:806-807`: drop `"PostRender"` from `kPhases`.

`TaskScheduler.cpp`: `is_render_phase` keeps RenderStepped and PreRender only. Delete `render_phase_thread_message`. In `run_phase`:

```cpp
void TaskScheduler::run_phase(Phase phase, double dt) {
    if (thread_role() != ThreadRole::Simulation) {
        contract_fail("phases run on the engine thread");
    }
```

(The window phases' "inside the window" rule is enforced by `EventQueue::invoke_render`, which every window delivery goes through.)

`Events.cpp`:
- `invoke_render`: delete the role check and keep the `prerender_open_` check.
- `post`: `const bool on_sim = thread_role() == ThreadRole::Simulation && !(prerender_open_ != nullptr && *prerender_open_);`
- `drain`: keep both checks as they are. The role check's message becomes "EventQueue::drain runs on the engine thread".

`ScriptRuntime.cpp` `assert_lua_thread`:

```cpp
void ScriptRuntime::assert_lua_thread() const {
    // Lua runs wherever the DataModel write lock is held: the engine thread's
    // sim step or window, or a paused edit.
    if (game_ != nullptr && game_->prerender_window()) {
        if (!game_->engine_thread()) {
            contract_fail("Lua runs on the engine thread");
        }
        return;
    }
    if (game_ != nullptr && !game_->on_gameplay_thread()) {
        contract_fail("Lua runs on the engine thread");
    }
}
```

`SnapshotPump.cpp:162-165`: `if (!window_open_) {` (drop the role half).

Grep `src/` for any remaining `ThreadRole::Render`, `render_thread_id`, `set_thread_ids`, and `Phase::PostRender`, and fix each the same way. The explore pass found nothing beyond the sites above, but confirm.

- [ ] **Step 5: Merge the loops**

`Engine.hpp`:
- Delete `render_loop`, `render_`, `render_id_`, `render_ready_`, `render_pace_hz_`, `render_client_sync_`, `simulation_client_sync_`, the `RenderStepTime` struct (35–50), and the declarations of the four split setters and `render_thread_id`.
- Rename `simulation_loop` to `engine_loop`. Keep `simulation_id_` and `simulation_ready_`, which now name the engine thread.
- Add:

```cpp
    // 0 runs as fast as the machine allows.
    void set_pace_hz(double hz);
    // An unpaced loop waits for note_client_frame, one frame per paint, and
    // falls back to about 60 Hz when nothing paints.
    void set_client_sync(bool enabled);
    // One frame on the calling thread: the sim step (or, paused, the stopped
    // tick), then the window and the copy. engine_loop calls it after its wait;
    // tests call it on an engine that was never started.
    void run_frame(double wall);
```

and private members `double pace_hz_ = 0; std::atomic<bool> client_sync_{false}; double accumulator_ = 0; std::chrono::steady_clock::time_point last_tool_{}; void run_window(double dt);`. Replace the two-loop class comment (52–54) with: "One engine thread steps the DataModel and then prepares that frame's snapshot under the same write lock. The UI thread draws from SceneFeed."

`Engine.cpp`:
- `set_pace_hz` sets `pace_hz_`. `set_client_sync` stores `client_sync_`.
- `note_client_frame`'s comment becomes "The engine loop may be waiting on this paint."
- `start()` spawns only `simulation_ = std::thread([this] { engine_loop(); });`, waits for `simulation_ready_`, and calls `game_.set_engine_thread(simulation_id_)`.
- `stop()` joins only `simulation_`.

`engine_loop` is the start handshake (as `simulation_loop`'s 295–304), `set_thread_role(ThreadRole::Simulation)`, and `profiler::register_thread("Engine")`, then:

```cpp
    auto last = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_frame;
    bool was_paused = true;
    while (running_.load()) {
        // Sample before the frame so a paint that arrives during it is not missed.
        std::uint64_t client_seen = 0;
        const bool wait_for_client = client_sync_.load() && !(pace_hz_ > 0.0);
        if (wait_for_client) {
            std::lock_guard<std::mutex> guard(client_frame_mu_);
            client_seen = client_frames_;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        double wall = std::chrono::duration<double>(frame_start - last).count();
        last = frame_start;
        const bool paused_now = paused();
        // The time spent paused is not a simulation step, nor slots to catch up on.
        if (was_paused && !paused_now) {
            wall = 0;
            next_frame = {};
        }
        was_paused = paused_now;
        run_frame(std::clamp(wall, 0.0, 0.1));

        if (pace_hz_ > 0) {
            WaitForSlot(next_frame, frame_start, pace_hz_);
        } else if (wait_for_client) {
            // Steps with the window's paint. stop() wakes this wait as well.
            std::unique_lock<std::mutex> guard(client_frame_mu_);
            client_frame_cv_.wait_until(guard, frame_start + kSimulationClientFallback, [&] {
                return !running_.load() || client_frames_ != client_seen;
            });
        } else if (paused_now) {
            // Unpaced and paused: the editor's tick rate, woken early by resume or stop.
            std::unique_lock<std::mutex> guard(pause_mu_);
            pause_cv_.wait_for(guard, kToolInterval, [&] { return !paused_ || !running_.load(); });
        }
    }
```

`run_frame(double wall)` is `profiler::frame_boundary()` followed by one `DataModelLock lock(game_, DataModelLock::Write)` scope that contains:
- **Paused:** if `std::chrono::steady_clock::now() - last_tool_ >= kToolInterval`, the stopped tick: the body of `simulation_loop`'s 327–367 as two `guarded_step`s *without* their own locks. `tool_dt` is the clamped time since `last_tool_`. Then set `last_tool_`.
- **Resumed:** the existing step body (420–515) without its own lock. `step_dt` is computed as at 393–410, with `clock_ != nullptr` taking `clock_->delta_seconds()` in place of `wall`, and `accumulator_` replacing the local. Then `last_substeps_.store(substeps); sim_frames_.fetch_add(1);`.
- Then `run_window(paused_now ? wall : step_dt)`.

After the lock scope it does what render_loop's 630–671 did:
- `take_deferred_violation` into `contract_count_`;
- `pump_.finish_copy(); pump_.publish();`;
- Perform and Present in a `guarded_step`;
- `present_count_.fetch_add(1)`.

Delete the PostRender block. Delete the `scripts_->drop_render_frames()` call in `resume()` only if `drop_render_frames` has no other caller. It still feeds the console VM's summed RenderStepped, so keep it.

`run_window(double dt)` is render_loop's 597–624 without the lock: `pump_.begin_prerender_window(game_)`, a guarded RenderStepped with `dt`, a guarded PreRender with `dt`, `pump_.end_prerender_window(game_)`, and a guarded `take_changes`.

`Runner.cpp:37-40` becomes `simulation->set_pace_hz(0.0); simulation->set_client_sync(true);`. Update the comment above it if it mentions two loops.

Rename the profiler's fixed "Sim" and "Render" rows to one "Engine" row: grep `src/` for `kFixedRows` and `register_thread("Render")`. `README.md:133` lists the rows; update it to "Engine", "Render draw", "UI", "GPU". Any profiler test that names the rows changes with it.

- [ ] **Step 6: Build and run the whole suite**

Run: `cmake --build build --config Debug --target sandbox`, then `build/Debug/sandbox.exe`.
Expected: everything passes. The release-test-flakes memory lists six known Windows timing flakes in `[explorer]`. Rerun those alone before treating them as failures.

- [ ] **Step 7: Commit**

```bash
git checkout -b single-engine-loop
git add src/engine_core src/runner/Runner.cpp sandbox/tests.cpp sandbox/support.hpp sandbox/billboard_tests.cpp sandbox/profiler_tests.cpp README.md
git commit -m "Engine: one loop steps the simulation, then prepares that frame's snapshot"
```

(End the message body with the attribution lines from the session's system reminder.)

---

### Task 2: PoseBlend, the capture and the blend

**Files:**
- Create: `src/engine_core/PoseBlend.hpp`, `src/engine_core/PoseBlend.cpp`
- Modify: `CMakeLists.txt` (add `PoseBlend.cpp` to the engine_core sources beside `PhysicsWorld.cpp`, and `sandbox/pose_blend_tests.cpp` to the sandbox list at ~1007–1040)
- Test: `sandbox/pose_blend_tests.cpp`

**Interfaces:**
- Consumes: `Matrix4`, `matrix4_lerp`, `matrix4_inverse`, `matrix4_multiply`, `same_matrix4` (`engine_datatypes/Matrix4.hpp`), and `InstanceId` (`types.hpp`).
- Produces (used by Task 3):

```cpp
#pragma once

#include "Matrix4.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine_core {

// Simulated bodies drawn between their last two physics substeps. The engine
// arms it before a frame's last substep; PhysicsWorld opens a capture around
// that substep's Box3D step, pull, and held placement; every pose written
// meanwhile is recorded with what it replaced. The render window then draws
// each recorded instance's current pose rewound by the part of that substep
// not yet due: lerp(before, after, alpha) * inverse(after) * current. A pose
// written after the substep (a script turning a character, a held tool put
// on its hand) keeps the substep's motion. Engine thread only.
class PoseBlend {
public:
    enum class Kind : std::uint8_t { Body, Spatial };

    struct Entry {
        InstanceId id = 0;
        Kind kind = Kind::Body;
        Matrix4 before;
        Matrix4 after;
    };

    void arm() { armed_ = true; }
    void disarm() { armed_ = false; }
    bool armed() const { return armed_; }

    // Clears the entries: this substep's motion replaces the last one's.
    void begin_capture();
    void end_capture() { capturing_ = false; }
    bool capturing() const { return capturing_; }

    // Nothing unless capturing. An instance written twice keeps its first
    // before and its last after.
    void record(InstanceId id, Kind kind, const Matrix4& before, const Matrix4& after);
    // Forgets every entry: play starting or stopping.
    void clear();
    const std::vector<Entry>& entries() const { return entries_; }

    // current rewound by (1 - alpha) of entry's motion. current == entry.after
    // gives lerp(before, after, alpha); alpha 1 gives current exactly.
    static Matrix4 blended(const Entry& entry, const Matrix4& current, double alpha);

private:
    std::vector<Entry> entries_;
    std::unordered_map<InstanceId, std::size_t> index_;
    bool armed_ = false;
    bool capturing_ = false;
};

}  // namespace engine_core
```

- [ ] **Step 1: Write the failing tests**

`sandbox/pose_blend_tests.cpp`:

```cpp
#include "PoseBlend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using engine_core::Matrix4;
using engine_core::PoseBlend;

namespace {

bool near(const Matrix4& a, const Matrix4& b, float tolerance = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

Matrix4 at(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

Matrix4 turned(float x, float radians) {
    return engine_core::matrix4_multiply(at(x, 0, 0), engine_core::matrix4_axis_angle({0, 1, 0}, radians));
}

}  // namespace

TEST_CASE("PB1 records only while capturing, first before and last after", "[pose_blend]") {
    PoseBlend blend;
    blend.record(7, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0));
    REQUIRE(blend.entries().empty());
    blend.begin_capture();
    blend.record(7, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0));
    blend.record(7, PoseBlend::Kind::Body, at(1, 0, 0), at(2, 0, 0));
    blend.end_capture();
    REQUIRE(blend.entries().size() == 1);
    REQUIRE(near(blend.entries()[0].before, at(0, 0, 0)));
    REQUIRE(near(blend.entries()[0].after, at(2, 0, 0)));
    blend.begin_capture();
    REQUIRE(blend.entries().empty());
}

TEST_CASE("PB2 blends position and rotation through the substep", "[pose_blend]") {
    const PoseBlend::Entry moved{1, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0)};
    REQUIRE(near(PoseBlend::blended(moved, at(1, 0, 0), 0.0), at(0, 0, 0)));
    REQUIRE(near(PoseBlend::blended(moved, at(1, 0, 0), 0.25), at(0.25f, 0, 0)));
    REQUIRE(near(PoseBlend::blended(moved, at(1, 0, 0), 1.0), at(1, 0, 0)));
    const PoseBlend::Entry spun{2, PoseBlend::Kind::Body, turned(0, 0.f), turned(0, 0.4f)};
    REQUIRE(near(PoseBlend::blended(spun, turned(0, 0.4f), 0.5), turned(0, 0.2f)));
}

TEST_CASE("PB3 a pose written after the substep keeps its motion", "[pose_blend]") {
    // A script turned the character after physics moved it 1 unit along X.
    const PoseBlend::Entry moved{1, PoseBlend::Kind::Spatial, at(0, 0, 0), at(1, 0, 0)};
    const Matrix4 scripted = turned(1, 1.0f);
    const Matrix4 shown = PoseBlend::blended(moved, scripted, 0.5);
    REQUIRE(near(shown, turned(0.5f, 1.0f)));
}

TEST_CASE("PB4 clear forgets every entry", "[pose_blend]") {
    PoseBlend blend;
    blend.begin_capture();
    blend.record(3, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0));
    blend.end_capture();
    blend.clear();
    REQUIRE(blend.entries().empty());
    REQUIRE_FALSE(blend.capturing());
}
```

- [ ] **Step 2: Run them to verify they fail**

Add `sandbox/pose_blend_tests.cpp` to the sandbox list in `CMakeLists.txt`, then run `cmake --build build --config Debug --target sandbox`.
Expected: compile error, `PoseBlend.hpp` not found.

- [ ] **Step 3: Implement**

`src/engine_core/PoseBlend.cpp`:

```cpp
#include "PoseBlend.hpp"

namespace engine_core {

void PoseBlend::begin_capture() {
    entries_.clear();
    index_.clear();
    capturing_ = true;
}

void PoseBlend::record(InstanceId id, Kind kind, const Matrix4& before, const Matrix4& after) {
    if (!capturing_) {
        return;
    }
    const auto found = index_.find(id);
    if (found != index_.end()) {
        entries_[found->second].after = after;
        return;
    }
    index_.emplace(id, entries_.size());
    entries_.push_back(Entry{id, kind, before, after});
}

void PoseBlend::clear() {
    entries_.clear();
    index_.clear();
    armed_ = false;
    capturing_ = false;
}

Matrix4 PoseBlend::blended(const Entry& entry, const Matrix4& current, double alpha) {
    if (alpha >= 1.0) {
        return current;
    }
    const Matrix4 drawn = matrix4_lerp(entry.before, entry.after, alpha < 0.0 ? 0.0 : alpha);
    return matrix4_multiply(matrix4_multiply(drawn, matrix4_inverse(entry.after)), current);
}

}  // namespace engine_core
```

Add `src/engine_core/PoseBlend.cpp` to the engine_core source list in `CMakeLists.txt`, next to `PhysicsWorld.cpp`.

- [ ] **Step 4: Run them to verify they pass**

Run `build/Debug/sandbox.exe "[pose_blend]"`.
Expected: 4 test cases pass. If PB3 is off in the rotation columns, `matrix4_lerp`'s scale handling is the suspect. Read `Matrix4.cpp:360-380` before changing the formula.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PoseBlend.hpp src/engine_core/PoseBlend.cpp sandbox/pose_blend_tests.cpp CMakeLists.txt
git commit -m "PoseBlend: capture a substep's poses and rewind a pose through it"
```

---

### Task 3: Capture in physics, blend in the window

**Files:**
- Modify: `src/engine_core/DataModelState.hpp` (a `PoseBlend pose_blend;` member), `src/engine_core/DataModel.hpp` and `.cpp` (`pose_blend()`, `store_visual_transform`, `spatial_transform`, and recording in `write_simulated_transform` at 1135–1143)
- Modify: `src/engine_instances/PhysicsBase.hpp:107`, `src/engine_instances/PhysicsBase.cpp:218-221`
- Modify: `src/engine_core/PhysicsWorld.cpp` (`Impl::simulate`, 1853–1883)
- Modify: `src/engine_core/Engine.cpp` (`run_frame`'s substep loop and `run_window`)
- Modify: `src/engine_core/DataModelPlace.cpp:42, 84`
- Modify: `src/engine_core/PoseBlend.hpp` and `.cpp` (`swap_in` and `swap_out`)
- Test: `sandbox/pose_blend_tests.cpp`

**Interfaces:**
- Consumes: `PoseBlend` (Task 2), and `Engine::run_frame` / `run_window` (Task 1).
- Produces:
  - `PoseBlend& DataModel::pose_blend()`
  - `void DataModel::store_visual_transform(InstanceId id, const Matrix4& transform)`: the ECS write and a snapshot note, with no watchers, history, or Changed.
  - `bool DataModel::spatial_transform(InstanceId id, Matrix4& out) const`
  - `void PhysicsBase::store_pose(const Matrix4& transform)`: sets `transform_` only.
  - Free functions in `PoseBlend.hpp`: `void swap_in_blend(DataModel& game, double alpha);` and `void swap_out_blend(DataModel& game);`. The swapped list is a private `std::vector<Swap> swaps_` inside PoseBlend, reached by these two `friend` functions.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/pose_blend_tests.cpp`. The Engine is never started, so `run_frame` runs on the test thread. `Workspace.Gravity = 0` with a set velocity gives constant-speed motion.

```cpp
#include "Engine.hpp"
#include "PhysicsObject.hpp"
#include "support.hpp"

#include <vector>

namespace {

struct Drive {
    SimRole role;
    engine_core::Engine engine;
    engine_core::PhysicsObject* body = nullptr;
    std::vector<float> drawn;  // body x read in each frame's RenderStepped

    explicit Drive(float speed) {
        engine_core::DataModel& game = engine.datamodel();
        REQUIRE_FALSE(workspace_service(game)->set_gravity(0.0));
        body = &game.create<engine_core::PhysicsObject>();
        REQUIRE_FALSE(body->set_transform(at(0, 10, 0)));
        REQUIRE_FALSE(body->set_size({1, 1, 1}));
        game.set_parent(body->id(), workspace_of(game));
        game.start_simulation();
        engine.set_timing(1.0 / 120.0, 1.0 / 240.0);
        engine.resume();
        REQUIRE_FALSE(body->set_velocity({speed, 0, 0}));
        engine.scheduler().bind(engine_core::Phase::RenderStepped, [this](double) {
            drawn.push_back(body->transform().m[12]);
        });
    }
};

}  // namespace

TEST_CASE("PB5 uneven frames draw evenly spaced", "[pose_blend]") {
    Drive drive(30.f);
    const double walls[] = {0.007, 0.009, 0.008, 0.0075, 0.0085, 0.007, 0.009, 0.008};
    for (int warm = 0; warm < 4; ++warm) {
        drive.engine.run_frame(0.008);
    }
    drive.drawn.clear();
    for (double wall : walls) {
        drive.engine.run_frame(wall);
    }
    for (std::size_t i = 1; i < drive.drawn.size(); ++i) {
        INFO("frame " << i);
        REQUIRE(std::fabs((drive.drawn[i] - drive.drawn[i - 1]) - 30.f * static_cast<float>(walls[i])) < 1e-3f);
    }
}

TEST_CASE("PB6 the sim step reads true poses", "[pose_blend]") {
    Drive drive(30.f);
    std::vector<float> heartbeat;
    drive.engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        heartbeat.push_back(drive.body->transform().m[12]);
    });
    drive.engine.run_frame(0.009);
    drive.engine.run_frame(0.009);
    REQUIRE(heartbeat.size() == 2);
    // Each Heartbeat reads where physics left the body, a whole number of substeps.
    const float substep = 30.f / 240.f;
    const float steps = (heartbeat[1] - heartbeat[0]) / substep;
    REQUIRE(std::fabs(steps - std::round(steps)) < 1e-3f);
    REQUIRE(std::fabs(drive.body->transform().m[12] - heartbeat[1]) < 1e-5f);
}

TEST_CASE("PB7 a RenderStepped write to a body wins over the swap-out", "[pose_blend]") {
    Drive drive(30.f);
    drive.engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) {
        drive.body->store_pose(at(100, 10, 0));
    });
    drive.engine.run_frame(0.009);
    REQUIRE(std::fabs(drive.body->transform().m[12] - 100.f) < 1e-5f);
}

TEST_CASE("PB8 a frame with no substep keeps blending forward", "[pose_blend]") {
    Drive drive(30.f);
    drive.engine.run_frame(0.0042);  // one substep, accumulator ~0
    drive.drawn.clear();
    drive.engine.run_frame(0.001);  // no substep
    drive.engine.run_frame(0.001);  // no substep
    REQUIRE(drive.drawn.size() == 2);
    REQUIRE(std::fabs((drive.drawn[1] - drive.drawn[0]) - 30.f * 0.001f) < 1e-3f);
}

TEST_CASE("PB9 Stop clears the blend and a destroyed body is skipped", "[pose_blend]") {
    Drive drive(30.f);
    drive.engine.run_frame(0.009);
    REQUIRE_FALSE(drive.engine.datamodel().pose_blend().entries().empty());
    drive.engine.datamodel().destroy(drive.body->id());
    drive.body = nullptr;
    drive.engine.scheduler().unbind_all(engine_core::Phase::RenderStepped);  // the probe reads body
    drive.engine.run_frame(0.009);  // must not touch the dead id
    drive.engine.datamodel().stop_simulation();
    REQUIRE(drive.engine.datamodel().pose_blend().entries().empty());
}

TEST_CASE("PB10 a body turned by Heartbeat after the substeps still blends", "[pose_blend]") {
    Drive drive(30.f);
    float yaw = 0.f;
    drive.engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        yaw += 0.05f;
        const engine_core::Matrix4 now = drive.body->transform();
        REQUIRE_FALSE(drive.body->set_transform(engine_core::matrix4_multiply(
            at(now.m[12], now.m[13], now.m[14]), engine_core::matrix4_axis_angle({0, 1, 0}, yaw))));
    });
    const double walls[] = {0.007, 0.009, 0.008, 0.0075};
    for (int warm = 0; warm < 4; ++warm) {
        drive.engine.run_frame(0.008);
    }
    drive.drawn.clear();
    for (double wall : walls) {
        drive.engine.run_frame(wall);
    }
    for (std::size_t i = 1; i < drive.drawn.size(); ++i) {
        REQUIRE(std::fabs((drive.drawn[i] - drive.drawn[i - 1]) - 30.f * static_cast<float>(walls[i])) < 2e-3f);
    }
}
```

> Before writing the fixture, check these helper names against the code, and use the real ones if they differ:
> - `workspace_service(game)` returning the Workspace with `set_gravity` (see `sandbox/joint_tests.cpp:72`);
> - `DataModel::start_simulation` / `stop_simulation` (`DataModelPlace.cpp`);
> - `TaskScheduler::unbind_all` (if absent, keep the job's id from `bind` and `unbind` it).
>
> The `Drive` fixture must reproduce the live bug before any change. In Step 2, PB5 has to fail on spacing, not on setup.

- [ ] **Step 2: Run them to verify they fail**

Run `cmake --build build --config Debug --target sandbox && build/Debug/sandbox.exe "[pose_blend]"`.
Expected: compile errors for `store_pose` and `pose_blend()`. Once those two exist as stubs (empty `store_pose`, a `PoseBlend` member), PB5 and PB10 fail with spacings of 0.125, 0.25, or 0.375 units (1, 2, or 3 substeps × 30/240) instead of 30 × wall.

- [ ] **Step 3: Implement the capture**

- `DataModelState.hpp`: add `#include "PoseBlend.hpp"` and the member `PoseBlend pose_blend;`.
- `DataModel.hpp`: add `PoseBlend& pose_blend();`. `DataModel.cpp`: `PoseBlend& DataModel::pose_blend() { return state_->pose_blend; }`.

`PhysicsBase.cpp:218`:

```cpp
void PhysicsBase::store_simulated(const Matrix4& transform, Vec3 velocity) {
    // While the frame's last substep is captured, the window draws this
    // body between where it was and where physics put it.
    pose_blend().record(id(), PoseBlend::Kind::Body, transform_, transform);
    transform_ = transform;
    velocity_ = velocity;
}

void PhysicsBase::store_pose(const Matrix4& transform) { transform_ = transform; }
```

Declare `store_pose` in `PhysicsBase.hpp` beside `store_simulated`, with the comment "The pose alone, for the render window's blend: no velocity, no dirty bit, no Changed."

`DataModel.cpp` `write_simulated_transform`, before `write_component`:

```cpp
    state_->pose_blend.record(id, PoseBlend::Kind::Spatial, part->body->transform(), transform);
```

Add next to it:

```cpp
void DataModel::store_visual_transform(InstanceId id, const Matrix4& transform) {
    const Slot* part = slot(id);
    if (part == nullptr || part->body == nullptr) {
        return;
    }
    write_component(ecs_world(), part->entity, state_->ecs_ids.transform, transform);
    note(id, VisualField::Transform, WriteOrigin::Simulation);
}

bool DataModel::spatial_transform(InstanceId id, Matrix4& out) const {
    const Slot* part = slot(id);
    if (part == nullptr || part->body == nullptr) {
        return false;
    }
    out = part->body->transform();
    return true;
}
```

`PhysicsWorld.cpp` `Impl::simulate` (1853–1883): before `b3World_Step`, add

```cpp
        PoseBlend& blend = game.pose_blend();
        const bool capture = blend.armed();
        if (capture) {
            blend.begin_capture();
        }
```

and after `game.assemblies().place_held(game);` add `if (capture) { blend.end_capture(); }`. The `sync` in `step` runs before `simulate`, so script teleports never enter the capture.

`Engine.cpp` `run_frame`'s substep loop:

```cpp
                const int planned = std::min(static_cast<int>(accumulator_ / physics_dt_), kMaxSubsteps);
                while (accumulator_ >= physics_dt_ && substeps < kMaxSubsteps) {
                    // The last substep's motion is what the window draws through.
                    if (substeps == planned - 1) {
                        game_.pose_blend().arm();
                    }
                    ... existing substep body ...
                    game_.pose_blend().disarm();
                    accumulator_ -= physics_dt_;
                    ++substeps;
                }
```

`DataModelPlace.cpp`: call `state_->pose_blend.clear();` beside `simulation_running = true` (line 42) and beside `simulation_running = false` (line 84).

- [ ] **Step 4: Implement the swap**

Add to `PoseBlend.hpp`: `class DataModel;`; a private `struct Swap { InstanceId id; Kind kind; Matrix4 truth; Matrix4 shown; }; std::vector<Swap> swaps_;`; the friend declarations; and below the class:

```cpp
// At the window's start: every entry's instance shows its blended pose,
// written quietly so nothing hears it. Its true pose is kept.
void swap_in_blend(DataModel& game, double alpha);
// After the copy: each instance still showing its blend gets its true pose
// back. One a window script wrote keeps the script's value.
void swap_out_blend(DataModel& game);
```

`PoseBlend.cpp` (include `DataModel.hpp` and `PhysicsBase.hpp`):

```cpp
void swap_in_blend(DataModel& game, double alpha) {
    PoseBlend& blend = game.pose_blend();
    blend.swaps_.clear();
    for (const PoseBlend::Entry& entry : blend.entries_) {
        Matrix4 truth;
        if (entry.kind == PoseBlend::Kind::Body) {
            auto* body = dynamic_cast<PhysicsBase*>(game.instance(entry.id));
            if (body == nullptr) {
                continue;
            }
            truth = body->transform();
            const Matrix4 shown = PoseBlend::blended(entry, truth, alpha);
            body->store_pose(shown);
            blend.swaps_.push_back({entry.id, entry.kind, truth, shown});
        } else if (game.spatial_transform(entry.id, truth)) {
            const Matrix4 shown = PoseBlend::blended(entry, truth, alpha);
            game.store_visual_transform(entry.id, shown);
            blend.swaps_.push_back({entry.id, entry.kind, truth, shown});
        }
    }
}

void swap_out_blend(DataModel& game) {
    PoseBlend& blend = game.pose_blend();
    for (const PoseBlend::Swap& swap : blend.swaps_) {
        Matrix4 now;
        if (swap.kind == PoseBlend::Kind::Body) {
            auto* body = dynamic_cast<PhysicsBase*>(game.instance(swap.id));
            if (body != nullptr && same_matrix4(body->transform(), swap.shown)) {
                body->store_pose(swap.truth);
            }
        } else if (game.spatial_transform(swap.id, now) && same_matrix4(now, swap.shown)) {
            game.store_visual_transform(swap.id, swap.truth);
        }
    }
    blend.swaps_.clear();
}
```

`Engine::run_window(double dt)`: after `pump_.begin_prerender_window(game_);`, call `swap_in_blend(game_, std::clamp(accumulator_ / physics_dt_, 0.0, 1.0));`. After the guarded `take_changes`, call `swap_out_blend(game_);`. Both run under the frame's lock. Put `swap_out_blend` in its own `guarded_step`, so a contract failure in `take_changes` can't leave blended poses in the sim.

- [ ] **Step 5: Run the tests**

Run `build/Debug/sandbox.exe "[pose_blend]"`, then the full `build/Debug/sandbox.exe`.
Expected: PB1–PB10 pass, and so does everything else. If the T5/T13/RW6 snapshot-origin tests now see `Simulation` where they expected `PreRenderDataModel` on a body-driven row, it's because `store_visual_transform` notes `Simulation`. That is correct for a sim-driven pose, but confirm the failing test's instance is actually body-driven before changing any expectation.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core src/engine_instances/PhysicsBase.hpp src/engine_instances/PhysicsBase.cpp sandbox/pose_blend_tests.cpp
git commit -m "Physics bodies draw blended through their last substep"
```

---

### Task 4: Docs, Release build, live proof

**Files:**
- Modify: `src/engine_core/README.md` (lines 3, 13, 15, 17–23, 35–43), `src/engine_core/Engine.hpp`, `TaskScheduler.hpp:18-22, 37-50`, `types.hpp`, `DataModelLock.hpp:12-13`, `Events.hpp:48, 89, 110, 169`, `ScriptRuntime.hpp:37-38, 489`, `src/runner/Runner.hpp:14-18, 34, 46`, `src/runner/Renderer.hpp:442, 459`, `src/engine_core/IRenderer.hpp:8`, `src/engine_core/IClock.hpp:5-6`, `src/engine_core/DataModel.hpp:120-125`, `src/engine_core/DataModelState.hpp:143-164`
- Modify: `docs/superpowers/specs/2026-10-03-renderstepped-window-design.md` (a superseded note under the title)

- [ ] **Step 1: Docs**

In `src/engine_core/README.md`:
- Line 3: "One engine thread steps the simulation and then prepares that frame's snapshot; the UI thread draws it."
- Delete path D from the A–E table.
- Path B's row becomes "the engine thread inside the RenderStepped window".
- Add a paragraph "Interpolation": bodies moved by a frame's last substep are drawn through it, at `accumulator / physics_dt`; only the window sees it (`PoseBlend.hpp`).

In each header listed above, replace "RenderThread" or "SimulationThread" with "the engine thread", or with "the window" where the comment is about the pre-draw window, and make each sentence true of one thread. `Renderer.hpp:442,459` and `IRenderer.hpp:8` say the UI thread draws. Under the 2026-10-03 spec's title, add: "Superseded in part by 2026-10-12-single-engine-loop-design.md: there is one engine thread, and the window is a phase of it."

Then grep `src/` for `RenderThread|render thread|SimulationThread|simulation thread` and fix each comment that is now false. Leave comments that say "SimulationThread only" where the code is still engine-thread-only, reworded to "Engine thread only".

- [ ] **Step 2: Release build and full suite**

Run `cmake --build build --config Release`, then `build/Release/sandbox.exe`.
Expected: everything passes, apart from the known `[explorer]` timing flakes (rerun alone).

- [ ] **Step 3: Live proof on H1Z1**

1. Launch `build/Release/AnarchyStudio.exe "C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1"` with `ANARCHY_MCP_PORT`, `ANARCHY_MCP_TOKEN`, and a scratch `APPDATA` (the drive-studio-over-mcp memory).
2. `playtest {"action":"start"}`, then spin the car's wheel motors with `run_lua`: set `SpinMotorSpeed = 3500` and `MaxSpinTorque = 600` on every WheelConstraint under `workspace.Car`.
3. Create a temporary play Script that logs `dt` and the chassis' horizontal displacement for 120 RenderStepped frames and prints them.

Expected: displacement / dt stays within a few percent of the car's speed every frame. Before this change, the same probe gave 0.50–1.69 units per frame at a steady ~32 units/s. Paste the numbers into the hand-back. Stop the studio without saving.

- [ ] **Step 4: Commit**

```bash
git add src docs
git commit -m "Docs: one engine thread, and bodies drawn through their last substep"
```

---

## Self-Review Notes

- Spec coverage:
  - single loop, lock, pacing, paused window, thread rules, `ThreadRole::Render` and PostRender removal, profiler row: Task 1;
  - interpolation capture, swap, script write wins, no-substep frames, Stop: Tasks 2 and 3;
  - docs and live proof: Task 4.
- While planning, "blend only while the true pose equals `after`" became "rewind whatever the current pose is". Otherwise a turning player would drop out of the blend every frame (Review Focus 3). The spec has been updated to match.
