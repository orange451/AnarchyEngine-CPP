# Microprofiler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A microprofile-style frame profiler drawn over the Scene View in the studio and AnarchyPlayer: engine phases, per-Script time with its cause, `debug.profilebegin` scopes, GPU passes, a frame graph, a Timeline and a Scopes table, pause, capture files, and an MCP tool.

**Architecture:** A dependency-free recorder (`src/profiler/`, library `profiler`) with one single-writer ring per thread and a collector that builds a 300-frame history. engine_core and the renderer call into it through `PROFILE_SCOPE`. A Painter-drawn JadeFX node (`runner::ProfilerOverlay`) shows it inside `GameView`; capture and report JSON live in studio_core next to the MCP tools.

**Tech Stack:** C++17 (Xcode 13 / libc++ 13 must build it: no `<format>`, no `std::span`), CMake, JadeFX Painter, OpenGL 3.3 `GL_TIMESTAMP` queries, Luau, Catch2 (sandbox) and the repo's hand-rolled `expect` tests.

**Spec:** `docs/superpowers/specs/2026-10-03-microprofiler-design.md`

## Global Constraints

- Hotkeys: Cmd+F6 shows/hides, Cmd+P pauses/resumes (Ctrl on Windows/Linux); no other hotkeys.
- History: the last 300 frames. Frame = render-thread Prepare to the next Prepare.
- Ring: 65536 events per thread; overflow drops the newest and counts them.
- Scope names cut to 64 bytes; at most 256 distinct user scope names per Script, then "(too many scopes)".
- `debug` holds only `profilebegin` and `profileend`.
- Recording is off until the overlay is shown or `get_profile` asks; the overlay always starts hidden.
- `get_profile`: `seconds` default 2, max 10; `top` default 25; timeline cut at depth 6 and 0.05 ms.
- Capture: `format` "anarchy-profile", `version` 1, times in microseconds from the first frame's start.
- Any GL error ends the studio: every new GL call is proven in `gpu-timer-check` first; timer queries are optional entry points.
- Never commit in the `main` worktree. Work on branch `microprofiler` in `../AnarchyEngine-CPP-microprofiler`.

## Deviation from the spec (decided while planning)

- **Colours:** the spec put the overlay's colours in the studio theme editor. The player has no studio theme, and the overlay is drawn over the 3D scene rather than studio chrome, so it uses one fixed dark palette in both programs. The spec is updated in Task 10.
- **Where the renderer runs:** `Renderer::draw` runs on the UI thread inside `GameView::renderContent`; the engine's render thread hands snapshots to `SceneFeed`. So pass scopes and the GPU row are recorded from the UI thread, and the Render row holds Prepare / Perform (snapshot handoff) / Present / PostRender.

## Review Focus

1. **Profiler toggled mid-scope** (shown while a scope is open, hidden while one is open): an unmatched end must be ignored and an unmatched begin closed at the frame end, never a crash or a scope that never ends. Tested in Task 1.
2. **A Script that yields inside `profilebegin` every frame** (common in loops with `task.wait`): the scope is closed at the yield and warned once, not once per frame. Tested in Task 5.
3. **Studio with two Scene Views, the overlay on view 2, then view 2 closed:** the overlay moves to the remaining view on the next show, with no dangling pointer. Tested in Task 7.
4. **`get_profile` while the overlay is hidden and the studio window is minimized** (no UI paints, so no UI-thread collection): the tool must collect itself and still return frames. Tested in Task 9.
5. **Opening a capture whose events reference scope or thread indices out of range, or a truncated file:** refused with a message; the live overlay is unchanged. Tested in Task 2.

---

### Task 1: Recorder core (`src/profiler/`)

**Files:**
- Create: `src/profiler/Profiler.hpp`, `src/profiler/Profiler.cpp`, `src/profiler/ProfileStats.hpp`, `src/profiler/ProfileStats.cpp`
- Create: `tests/ProfilerTest.cpp`
- Modify: `CMakeLists.txt` (add `add_library(profiler STATIC ...)`, link it PUBLIC from `engine_core`, add `profiler-tests`, register it with ctest and `anarchy_warnings`)

