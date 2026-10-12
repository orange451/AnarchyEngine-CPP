# One Engine Loop, with Physics Interpolation

2026-10-12 · Replaces the engine's two free-running loops (SimulationThread, RenderThread) with one loop that steps the simulation and then prepares that frame's snapshot, and draws simulated bodies between their last two physics substeps.

## Goal

A moving physics body draws at evenly spaced positions on every display, and a camera that follows it from `RunService.RenderStepped` stays still relative to it. The H1Z1 car's lerp camera feels as smooth as a camera snapped to the car.

## Why it stutters today

Measured live in H1Z1 with the car driving straight at about 32 units/s:

- **Substep rounding.** Physics steps in fixed 1/240 s substeps from an accumulator, and the leftover time is never shown. At about 120 Hz, Heartbeat's dt held at 7–9 ms while the chassis moved 1, 2, or 3 substeps' worth (0.135, 0.27, 0.40 units). 144 and 165 Hz displays judder in a steady 2, 2, 1 pattern.
- **Two loops drifting.** Prepare takes the lock whenever it can, so each drawn frame catches 0, 1, or 2 sim steps. RenderStepped's dt held at 33 ms (the window was in the background) while the chassis moved 0.50 to 1.69 units per frame.

RenderStepped's dt is correct. What it reads is not evenly spaced.

## Decisions

| Question | Decision |
| --- | --- |
| Threads | One engine thread runs each frame in this order: sim step, then the render window (RenderStepped, PreRender), then the snapshot copy and `SceneFeed::perform`. The UI thread still draws with GL from `SceneFeed`, and the terrain mesher's workers are unchanged. The engine never drew; its "RenderThread" only prepared the snapshot. |
| Frame order | Sim step, then RenderStepped. A camera handler then reads this frame's physics. Roblox runs RenderStepped first and simulates after the draw. That is not needed here: our sim step reads its input at its own start, and the window comes after it in the same frame. |
| Pacing | Unchanged. The loop waits for the window's paint (`note_client_frame`) and falls back to 16.7 ms when nothing paints. A paced test (`set_pace_hz`) waits for its slot. One wait per frame replaces the two. |
| Lock | One `DataModelLock::Write` covers the whole frame from the sim step through the copy, so nothing can slip a write between the step and its snapshot. The 2 ms try-lock and `RenderStepTime`'s carried time go away: a frame no longer skips its window. |
| dt | RenderStepped gets the frame's dt, the same one Heartbeat got. |
| Paused or stopped | Each paint (or 16.7 ms fallback) runs the window and copy, so the editor camera keeps the display rate. The stopped-physics and tool tick still runs only when `kToolInterval` has passed, as now. |
| Thread rules | The window becomes a phase of the one thread, not a thread. `mutation_thread`, `authorize`, `current_origin`, `destroy`, `queues_visual_write`, `TaskScheduler::run_phase`, `Events::invoke_render`/`drain`/`post`, `assert_lua_thread`, and `override_visual` decide by "is the window open" instead of by thread id or `ThreadRole::Render`. Paths A (sim step), B (window), C (override), and E (other threads queue) keep their meanings. Path D (render thread outside the window) no longer exists. |
| `ThreadRole::Render` | Removed. The engine thread is `Simulation`. |
| PostRender | Removed. No production code binds it, and no script can reach it. |
| PreRender | Kept as the phase after RenderStepped in the window; declared to scripts and blocked, as now. |
| SnapshotPump double buffer | Kept. It is harmless, and `SceneFeed` remains the real handoff to the UI thread. |
| Console VM's summed RenderStepped | Kept as is (`note_frame`, `fire_render_stepped`). The command line still enters it without the lock. |
| Profiler | The "Sim" and "Render" rows merge into one "Engine" row. `frame_boundary` marks each engine frame. |
| Interpolation | Every body moved by the frame's last substep draws between its pose before and after that substep, blended by `alpha = accumulator / physics_dt`. One substep (at most 4.2 ms) of display latency. |
| What sees the blend | Everything inside the window: Lua reads of `Transform` in RenderStepped, snapshot rows, Brush draws, Attachments and Bones (derived from their parent), billboards, wireframes. The sim step, Heartbeat, and physics see true poses. |
| How | Swap in, swap out. At the window's start each blended pose is written with quiet setters (no Changed, no history, no watchers; snapshot rows noted). After the copy, the true pose goes back. |
| A script writes the body in the window | Its write wins, carried from the blended frame it was read in to the sim's: `truth * inverse(shown) * write`. A write built from the blended read (`Transform = Transform * spin`) then keeps the body's true motion; an absolute write lands within one substep's motion of where it asked. |
| Writes after the substep | The blend rewinds whatever the current pose is by the part of the last substep's motion not yet due: `lerp(before, after, alpha) * inverse(after) * current`. A character a script turns after physics, or a tool put on its hand, keeps blending. A teleport shows the new spot less under one substep's motion, which can't be seen. Play start and Stop clear the entries. |
| Frames with no substep | At high refresh the accumulator can stay under one substep. The entries carry over and alpha keeps growing, so motion stays even. |
| Out of scope | `DataModel::integrate_simulated` (the older velocity integrator for `Simulated` SpatialObjects) is not interpolated. Network smoothing does not exist yet. |

