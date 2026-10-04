# Microprofiler Design

2026-10-03 · A frame profiler drawn over the Scene View, in the studio and in AnarchyPlayer, in the spirit of [jonasmr/microprofile](https://github.com/jonasmr/microprofile) but our own code. It measures how long each engine phase, each Script, each `debug.profilebegin` section, and each GPU pass takes, per frame.

## Goal

A developer playing their place presses Cmd+F6 and sees, over the scene, one bar per frame and a timeline of what every thread did in it. When a frame spikes, they pause (Cmd+P), click it, and see which phase, which Script, and which of that Script's own sections made it slow. A table of averages and maxima says what is expensive in general. The same numbers reach AI agents through MCP and can be saved to a file and opened again.

## Decisions

| Question | Decision |
| --- | --- |
| What is measured | Engine phases (C++ scopes), each Script's resumes (one scope per Script, with what resumed it), user scopes from `debug.profilebegin(name)` / `debug.profileend()`, and GPU passes. |
| Views | Frame graph on top. Below it, two tabs: **Timeline** (a flame chart per thread) and **Scopes** (a sortable table). Mockup: option B of the view study. |
| GPU | Yes. A GPU row from `GL_TIMESTAMP` queries around the Renderer's main passes, shown 2–3 frames late and labelled so. Engine passes only; scripts cannot mark GPU work. |
| Where it draws | `GameView`, so the studio and the player share it. In the studio, only on the Scene View last focused (the one whose Camera is CurrentCamera). |
| Hotkeys | Cmd+F6 shows and hides. Cmd+P pauses and resumes. Studio-wide, whatever has focus. Ctrl on Windows and Linux. No other hotkeys. |
| Menus (studio) | View → Profiler (Cmd+F6), View → Pause Profiler (Cmd+P), File → Open Profile Capture… |
| Pointer | While the overlay is shown, a pointer locked by `MouseBehavior` is freed. `MouseBehavior` keeps its value; hiding the overlay locks again on the next click in the view, as after Shift+Esc. |
| Recording | Off until the overlay is first shown, or `get_profile` asks. Hiding the overlay stops recording; its history stays until the next show. |
| Frame | One render-thread frame, Prepare to the next Prepare. Other threads' events fall into frames by timestamp. |
| History | The last 300 frames. |
| Pause | Freezes what the overlay shows; the game runs on. Resume returns to live. Selecting a frame in the graph also pauses. |
| Outside the overlay | MCP tool `get_profile`, and capture files (`*.aprof.json`) saved from the overlay and opened in the studio. |
| Implementation | Our own library, `src/profiler/`. Not vendored microprofile (its UI needs a backend we would rewrite, it brings a web server, and it risks the Xcode 13 / libc++ 13 toolchain). Not Tracy (its viewer is a separate app). |

## Architecture

### `src/profiler/` — the recorder

A library with no dependency on the engine, JadeFX, or GL.

- **`Profiler`** is process-wide. `enabled()` is one relaxed atomic load; a scope while disabled costs that and nothing else. Enabled, a scope reads `steady_clock` twice and writes two events into its thread's ring.
- **Scope ids.** `ScopeId` is interned once per name and group: `PROFILE_SCOPE("Physics", Group::Physics)` caches its id in a function-local static. Groups: `Engine`, `Physics`, `Render`, `Script`, `User`, `Gpu`. Script scopes are interned by the Script's GUID plus a name (see Scripts). Ids live for the process, so history survives Play, Stop, New, and Open.
- **Threads.** Each thread that records calls `Profiler::register_thread("Sim")` once and gets a single-writer ring of 64k events (about 1.5 MB). Recording never locks. Named rows: Sim, Render, UI, and GPU (filled by `GpuTimer`, not a real thread). Any other thread that registers gets its own row after these.
- **Event.** Begin or end, scope id, timestamp in nanoseconds from `steady_clock`, and an optional cause id (for Script scopes). Every thread uses the same clock, so rows line up.
- **Frames.** The render thread calls `Profiler::frame_boundary()` at the start of each Prepare. A frame is the span between two boundaries.
- **Collector.** Once per UI paint, while recording, it drains every ring into a `FrameHistory`: the last 300 frames, each with its events per thread as nested scopes (depth, start, duration, cause). Events from threads that run at their own rate (Sim, UI) go into the frame whose span holds their begin time; a scope that crosses a boundary is drawn where it starts, at its full length.
- **Stats.** From the history: for each scope (and each Script-plus-cause pair), its max, average, and calls per frame over the history, its time in the selected frame, and its share of the average frame.
- **Pause.** `FrameHistory` is copied to a frozen copy the overlay reads; the collector keeps draining (so rings never fill) but into the live history only.
- **Capture I/O.** `write_capture` and `read_capture` (see Capture file). Read checks everything before it returns a history; on any failure it returns why and nothing else.

### Timing points

| Where | Scopes |
| --- | --- |
| `Engine::simulation_loop` | Simulation step; each scheduler phase; each events drain; Input dispatch (around `UserInputService::dispatch` and the Dragger filter); Physics, with each Box3D substep; Audio. |
| `Engine::render_loop` | Prepare (with RenderStepped, PreRender, Snapshot copy, path-C overrides, and the lock wait when Prepare waits for the DataModel lock), Perform, Present, PostRender. |
| `ScriptRuntime::resume_one` | One Script scope per resume (see Scripts). |
| `ScriptRuntime` `require` | A nested scope named after the ModuleScript, inside the Script that called it. |
| `Renderer` | CPU and GPU scopes: Shadows, Meshes, Skybox, Outlines and handles, Grid. |
| `GameView` / UI thread | Input (event polling and JadeFX event handling), Paint, Scene View, GuiLayer, Profiler overlay (so its own cost is visible). |

### Input across threads

The UI thread receives OS input and posts it to `UserInputService`'s queue (a short mutex-held push); that cost is in the UI row's Input scope. The Sim thread's `dispatch` applies it and queues InputBegan, InputChanged, and InputEnded, inside Input dispatch. Lua handlers run on the Sim thread as listener coroutines through `start_listener` → `resume_one`, so they appear as Script scopes with cause InputBegan (or whichever). Lua never runs on the UI thread.

### Scripts

- **Script scope.** `resume_one` opens a scope named after the Script (its GUID keys the id, so a renamed Script keeps its history under the new name) and records a **cause**: the signal's name (`InputBegan`, `Heartbeat`, `RenderStepped`, …), `wait` for a `task.wait` or `wait` wake, `spawn`, `defer`, or `start` for the Script's first run. `start_listener` must know its signal's name; if it does not today, the plan plumbs it through. The tooltip shows the cause; the Scopes table lists each Script-plus-cause pair as its own row, such as "PlayerControls · InputBegan".
- **`debug` table.** `LuauSandbox` removes `debug` today. The play, console, and plugin VMs get back a `debug` table holding only `profilebegin(name)` and `profileend()`, as in Roblox. Everything else in `debug` stays nil.
- **`profilebegin(name)`** opens a User scope inside the current Script scope. `name` must be a string (otherwise a Lua error, as any bad argument); it is cut to 64 bytes. A Script may create at most 256 distinct names; past that, its new names record as "(too many scopes)".
- **Unbalanced use.** A scope left open when the coroutine yields, errors, or finishes is closed at that point. `profileend()` with nothing open does nothing. Either prints a console warning once per Script per kind; Problems lists nothing.
- **Disabled.** Both functions return at once when not recording.

### `GpuTimer` (in `src/runner/`)

- A pool of `GL_TIMESTAMP` query objects, made once on the context's thread. Around each GPU scope, `glQueryCounter` at begin and end.
- Results are read 2–3 frames later with `GL_QUERY_RESULT_AVAILABLE`; a query not ready is checked again next frame, never waited on. When the pool runs out, that frame's GPU row is skipped.
- GPU timestamps are converted to the CPU timeline by one `glGetInteger64v(GL_TIMESTAMP)` against `steady_clock` per frame, and pushed into the GPU row of the frame they belong to. The overlay labels the row "GPU (frame −N)" with the lag it measured.
- If timer queries are unsupported, the GPU row reads "GPU timing unavailable" and nothing else changes.
- Every GL call stays on the context's thread. It is proven in `assets-demo` before the studio uses it, since a GL error ends the studio.

## The overlay

`ProfilerOverlay` is a JadeFX node in `GameView`, above the ScreenGuis and the camera list, left out of `requestCapture` readbacks.

- **Header:** "Profiler", the selected frame's number and ms, the **Timeline | Scopes** tabs, a pause/resume button, **Save** (only while paused), events dropped (when any), the capture's file name (when showing one), and the hotkey hints.
- **Frame graph:** the last 300 frames as bars, scaled to the slowest of them and never below 33 ms. A dashed line at 16.6 ms; frames over it are red. Hover: frame number and ms. Click: selects the frame and pauses.
- **Lower half**, about 45% of the view at first, resized by dragging its top edge:
  - **Timeline:** rows Sim, Render, UI, GPU, each labelled; nested scopes stacked, coloured by group. Scroll zooms about the pointer; drag or right-drag pans; double-click a scope zooms to fit it. Live, it follows the newest frames; a selected frame is centred. Hover tooltip: name, ms, start within the frame, thread, and cause or detail. A row with no events in view is drawn empty, never hidden.
  - **Scopes:** columns Scope, Thread, Max, Avg, This frame, Calls/frame, % of frame; sorted by Max at first, any column by clicking its header. Over the live history, or the frozen one while paused. Clicking a row switches to Timeline with that scope's blocks highlighted.
- **Colours:** one per group, added to the theme editor under a "Profiler" group. User scopes do not choose their own.
- **Input:** the overlay takes the mouse only where it covers the view; elsewhere the scene, the camera, the Dragger, and the GUIs get it as before.
- **Pointer:** while the overlay is shown, `GameView::syncPointerLock` treats a lock as unwanted, so it frees the pointer and posts no mouse delta. `MouseBehavior` is untouched. On hide, the next click in the view locks again.
- **Preferences:** the selected tab and the lower half's height. The overlay always starts hidden.

### Studio and player

- **Studio:** the studio's key handling and the View menu items toggle the overlay on the last focused Scene View, whatever has focus. File → Open Profile Capture… shows a capture in that view's overlay, paused and read-only, with its file name in the header; Resume or closing the capture returns to live.
- **Player:** no menus. `GameView` handles Cmd+F6 and Cmd+P itself when it is the player's view. Save writes `profile-<yyyyMMdd-HHmmss>.aprof.json` next to the game's files. No Open.

## MCP tool `get_profile`

- **Arguments:** `seconds?` (default 2, at most 10), `top?` (Scopes rows, default 25), `include_timeline?` (default true), `path?` (also save a capture file there).
- **Recording:** if nothing records, the tool records for `seconds`, then stops and returns. If the overlay records or is paused, the tool reads that history (the frozen one when paused) without waiting.
- **Result (JSON):** frame count; average, p95, and max frame ms; frames over 16.6 ms; the top Scopes rows with the table's columns; with `include_timeline`, the slowest frame as a scope tree per thread, cut at depth 6 and at scopes under 0.05 ms; the GPU lag in frames.
- **Description** tells agents that edit-mode numbers are not play numbers and suggests `playtest` first.

## Capture file

```json
{ "format": "anarchy-profile", "version": 1,
  "place": "MyGame", "created": "2026-10-03T14:02:11Z",
  "threads": ["Sim", "Render", "UI", "GPU"], "gpu_lag_frames": 2,
  "scopes": [{"name": "Physics", "group": "physics"},
             {"name": "EnemyAI", "group": "script", "script": "<guid>"}],
  "causes": ["InputBegan", "Heartbeat", "wait"],
  "frames": [[0, 16612], [16612, 33190]],
  "events": [[0, 1, 1, 13100, 5500, null], [0, 2, 2, 3100, 9500, 0]] }
```

- Times are microseconds from the first frame's start. `frames` are `[start, end]`. `events` are `[thread, scope, depth, start, duration, cause]`, with `cause` an index into `causes` or null.
- Save writes the paused history. In the studio it opens a save dialog in the project folder.
- Open refuses, with a console message and no change, a file whose `format` is not `anarchy-profile`, whose `version` is unknown, or whose indices or times do not hold together.

## Errors and limits

| Case | Behaviour |
| --- | --- |
| A ring fills within a frame | The newest events are dropped and counted; the header shows "N events dropped". Recording goes on. |
| Unbalanced script scopes | Closed at yield, error, or finish; stray `profileend` ignored; a warning once per Script per kind. |
| Non-string scope name | Lua error. |
| Long name / too many names | Cut to 64 bytes / "(too many scopes)" past 256 per Script. |
| Timer queries unsupported | GPU row reads "GPU timing unavailable". |
| Query not ready / pool exhausted | Read next frame / that frame's GPU row skipped. |
| Engine paused (edit mode) | The Sim row shows only the tool step at its rate; that is expected. |
| Bad capture file | Refused before anything changes, with why in the console. |

## Testing

- **`profiler-tests`** (new; pure C++, a fake clock): nesting; frame bucketing across three fake threads; stats; ring overflow drops and counts, and the next frame records; pause freezes, resume returns to live; disabled scopes record nothing; capture round trip; a wrong format, unknown version, or broken file refused with the history unchanged. A micro-benchmark reports, without asserting, the cost of an enabled and a disabled scope. Run under `build-tsan` too.
- **Lua tests** (`sandbox` / Lua engine tests): user scopes nest under the Script scope, which records its cause (InputBegan, Heartbeat, wait); open scopes closed at yield, error, and finish, warning once; stray `profileend` warns once; non-string name errors; name cut; 256-name limit; `debug` holds only the two functions and the sandbox test still passes; `require` makes a nested ModuleScript scope.
- **`studio-tests`** and a GameView test: Cmd+F6 shows the overlay on the last focused view only, Cmd+P pauses, both while a script editor has focus; the menu items show their shortcuts; with `MouseBehavior = LockCenter` the view locks, showing the overlay frees it with `MouseBehavior` unchanged, hiding locks again on the next click; clicking a bar selects and pauses; tabs and sorting work; `requestCapture` leaves the overlay out.
- **`mcp-tests`:** `get_profile` records when idle and returns well-formed JSON; reads the paused history when there is one; `path` writes a capture that opens again.
- **GPU:** in `assets-demo` or a GL check program, `GpuTimer` around a few draws gives times after the lag, no GL errors, and the unavailable path reads as such.
- **By hand:** a sample place with a slow Script using `debug.profilebegin("pathfind")`, played in the studio (bundle-resources build) and in AnarchyPlayer: the spike shows in the graph, the timeline puts it under that Script with its cause, the Scopes table ranks it first; a saved capture opens again in the studio.

## Out of scope

Loading captures in the player; counters (memory, draw calls) as rows; per-scope colours chosen by scripts; network or remote viewing; GPU scopes from scripts; hotkeys beyond Cmd+F6 and Cmd+P.