**Interfaces (produces):**

```cpp
namespace profiler {
enum class Group : std::uint8_t { Engine, Physics, Render, Script, User, Gpu };
using ScopeId = std::uint32_t;
using CauseId = std::uint16_t;
inline constexpr CauseId kNoCause = 0xffff;
inline constexpr std::size_t kRingEvents = 65536;
inline constexpr std::size_t kHistoryFrames = 300;
inline constexpr std::size_t kMaxNameBytes = 64;

struct ScopeInfo { std::string name; Group group; std::string key; };  // key: Script GUID, or empty
struct ScopeRecord { std::uint16_t row; ScopeId scope; CauseId cause; std::uint8_t depth; std::uint64_t start_ns, end_ns; };
struct Frame { std::uint64_t start_ns = 0, end_ns = 0; std::vector<ScopeRecord> scopes; };
struct History {
    std::vector<std::string> rows;          // "Sim", "Render", "UI", "GPU", then others in registration order
    std::deque<Frame> frames;               // oldest first, at most kHistoryFrames
    std::vector<ScopeInfo> scopes;          // indexed by ScopeId
    std::vector<std::string> causes;        // indexed by CauseId
    std::uint64_t dropped = 0;
    int gpu_lag_frames = 0;
    std::string capture_name;               // non-empty when this came from a file
};

bool enabled();                              // one relaxed atomic load
void acquire();  void release();             // recording on while count > 0
ScopeId intern(std::string_view name, Group group);
ScopeId intern_keyed(std::string_view key, std::string_view name, Group group); // renames on a new name
CauseId intern_cause(std::string_view cause);
void register_thread(const char* row);       // once per thread; a row name may be shared by successive threads
void begin(ScopeId scope, CauseId cause = kNoCause);
void end();
void frame_boundary();                       // render thread, at the start of Prepare
void gpu_scope(ScopeId scope, std::uint8_t depth, std::uint64_t start_ns, std::uint64_t end_ns, int lag_frames);
std::uint64_t now_ns();
void set_clock_for_testing(std::uint64_t (*clock)());   // nullptr restores steady_clock
void reset_for_testing();                    // empties rings and history, keeps ids
void collect();                              // drain rings into the live history; any thread, serialized
void set_paused(bool paused);  bool paused();           // paused freezes the view copy
void show_capture(History capture);  void close_capture(); bool showing_capture();
template <class Fn> void with_view(Fn&& fn);  // fn(const History&): frozen/capture when paused, else live
void with_live(const std::function<void(const History&)>& fn);

class Scope {                                 // RAII; records only when enabled() at construction
public:
    explicit Scope(ScopeId id, CauseId cause = kNoCause);
    ~Scope();
};
}  // namespace profiler

#define PROFILE_SCOPE(name, group) /* static id per call site + profiler::Scope */

// ProfileStats.hpp
struct ScopeStat { std::uint16_t row; ScopeId scope; CauseId cause; double max_ms, avg_ms, frame_ms, calls_per_frame, share; };
enum class StatSort { Max, Avg, Frame, Calls, Share, Name, Row };
std::vector<ScopeStat> compute_stats(const History& history, std::size_t selected_frame);
void sort_stats(std::vector<ScopeStat>& stats, const History& history, StatSort sort);
double frame_ms(const Frame& frame);
```

Collector rules: each ring keeps an open stack; an end with an empty stack is ignored; at each frame boundary, scopes still open on a thread that has gone quiet stay open (they close when their end arrives); a completed scope goes into the frame whose span holds its start (binary search); scopes starting before the oldest frame are dropped; frames past 300 drop from the front. GPU scopes arrive late and are placed the same way.