## Architecture

### 1. The loop (`Engine`)

`Engine::start` spawns one thread, `engine_loop`. Each pass:

1. Wait for the paint or the pace slot (skipped on the first pass).
2. Measure `wall` (clamped to 0.1 s) and take the write lock.
3. **Playing:** run the existing step body: commands, PreAnimation, events, animation, terrain, substeps, Heartbeat, scripts, tools, mesh edits, held items, audio.
   **Paused:** run the stopped-physics and tool tick when `kToolInterval` has passed.
4. Run the window: `begin_prerender_window`, interpolation swap-in, RenderStepped, PreRender, `end_prerender_window`, `take_changes`, swap-out.
5. Release the lock, then `finish_copy`, `publish`, `renderer_->perform`, `present`.

`pause`, `resume`, `on_simulation`, and `stop` keep their contracts. `on_simulation` while paused still runs on the caller under the lock. `render_thread_id()` goes away, and `simulation_thread_id()` is the engine thread. `set_render_pace_hz`, `set_simulation_pace_hz`, `set_render_client_sync`, and `set_simulation_client_sync` fold into `set_pace_hz` and `set_client_sync`. Only `Runner` and tests call them.

### 2. Phase rules (`DataModel`, `TaskScheduler`, `Events`, `ScriptRuntime`, `SnapshotPump`)

`DataModel::set_thread_ids(sim, render)` becomes `set_engine_thread(id)`. Every check that asked "is this RenderThread" asks `prerender_window()` instead. Inside the window, a write from C++ needs visual-only or `ForceSimWrite`, a write from Lua (`window_script`) is unrestricted, and the origin is `PreRenderDataModel`, exactly as now. Outside it, the engine thread is the sim. `Events::post` already queues while the window is open, so window writes still queue their handlers. `drain` and `Signal::wait` refuse inside the window, as they refuse on RenderThread today.

### 3. Interpolation (`PhysicsWorld`, `Assemblies`, new `PoseBlend`)

The new `engine_core/PoseBlend.{hpp,cpp}` holds `entries: vector<{InstanceId id, Kind kind, Matrix4 before, Matrix4 after}>`, where `Kind` is `Body` (a `PhysicsBase`) or `Spatial` (a body-driven SpatialObject or welded member).

- **Recording.** The substep loop knows its count up front (`n = min(floor(accumulator / physics_dt), 32)`). Before the last substep it calls `blend.begin()`, which clears the entries. During that substep, `PhysicsWorld::pull` and `Assemblies::write_member` call `blend.record(id, kind, old, new)` wherever they call `store_simulated` or `write_simulated_transform`. A frame with no substep records nothing and keeps the previous entries.
- **Swap in** (start of the window). `alpha = clamp(accumulator / physics_dt, 0, 1)`. For each entry whose instance still exists, write `lerp(before, after, alpha) * inverse(after) * current` with a quiet `PhysicsBase::store_pose` (Body) or a new quiet `DataModel::store_visual_transform` that sets the ECS transform and notes the row without watchers (Spatial). Remember the true pose and what was written.
- **Swap out** (after `take_changes`). For each instance swapped in, if it still holds the blend, write its true pose back the same quiet way. Otherwise keep the script's write, carried into the sim's time as above.
- **Capture window.** `PhysicsWorld::simulate` records only around the armed substep's Box3D step, `pull`, and held placement. The `sync` before it, where script teleports reach bodies, is not recorded.
- **Clear.** Play start and Stop clear the entries. Ids are generation-checked, so a destroyed body is skipped.

The camera handler, `pull`'s `follow_driven` check, and `Assemblies` see only true poses outside the window, so nothing teleports.

### 4. Tests (Catch2, `sandbox`)

- **Rewritten:** T1 (one engine thread), T2 (a write during `perform` is rejected because the window is closed), T22 (the window runs while paused), and the pace tests (one loop, one wait).
- **Removed:** T3 and T4 (sim and present overlapping), T21's and T23's PostRender cases, and the Render-role checks. `ScriptRig::render()` runs the window on the calling thread.
- **New, `pose_blend_tests.cpp`:**
  - A body moving at constant speed, stepped with uneven `wall` times (7, 9, 8, 7.5 ms), reads evenly spaced positions in RenderStepped (spacing = speed × wall, within 1e-4).
  - Heartbeat reads true poses.
  - A RenderStepped write to a body's Transform survives the swap-out.
  - A body a Heartbeat script turns after the substeps still blends.
  - Stop clears the blend, and a destroyed body is skipped.
  - A frame with no substep keeps blending.
- **Live proof:** rerun this investigation's probes on H1Z1. RenderStepped's per-frame chassis displacement should track speed × dt within a few percent, where today it spans 0.5 to 1.69 units at a steady speed. Then turn the car camera's lerp back on and drive.

## Docs

Update `src/engine_core/README.md` (one thread; the path table without D), `Engine.hpp`, `TaskScheduler.hpp`, `types.hpp`, `Runner.hpp`, and the stale "RenderThread, GL context current" comments in `Renderer.hpp` and `IRenderer.hpp`. Add a note to the 2026-10-03 RenderStepped window spec that this design supersedes its threading section. The ~360 comment mentions of RenderThread are fixed where the code beside them changes; the rest are swept once at the end.
