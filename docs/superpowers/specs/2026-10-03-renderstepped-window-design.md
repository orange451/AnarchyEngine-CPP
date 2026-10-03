# RenderStepped in the Render Window Design

2026-10-03 · Replaces the sim-thread delivery of `39d28ff` (Let scripts connect to RunService.RenderStepped) with Roblox's semantics: handlers run before the frame is drawn, and what they read is what that frame draws.

## Goal

`RunService.RenderStepped:Connect(fn)` and `:Wait()` run scripts in the render thread's Prepare window, once per displayed frame, with that frame's dt. A camera handler that reads a ball and writes the camera produces a frame showing both from the same world state, which `39d28ff` does not: it fires the signal at the start of the next simulation step, before physics, so the camera trails every moving target by one step (the CannonVsPigs desync). Plugins get the same delivery in edit mode, so an editor camera moves at display rate.

The scheduling design is unchanged: the engine's RenderStepped phase stays on RenderThread inside the Prepare lock, simulation phases stay on SimulationThread, and the snapshot paths A–E keep their meanings. What changes is that path B can now run Lua.

## Why this is thread-safe

The two threads already exclude each other: the simulation loop holds the DataModel write lock for its whole step, and Prepare holds it for RenderStepped, PreRender, and the copy. Lua today runs only inside the sim step, so during Prepare every VM is quiescent. The rule "the VM is entered only on SimulationThread" becomes "the VM is entered only while holding the DataModel write lock"; a `lua_State` does not care which OS thread calls it as long as the calls are serialized, and the lock serializes them. `ScriptRuntime::assert_lua_thread` is relaxed accordingly.

The console VM is the exception: the command line can enter it from a thread that does not hold the write lock (`ScriptRuntime.hpp`, "Safe from the simulation thread and from the thread that runs the command line"). It stays out of the window.

## Decisions

| Question | Decision |
| --- | --- |
| Delivery | Direct dispatch. The EventQueue cannot drain in the window (`Events.cpp` contract), so ScriptRuntime keeps per-VM lists of render connections and parked waiters, and its render-phase job invokes them. The queue is not involved. |
| Which VMs | Play and plugin connections run in the window. A console-VM connection falls back to the sim-side delivery of `39d28ff`: fired at the start of the next step with the summed frame time. |
| dt | The displayed frame's delta, as the render loop computes it. The console fallback keeps the summed-frames dt. |
| Yields | Each invocation is its own coroutine. `task.wait` and other yields park it; the sim side's existing wake machinery resumes it as an ordinary script continuation. Roblox semantics: after a yield you are no longer in the render step. |
| `Wait()` | Parks the thread on the window list and resumes it in the window with the frame dt, so `while true do RenderStepped:Wait() end` cameras work. |
| Writes | Path B while in the window: visual writes land in this frame's snapshot, by the existing enforcement. A write the window refuses raises a catchable Lua error instead of `contract_fail`. After a yield, writes are ordinary sim writes (path A). |
| Errors | Lenient, as Roblox: a handler error goes to the console through the existing guarded path and the connection stays, every frame if it keeps erroring. No watchdog, no auto-disconnect; a slow handler costs frame rate and nothing else. |
| Infinite loops | Window execution runs under the same Luau interrupt budget as sim scripts, so a hung handler errors instead of deadlocking render against sim. If sim scripts have no such budget, the window gets none either. |
| Pause | Play handlers do not run while the session is paused, matching "signals wait for the session to resume". Plugin handlers run through pause and in edit mode, every displayed frame. |
| Lifecycle | Play connections end at Stop, carrying script and generation like scripted connections. Plugin connections live until the plugin unloads, like `connect_kept`. `Connection:Disconnect()` works from either thread's Lua. |
| PreRender | Unchanged: declared to scripts, blocked. In Roblox, PreRender is RenderStepped's new name for the same event; one open name is enough. |
| Signals fired by a handler | `note_property_change` and friends queue as today and drain on the next sim step. Only delivery of the two pre-render phases bypasses the queue; everything a handler causes stays queued. |

## Architecture

### 1. Render connection lists (`ScriptRuntime`)

ScriptRuntime owns, per window VM (play, plugin), a list of render connections `{HeldRef callback, InstanceId script, std::uint32_t generation, Connection handle}` and a list of parked render waiters (thread serials). `signal_connect` and `signal_wait` on the RenderStepped signal from a play or plugin thread append to these instead of connecting through the EventQueue; from the console VM they take the existing queued path. Mutation happens only under the DataModel write lock, from whichever thread holds it, so no further locking is added. Disconnect marks the entry; the walker skips and frees marked entries outside the walk.

### 2. The window job (`ScriptRuntime::render_step`)

The render-phase job bound in `attach`, which today only calls `RunService::note_frame`, becomes: keep `note_frame` (the console fallback still needs the accumulator), then, for the plugin VM always and the play VM when open and not paused, resume parked waiters with the frame dt and invoke each connection as a fresh coroutine with the frame dt. A coroutine that completes is released as usual; one that yields is handed to the VM's normal thread bookkeeping and wakes on the sim side. Errors report through `guarded` to the console. Paused is read from a flag the engine sets, since the runtime cannot ask the engine.

### 3. Script-context write refusals (`DataModel`)

While Lua executes, DataModel write refusals must raise, not abort. A thread-local "script context" depth, set around every Lua entry (sim and window), makes `reject_write` and the window's authorize failures throw the existing script-error type instead of `contract_fail`. Sim-side script writes that are refused today already surface as Lua errors through the binding layer's checks; this extends the same promise to the paths the window can newly reach.

### 4. What `39d28ff` keeps and loses

`RunService::note_frame`, the accumulator, and `fire_render_stepped` stay for the console fallback; the sim-side fire in `fire_phase`/`step_tools` fires only the console VM's connections. Per-phase dt bookkeeping stays. The S9/S18/A11 test changes stay. S51 and S52 are rewritten around window delivery.

## Testing

Sandbox tests drive the window with the `rig.render()` helper (a real thread in the render role) and assert:

1. A handler runs once per rendered frame with the frame's dt, and not on sim steps without a frame.
2. A camera write from a handler lands in that same frame's snapshot (via SnapshotPump, as T21 does), aimed at the position the handler read.
3. `task.wait` inside a handler parks it; the continuation runs on the following sim step, and its writes are sim writes.
4. The `while true do RenderStepped:Wait() end` loop runs once per frame.
5. A handler that errors reports to the console every frame and stays connected; the engine lives.
6. A write the window refuses raises a Lua error the handler can pcall; the engine lives.
7. Play handlers are silent while paused and gone after Stop; plugin handlers run through both, and in edit mode.
8. A console-VM connection falls back to sim-side delivery with summed dt.
9. The full sandbox and engine-tests suites pass, and the new tests pass under ThreadSanitizer.

CannonVsPigs and SceneCamera are the live proof: camera locked to the ball in play, display-rate camera in edit, with no change to either script.

## Out of scope

Snapshot interpolation for sim-rate motion on faster displays; a named `BindToRenderStep` API (Connect order is the only ordering); running the console VM in the window; unblocking PreRender.