- [ ] **Step 1: Write the failing tests** in `tests/ProfilerTest.cpp` with a fake clock (`static std::uint64_t gNow; set_clock_for_testing([]{ return gNow; })`): disabled scopes record nothing; nesting depth 0/1/2; frames from three boundaries; a Sim-thread scope from a second `std::thread` lands in the frame holding its start; `compute_stats` max/avg/frame/calls; ring overflow (`kRingEvents + 10` begins) counts `dropped` and the next frame records normally; an unmatched end is ignored; a scope begun while disabled and ended while enabled records nothing; pause freezes `with_view` while `with_live` moves on; resume returns to live; 301 frames keep 300; `intern_keyed` renames; names cut to 64 bytes.
- [ ] **Step 2: Run** `cmake --build build --target profiler-tests && ./build/profiler-tests` — expect compile failure (no `profiler/Profiler.hpp`).
- [ ] **Step 3: Implement** `Profiler.cpp` (rings: `std::array<Event,kRingEvents>`, atomic `write`/`read`, `std::atomic<std::uint64_t> dropped`; thread_local `Ring*`; registry and history under one `std::mutex`) and `ProfileStats.cpp`.
- [ ] **Step 4: Run** `./build/profiler-tests` — expect `ok`.
- [ ] **Step 5: Commit** `git commit -m "Add the profiler recorder: per-thread rings, frame history, and scope stats"`.

### Task 2: Capture files and the MCP report (`src/profiler/ProfileJson.*`, studio_core)

**Files:**
- Create: `src/profiler/ProfileJson.hpp`, `src/profiler/ProfileJson.cpp` (added to `STUDIO_CORE_SOURCES`; uses `engine_core::JsonValue`)
- Modify: `tests/ProfilerTest.cpp`, `CMakeLists.txt` (profiler-tests links `studio_core`)

**Interfaces (produces):**

```cpp
namespace profiler {
std::string write_capture(const History& history, const std::string& place, const std::string& created_utc);
bool read_capture(std::string_view text, History& out, std::string& error);   // out untouched on failure
struct ReportOptions { int top = 25; bool include_timeline = true; };
engine_core::JsonValue build_report(const History& history, const ReportOptions& options);
std::string capture_file_name(std::time_t when);   // "profile-20261004-021503.aprof.json"
}
```

- [ ] **Step 1: Write failing tests:** round trip (write then read gives equal rows, frames, scope names/groups/keys, causes, events, gpu lag, within 1 µs); refusals with history unchanged for: wrong `format`, `version` 2, truncated text, event thread index out of range, scope index out of range, negative duration, `frames` not increasing; `build_report` on a fixed history: frame count, avg/p95/max, `over_budget`, top rows sorted by max, slowest frame tree depth-capped at 6 and pruned below 0.05 ms, `gpu_lag_frames`.
- [ ] **Step 2: Run** `./build/profiler-tests` — fails to link (`write_capture` undefined).
- [ ] **Step 3: Implement** with `compact_json` for writing (files stay small) and `parse_json` for reading.
- [ ] **Step 4: Run** — `ok`.
- [ ] **Step 5: Commit** `"Write and read profile captures, and build the profile report"`.

### Task 3: Engine timing points

**Files:**
- Modify: `src/engine_core/Engine.cpp` (`simulation_loop`, `render_loop`, `step_physics`), `src/engine_core/ScriptRuntime.cpp` (`fire_phase` input dispatch, tool-step dispatch), `src/engine_core/AudioWorld.cpp` (step), `src/engine_core/PhysicsWorld.cpp` (substep)
- Create: `sandbox/profiler_tests.cpp` (add to `sandbox` target)

Scopes (Group): Sim row — "Simulation step", "Commands", "PreAnimation", "Input dispatch", "Events", "Physics" with "Substep" (Physics), "Heartbeat", "Step instances", "Scripts", "Tools", "Audio", "Tool step" (paused edit mode). Render row — "Prepare" with "Lock wait", "RenderStepped", "PreRender", "Snapshot copy"; "Perform", "Present", "PostRender" (Render). `register_thread("Sim")` / `register_thread("Render")` at the top of each loop; `frame_boundary()` at the top of each render iteration.

- [ ] **Step 1: Failing sandbox test** `PF1 a running engine records frames with Sim and Render scopes`: `profiler::acquire()`, start an `Engine` with a game and resume it, sleep ~300 ms calling `profiler::collect()`, stop; expect ≥ 5 frames, rows contain "Sim" and "Render", a "Simulation step" and a "Prepare" scope exist; `release()`.
- [ ] **Step 2: Run** `./build/sandbox "[PF1]"` — FAIL.
- [ ] **Step 3: Add the scopes.**
- [ ] **Step 4: Run** `./build/sandbox "[PF1]"` and the whole `./build/sandbox` — pass.
- [ ] **Step 5: Commit** `"Time the engine's simulation and render loops"`.

### Task 4: Script scopes with their cause

**Files:**
- Modify: `src/engine_core/ScriptRuntime.hpp/.cpp` (`Thread::cause`, `resume_one`, `start_listener(..., const char* cause)`, wake sites, scope id cache), `src/engine_core/ScriptBindings.cpp` (`signal_connect` passes the signal's name; `task.spawn`/`delay` set "spawn"/"delay"), `sandbox/profiler_tests.cpp`

Cause strings (static): first run "start"; `task.wait`/`wait` wake "wait"; `task.defer` and a yield with no park "defer"; `task.spawn` "spawn"; `task.delay` "delay"; `WaitForChild` "WaitForChild"; `Signal:Wait()` wake "Wait"; a listener: the signal's member name (`InputBegan`, `Heartbeat`, `Changed`, the event's name, ...); `require` "require".
Scope: `intern_keyed(guid, name, Group::Script)`; console VM → "Command line"; module → `intern_keyed(module guid, module name, Script)` with cause "require".

- [ ] **Step 1: Failing tests** `PF2` (ScriptRig): a Script connecting `RunService.Heartbeat` and calling `task.wait()` in a loop records scopes named after the Script with causes "start", "Heartbeat", and "wait"; `PF3` a renamed Script keeps one scope id with the new name; `PF4` `require` records a nested ModuleScript scope at depth+1 with cause "require".
- [ ] **Step 2: Run** `./build/sandbox "[PF2],[PF3],[PF4]"` — FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass; whole sandbox passes.
- [ ] **Step 5: Commit** `"Record each Script's resumes, and what resumed them"`.

### Task 5: `debug.profilebegin` / `debug.profileend`

**Files:**
- Modify: `src/engine_core/ScriptRuntime.cpp` (`open_host_libraries` adds the `debug` table after sandboxing), `src/engine_core/ScriptBindings.hpp/.cpp` (`debug_profilebegin`, `debug_profileend`, register `lua_note_host_library("debug")`), `src/engine_core/LuaApi.cpp` (docs for `debug`, `profilebegin`, `profileend`), `ScriptRuntime::Thread` (`user_scopes` count), `tests/LuauCompleteTest.cpp` (expectations that `debug` is missing now expect only the two members), `sandbox/profiler_tests.cpp`

- [ ] **Step 1: Failing tests** `PF5` user scopes nest under the Script scope; `PF6` a scope open at `task.wait()` is closed at the yield, and one warning prints however many frames repeat it; `PF7` a stray `profileend()` warns once; `PF8` `debug.profilebegin(5)` is a Lua error; a 100-byte name is cut to 64; the 257th distinct name records as "(too many scopes)"; `PF9` `debug.traceback`, `debug.getinfo` are nil; analysis reports no unknown global for `debug.profilebegin("x")`.
- [ ] **Step 2: Run** `./build/sandbox "[PF5],[PF6],[PF7],[PF8],[PF9]"` — FAIL.
- [ ] **Step 3: Implement.** Warnings use `append_output(OutputKind::Print, "Warning: ...")`, keyed by (Script, kind) in a `std::set`, cleared on `on_start`.
- [ ] **Step 4: Run** sandbox, `./build/engine-tests` — pass.
- [ ] **Step 5: Commit** `"Let scripts mark their own sections with debug.profilebegin"`.

### Task 6: GPU timer and renderer pass scopes

**Files:**
- Modify: `src/runner/gl.hpp/.cpp` (types `GLint64`, `GLuint64`; constants `GL_TIMESTAMP=0x8E28`, `GL_QUERY_RESULT=0x8866`, `GL_QUERY_RESULT_AVAILABLE=0x8867`; optional entry points `glGenQueries`, `glDeleteQueries`, `glQueryCounter`, `glGetQueryObjectiv`, `glGetQueryObjectui64v`, `glGetInteger64v`; `bool GlTimerQueries()`)
- Create: `src/runner/GpuTimer.hpp`, `src/runner/GpuTimer.cpp` (in `STUDIO_SOURCES`), `tests/GpuTimerCheck.cpp` (`gpu-timer-check`, by hand)
- Modify: `src/runner/Renderer.hpp/.cpp` (owns a `GpuTimer`; `PassScope` around shadow, geometry, light, sky, transparency, merge, tone map, grid, outlines, handles; `gpu_.frame()` at the end of `draw`; `gpu_.shutdown()` in `shutdown`)

```cpp
class GpuTimer {
public:
    bool available() const;            // entry points loaded and the pool made
    void begin(profiler::ScopeId scope);
    void end();
    void frame();                      // reads finished queries, pushes gpu_scope; never waits
    void shutdown();                   // context current
};
```

- [ ] **Step 1: Write** `tests/GpuTimerCheck.cpp` (hidden GL 3.3 window as `amesh-gl-check`): 10 frames of begin/clear/end, then `frame()`; expect `glGetError()==0` after every call, at least one GPU scope collected with a duration > 0 and lag 1–4.
- [ ] **Step 2: Run** `./build/gpu-timer-check` — fails to build.
- [ ] **Step 3: Implement** GL additions, `GpuTimer`, and renderer scopes.
- [ ] **Step 4: Run** `./build/gpu-timer-check` and `./build/scene-render-check` from the repo root — both pass, no GL errors.
- [ ] **Step 5: Commit** `"Time the renderer's passes on the CPU and the GPU"`.

### Task 7: The overlay and GameView

**Files:**
- Create: `src/runner/ProfilerOverlay.hpp/.cpp` (in `STUDIO_SOURCES`)
- Modify: `src/runner/GameView.hpp/.cpp`, `tests/StudioLayoutTest.cpp` (or a new `tests/ProfilerOverlayTest.cpp` in `studio-tests`)

```cpp
namespace runner {
// One for the process: what every GameView's overlay shows.
struct ProfilerUi {
    static ProfilerUi& get();
    bool shown() const;  void setShown(bool);        // acquire()/release() follow it
    void togglePaused();                             // profiler::set_paused
    GameView* owner = nullptr;                       // the view that draws it
    enum class Tab { Timeline, Scopes } tab = Tab::Timeline;
    double split = 0.45;                             // the lower half's share of the view
    std::function<void()> save;                      // null hides Save
    std::function<void()> changed;                   // tab or split changed (preferences)
};
class ProfilerOverlay : public jadefx::Node { ... };  // Painter-drawn; header, graph, Timeline, Scopes
}
```

GameView: owns a `ProfilerOverlay` child laid out over the top `kHeader + kGraph + split * height` of the view, visible when `ProfilerUi::get().shown() && owner == this`; `handleFocusGained` and a press make it the owner; the destructor clears `owner`; `syncPointerLock` treats a lock as unwanted while shown; `renderContent` registers the UI thread once, times "Paint" / "Scene View" / "GuiLayer", calls `profiler::collect()` each paint while recording; player view adds a scene key hook for Cmd+F6 / Cmd+P.

- [ ] **Step 1: Failing studio tests:** Cmd+F6 (`noteKey(F6, true, false, ModControl)`) shows the overlay on the last clicked Scene View only, with two views open; Cmd+P pauses (`profiler::paused()`); both work with a script editor focused; `MouseBehavior = LockCenter` locks, showing the overlay frees it with MouseBehavior still LockCenter, hiding then clicking locks again; clicking a graph bar selects that frame and pauses; clicking the Scopes tab switches; clicking a column header sorts; closing the owner view then pressing Cmd+F6 twice shows it on the remaining view; `requestCapture` pixels do not change when the overlay shows.
- [ ] **Step 2: Run** `./build/studio-tests` — FAIL.
- [ ] **Step 3: Implement** overlay drawing (fixed palette, editor font), hit testing, zoom/pan, tooltips, table sorting.
- [ ] **Step 4: Run** `./build/studio-tests` — pass.
- [ ] **Step 5: Commit** `"Draw the profiler over the Scene View"`.

### Task 8: Studio menus, capture Save/Open, preferences

**Files:**
- Modify: `src/ide/IdeLayout.cpp` (View → Profiler Cmd+F6, Pause Profiler Cmd+P; File → Open Profile Capture…), `src/ide/IdeLayoutProject.cpp` (save/open dialogs), `src/ide/Preferences.hpp/.cpp` (`profiler_tab`, `profiler_split`), `tests/StudioLayoutTest.cpp`, `tests/PreferencesTest.cpp`

- [ ] **Step 1: Failing tests:** the View menu has "Profiler" with F6+Control and "Pause Profiler" with P+Control; File has "Open Profile Capture…"; Preferences round-trips tab and split and clamps split to 0.2–0.8; opening a capture through the studio's open helper (`open_profile_capture(path)`) shows the overlay paused with the capture name; a bad file leaves it unchanged and prints why.
- [ ] **Step 2–4:** implement and run `./build/studio-tests`.
- [ ] **Step 5: Commit** `"Add the profiler to the studio's menus, with capture files"`.

### Task 9: MCP `get_profile`

**Files:**
- Modify: `src/ide/McpToolSpecs.cpp`, `src/ide/McpTools.cpp`, `tests/McpTest.cpp`

Runs on the server thread: when not paused and nothing records, `acquire()`, loop `collect()` every 50 ms for `seconds`, `release()`; otherwise collect once. Reads `with_view`. With `path`, writes `write_capture` there.

- [ ] **Step 1: Failing tests:** with an engine running and no overlay, `get_profile {"seconds":0.5}` returns `frames > 0`, `top` rows, `slowest_frame` with per-thread trees, `gpu_lag_frames`; with `path` writes a file `read_capture` accepts; while paused returns the frozen history's frame count without waiting; `seconds: 30` is clamped to 10 (checked through the reported `recorded_for` being ≤ 10 when the engine is idle — use 0.2 to keep the test short and check the clamp through the schema maximum).
- [ ] **Step 2–4:** implement; `./build/mcp-tests` passes.
- [ ] **Step 5: Commit** `"Add get_profile to the studio's MCP tools"`.

### Task 10: Player, screenshots, docs, final verification

**Files:**
- Modify: `src/player/main.cpp` (ProfilerUi::save writes `capture_file_name` into the project folder, or the folder holding the program or its .app; prints the path)
- Create: `tests/ProfilerDemo.cpp` (`profiler-demo out-dir`, by hand: a place with a slow Script using `debug.profilebegin("pathfind")`, a spinning GameObject, the overlay shown; saves PNGs of the Timeline and the Scopes tab after the history fills)
- Modify: `README.md` (a Profiler section), `docs/superpowers/specs/2026-10-03-microprofiler-design.md` (palette and renderer-thread notes)

- [ ] **Step 1:** Build everything; run `ctest` (all suites) from `build`.
- [ ] **Step 2:** Run `./build/profiler-demo <scratch>`; look at the PNGs; fix what looks wrong.
- [ ] **Step 3:** Run the studio (bundle-resources target) and the player by hand with the sample place; Cmd+F6, Cmd+P, save, reopen.
- [ ] **Step 4:** Update README and spec.
- [ ] **Step 5: Commit** `"Save profiles from the player, and document the profiler"`.
