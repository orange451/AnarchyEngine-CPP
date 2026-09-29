# Anarchy Engine Code Quality Review

| | |
|---|---|
| **Date** | 2026-09-28 |
| **Commit** | `dfc2444` on `main` |
| **Scope** | All of `src/` (48,890 lines in 159 files), `tests/` and `sandbox/` (17,265 lines), and the build files |

> **Bottom line.** Line by line, the code is tidy: almost no manual memory management, consistent RAII, a written and enforced threading contract, and a clean `/W4` build. The problems sit one level up. Five critical defects leak memory without bound, end the studio, or silently lose work in ordinary use. Several more are crashes or undefined behavior that a script or a file on disk can trigger. Five oversized units hold most of the complexity, and helper code is copied between them.

## How to read this report

| Severity | Meaning |
|---|---|
| **Critical** | Can terminate the studio, corrupt a project on disk, or silently lose user work in ordinary use. |
| **High** | Undefined behavior, use-after-free, or a crash that needs unusual input or timing. |
| **Medium** | Wrong behavior with limited impact, a significant performance or maintainability problem, or a latent hazard. |
| **Low** | Style, naming, dead code, or a minor inefficiency. |

**Latent** means the code is wrong but nothing in the current code base reaches the bad path yet.

Line numbers refer to commit `dfc2444`. Paths in the tables drop the `src/` prefix and the package folder when the file name is unique.

**Method.** Every source file was read in full, split across ten subsystem reviews. Each finding in section 2 was re-checked by hand against the source. The studio's own targets were rebuilt at `/W4` to collect compiler warnings. Nothing was run under a sanitizer, and no code was changed. Runtime figures that are estimates say so.

## Contents

1. [Executive summary](#1-executive-summary)
2. [Verified defects](#2-verified-defects)
3. [Other notable defects](#3-other-notable-defects)
4. [Subsystem reviews](#4-subsystem-reviews)
5. [Cross-cutting problems](#5-cross-cutting-problems)
6. [What is working well](#6-what-is-working-well)
7. [Recommended order of work](#7-recommended-order-of-work)
8. [Appendix: metrics](#8-appendix-metrics)

---

## 1. Executive summary

### Where the problems are

Three themes account for nearly everything in this report.

1. **Resource lifetime in the script runtime.** Coroutines and signal callbacks created during a play session are never released before Stop. When Lua memory finally runs out, the studio process terminates.
2. **Failure handling at the edges.** Capacity limits end in `std::abort()` instead of an error, even when a user script or files on disk drive them. Project save is not transactional. UI actions that cannot get a lock give up without saying so.
3. **Size and duplication.** `ScriptRuntime`, `IdeLayout`, `LuauComplete`, `DataModel`, and `Project` each mix many responsibilities. Helpers are copied between files, sometimes byte for byte and sometimes with drift that already produces inconsistent behavior.

### Scorecard

*Correctness risk* is the worst verified defect in the subsystem. *Maintainability* is Poor for god classes or functions plus heavy duplication, Fair for localized problems, and Good for clean code.

| Subsystem | Lines | Correctness risk | Maintainability | Headline |
|---|---:|---|---|---|
| Script runtime and Lua bindings | 5,252 | Critical | Poor | Leaks every coroutine and `Connect` callback; running out of Lua memory ends the process |
| Core world model | 5,452 | Critical | Poor | Event-queue overflow aborts; `DataModel` declares 158 member functions |
| Project load and save | 3,405 | Critical | Fair | Save is not transactional; an oversized project aborts on read |
| Script editor | 6,147 | Critical | Poor | Closed-tab text overwrites newer changes at Stop; whole-document work on every keystroke |
| Script analysis and completion | 7,253 | High | Poor | A second Luau parser in the IDE, with no recursion limit |
| Datatypes, instances, services | 3,768 | High | Fair | Script-reachable out-of-bounds read; three copies of one Luau type template |
| Studio shell | 5,333 | Medium | Poor | A 99-method `IdeLayout`; one guard that can never fire |
| Rendering, MCP, bridge, preferences | 6,419 | Medium | Fair | Unauthenticated MCP by default; one 777-line function |
| Panels | 5,862 | Medium | Fair | Poll the whole world every frame under a lock |
| Tests | 17,265 | n/a | Fair | Two frameworks; two test binaries are never run |

### Fix these first

1. Release coroutine registry refs when threads finish, and release `Connect` callback refs on disconnect ([C1](#c1-every-script-coroutine-is-kept-alive-until-stop), [C2](#c2-signal-callbacks-are-kept-alive-until-stop)).
2. Catch Lua exceptions at the C++ boundary and add a last-resort handler to both engine loops ([C3](#c3-running-out-of-lua-memory-terminates-the-studio)).
3. Make event and command queue overflow recoverable instead of aborting ([C4](#c4-a-full-event-queue-aborts-the-process)).
4. Make `save_tree` safe to fail partway through ([C6](#c6-a-failed-save-can-leave-the-project-half-written-and-unloadable)).
5. Store closed-tab buffers only during a test ([C9](#c9-stop-writes-old-closed-tab-text-over-newer-script-changes)).
6. Bound the instance count when reading a project ([C5](#c5-an-oversized-project-aborts-the-studio)).
7. Apply the small targeted fixes: non-finite hue ([C7](#c7-color3fromhsv-reads-outside-an-array-for-a-nan-or-infinite-hue)), a parser recursion limit ([C8](#c8-the-completion-parser-can-overflow-the-stack)), key-hook removal on detach ([C10](#c10-the-color-picker-key-hook-can-outlive-its-editor)), and the Properties focus test ([C11](#c11-a-disk-check-can-reload-the-tree-during-a-properties-edit)).

---

## 2. Verified defects

Every entry here was confirmed against the source at `dfc2444`.

| ID | Severity | Defect |
|---|---|---|
| C1 | Critical | Every script coroutine is kept alive until Stop |
| C2 | High | Signal callbacks are kept alive until Stop |
| C3 | Critical | Running out of Lua memory terminates the studio |
| C4 | Critical | A full event queue aborts the process |
| C5 | High | An oversized project aborts the studio |
| C6 | Critical | A failed save can leave the project half-written and unloadable |
| C7 | High | `Color3.fromHSV` reads outside an array for a NaN or infinite hue |
| C8 | High | The completion parser can overflow the stack |
| C9 | Critical | Stop writes old closed-tab text over newer script changes |
| C10 | High | The color-picker key hook can outlive its editor |
| C11 | Medium | A disk check can reload the tree during a Properties edit |

### C1. Every script coroutine is kept alive until Stop

**Severity: Critical.** `src/engine_core/ScriptRuntime.cpp:1108-1126` (`new_thread`), `1402-1413` (`start_listener`)

Every coroutine the runtime creates is appended to `threads_` and pinned in the Lua registry. Nothing releases either one until `close_vm` clears them at Stop.

```cpp
threads_.emplace_back();                 // 1109
Thread& thread = threads_.back();
...
thread.anchor = lua_ref(state_, -1);     // 1120: never passed to lua_unref
```

`kill_script` (754-758) only sets `dead`. `drop_dead` (908-916) only removes pointers from the ready, sleep, and defer lists. A thread that finishes is marked dead and kept. Because `start_listener` creates a new coroutine for every signal delivery, a single `RunService.Heartbeat:Connect(fn)` leaks one coroutine per simulation step, about 60 a second. Each also leaves a `std::list` node that the Lua memory budget does not count, and `kill_script` and `ready()` walk these lists linearly, so they slow down as a session runs.

**Fix.** Release the anchor with `lua_unref` and erase the `Thread` when it finishes or is killed. An owning wrapper for registry refs would make this hard to get wrong again.

### C2. Signal callbacks are kept alive until Stop

**Severity: High.** `src/engine_core/ScriptRuntime.cpp:2124-2126` (`signal_connect`), `2217-2223` (`connection_disconnect`)

```cpp
lua_pushvalue(state, 2);
const int ref = lua_ref(state, -1);      // 2125: captured by the handler, never released
```

`Disconnect` tombstones the C++ connection and drops the handler, but the registry ref is never released. The Lua closure and everything it captures, including instance handles, stays alive until the VM closes. Code that connects per instance, or reconnects every frame, leaks one closure per call.

Related: the connection metatable registers `connection_gc` as `__gc` (572-573). Luau does not call `__gc`; userdata destructors are registered with `lua_newuserdatadtor`. That destructor has never run. It is harmless today only because `Connection` is trivially destructible.

**Fix.** Keep the ref with the connection and release it on `Disconnect`, `disconnect_script`, and `disconnect_scripted`. Use `lua_newuserdatadtor` for userdata that need cleanup.

### C3. Running out of Lua memory terminates the studio

**Severity: Critical.** `src/engine_core/Engine.cpp:266` (and the catch blocks at 328, 333, 342, 364, 373); `src/engine_core/ScriptRuntime.cpp:409-436`

The vendored Luau is built with C++ exceptions (`LUA_USE_LONGJMP 0`). When the budgeted allocator refuses memory at the 64 MB limit, every `lua_*` call made from C++ outside `lua_resume` or `lua_pcall` throws `lua_exception`. That includes `lua_newthread` and `lua_ref` in `new_thread`, `lua_newuserdata` in `push_instance`, and `lua_pushstring` in `make_ready`. `heartbeat()` has no handler, and every `try` in both engine loops catches only one type:

```cpp
} catch (const ContractViolation&) {     // Engine.cpp:266
    contract_count_.fetch_add(1);
}
```

The exception leaves the thread function and `std::terminate` ends the process. Together with C1, this is where any long play session with a per-frame handler ends up. At an estimated 1 to 2 KB per leaked coroutine, one 60 Hz Heartbeat listener reaches 64 MB in roughly 10 to 20 minutes.

Two side effects of the same build setting: the Luau panic handlers (`ScriptRuntime.cpp:475-478`, `LuaEngine.cpp:416-419`) never run, and the `co == nullptr` checks after `lua_newthread` can never be true.

**Fix.** Run C++-initiated Lua work, such as starting a listener and pushing its arguments, under `lua_pcall` or a `try` that turns the failure into a script error. Add a last-resort handler at the top of both engine loops that logs, stops the play session, and keeps the studio running.

### C4. A full event queue aborts the process

**Severity: Critical.** `src/engine_core/Events.cpp:10`, `266-268`; `src/engine_core/Contract.cpp:22-28`

```cpp
constexpr std::size_t kEventCapacity = 8192;          // Events.cpp:10

void EventQueue::enqueue(const Event& event) {
    if (events_.empty() || size_ == events_.size()) {
        contract_fail("event queue is full");        // 268
    }
```

`contract_fail` calls an installed handler, then `std::abort()`. Only the Catch2 suite installs a handler (`sandbox/tests.cpp:35`), so in the studio an overflow ends the process. An event is queued for each `ChildAdded`, `ChildRemoved`, `AncestryChanged`, and `Changed` that has a listener, and the queue drains only between phases. A script that parents a few thousand new instances under a parent with a `ChildAdded` listener within one step can pass 8,192 events before the next drain. The worker command queue aborts the same way at 4,096 entries (`DataModel.cpp:414-418`).

**Fix.** `InvalidationQueue` already handles overflow well: it drops entries and flags a full resync. The event and command queues should grow, or drop with a counted console warning, rather than abort on load a script can create.

### C5. An oversized project aborts the studio

**Severity: High.** `src/engine_core/Project.cpp` (`PlanReader::add` near line 394, and `build`); `src/engine_core/DataModel.cpp:459-460`

The project reader never limits how many instance files it builds, and the world aborts at its 16,384-instance capacity:

```cpp
if (world.slots.size() >= kMaxInstances) {
    contract_fail("instance capacity exhausted");   // DataModel.cpp:460
}
```

`compare_disk` builds the whole disk tree into a scratch world, and the studio runs that check whenever its window regains focus and before every save. A `git pull` or branch switch that takes `src/` past the cap kills the studio on the next alt-tab. The IDE's `catch (const std::exception&)` cannot intercept an abort. `Project::load` and `apply_changes` reach the same path.

**Fix.** Count nodes in `PlanReader` and fail with `ProjectError` before building anything. Check the remaining capacity before `apply_changes` creates instances.

### C6. A failed save can leave the project half-written and unloadable

**Severity: Critical.** `src/engine_core/Project.cpp:2106-2281` (`save_tree`)

`save_tree` writes, moves, and deletes files one at a time. It updates its record of what is on disk (`files_`) only after the last operation. Each file operation throws `ProjectError` on failure, and the function has no exception handling.

```cpp
for (const auto& [guid, files] : next) {             // 2188: one file at a time
    ...
}
...
files_ = std::unordered_map<std::string, Files>(      // 2265: record updated last
    std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
```

Renaming an instance that has children is a series of independent renames. If one fails partway through, for example from a sharing violation caused by a virus scanner or an open editor, a full disk, or a path over 260 characters, the old folder can lose its `init.json` while keeping some children. The next load then refuses the project with "a folder instance needs init.json". Because `files_` still describes the old layout, the next save also reports the studio's own moves as changes made outside. The header's promise that a conflicted save writes nothing (`Project.hpp:141-144`) covers only the pre-check. No test exercises a failed write.

**Fix.** Write every new file first, since GUID-based names never collide with old ones. Then update `files_`, then remove old files. Alternatively, update `files_` entry by entry as each operation succeeds.

### C7. `Color3.fromHSV` reads outside an array for a NaN or infinite hue

**Severity: High.** `src/engine_datatypes/Color3.cpp:204-215`

```cpp
const double h = (hue - std::floor(hue)) * 6.0;   // NaN for NaN or ±inf
const int sector = static_cast<int>(h) % 6;       // undefined; -2 on x86
...
return Color3{static_cast<float>(table[sector][0]), ...
```

`luaL_checknumber` accepts NaN and infinity (line 44), so `Color3.fromHSV(0/0, 1, 1)` or `Color3.fromHSV(math.huge, 1, 1)` in any script reads 48 bytes before the local table. In practice it returns a garbage color. Formally it is undefined behavior.

**Fix.** Treat a non-finite hue as 0 and clamp `sector` to the range 0 to 5.

### C8. The completion parser can overflow the stack

**Severity: High.** `src/ide/LuauComplete.cpp:2328-2348` (`parse_unary`), `2437-2445` (parenthesized expressions)

The IDE's own Luau parser recurses once per `not`, `-`, `#`, `(`, and nested `function`, with no depth limit. `depth_` (2585) counts lexical scopes, not recursion.

```cpp
if (is_kw("not")) {
    advance();
    ...
    parse_unary();                       // 2335: unbounded
```

A generated or pasted script with deep nesting before the caret crashes the studio on the next completion or hover. Diagnostics are unaffected because `Luau::Parser` limits its own recursion.

**Fix.** Count recursion in `parse_expr` and `parse_unary`. Past a limit, set `cut_` and return.

### C9. Stop writes old closed-tab text over newer script changes

**Severity: Critical.** `src/ide/IdeLayout.cpp:2779-2783`, `2812-2847`, `2999`

The header documents `kept_sources_` as "Source from an editor that was closed while the simulation was running" (`IdeLayout.hpp:370-371`). The tab-close handler stores the buffer on every close, including while stopped:

```cpp
tab->setOnClosed([this, id, editor] {
    if (editor && editor->isLoaded()) {
        kept_sources_[id] = editor->text();   // 2781: no check that a test is running
    }
});
```

At Stop, `restore_closed_edits` writes back every stored buffer that differs from the script's current Source, then captures the result as the place. An entry is forgotten only when its script is reopened (2766) or the place is closed (3085).

**Scenario.** While stopped, close a script's tab. Change that script some other way: Replace All in the Search pane, an MCP `write_script` call, or a change loaded from disk after a `git pull`. Press Play, then Stop. The script reverts to the text it had when the tab closed, the place is marked modified, and the next save writes the old text to disk.

**Fix.** Store buffers only while a test is running, and clear the map at Stop after use. More robust: record the Source revision at close and skip entries whose script changed since.

### C10. The color-picker key hook can outlive its editor

**Severity: High, with a narrow trigger.** `src/ide/IdeScriptEditor.cpp:837` (added); `576-580` and `878-885` (removed)

```cpp
color_edit_->key_hook = scene->addKeyHook([this](jadefx::KeyEvent& event) {   // 837
```

The hook is removed only while the editor still has a scene. The destructor checks `getScene() != nullptr`, and `close_color_picker` checks `scene != nullptr`. When editors are closed programmatically while the picker is open, for example by File > New or Open through `close_script_editors` from a keyboard shortcut, the editor is detached first, so neither path removes the hook. JadeFX clears a node's scene and hides its popups on detach but leaves scene key hooks alone, and the scene calls every hook on every key press. The next key press runs the lambda on an editor that may already be destroyed.

**Fix.** Remove the hook when the editor leaves its scene, or guard the lambda with a weak reference.

### C11. A disk check can reload the tree during a Properties edit

**Severity: Medium.** `src/ide/IdeLayout.cpp:2516`

```cpp
return InTextWidget(focused) && (Owning<IdeExplorer>(focused) != nullptr ||
                                 Owning<PropertiesPanel>(focused) != nullptr);
```

`Owning<T>` walks scene-node parents with `dynamic_cast<T*>` (61-69), but `PropertiesPanel` is not a scene node (`PropertiesPanel.hpp:48`), so the second test is always false. The header says a disk check waits while a Properties field is being typed in. Instead, a check on window focus can reload instances mid-edit, and the Properties page drops the half-typed value. `routeUndo` already uses the correct test at line 2908.

**Fix.** Use `properties_->owns(focused)`.

---

## 3. Other notable defects

These were found by the subsystem reviews with file and line evidence. They were not all re-verified by hand, but each cites the code it describes.

### 3.1 Security and robustness

| Severity | Location | Problem |
|---|---|---|
| Medium | `IdeLayout.cpp:1401`, `McpServer.cpp:259` | The MCP server can run Lua, write scripts, and delete instances, but checks a bearer token only when `ANARCHY_MCP_TOKEN` is set. It binds to loopback and checks `Origin`, which stops browsers and remote hosts. Any local process can still drive it, and its port is published in the studio registry. |
| Medium | `JsonMerge.cpp:8-13`, `PropertyBag.cpp:643-647` | The disk comparison serializes values in order to compare them. A NaN or infinite property value, which a script can set, makes `write_json` throw `std::invalid_argument`, so the disk check fails with "Can't read the project on disk". Comparing with `JsonValue::operator==` fixes this and is much faster. |
| Medium | `McpTools.cpp:158-160` | A client-supplied number is cast to `uint32_t` before the range check, which is undefined for `-1` or `1e300`. The same pattern appears at lines 707 and 1143, and in `StudioBridge.cpp:314`. |
| Medium | `McpTools.cpp:505-506` | `analysis.pump()` runs on an HTTP worker thread while the simulation is paused, although `ScriptAnalysis.hpp` allows only the simulation or UI thread. |
| Medium | `ChangeHistoryService.cpp:224-229` | `finish_recording` takes its id by reference. Its three internal callers (264, 275, 349) pass `recording_->id`, which the function moves out of and destroys two lines later. It is safe only because nothing reads `id` afterwards. |
| Medium | `IdeLayout.cpp:2276-2282` | Closing the studio during an MCP tool call joins the server thread from the UI thread while the tool call waits on the UI thread. Each wait times out, so shutdown can freeze for up to about 20 seconds. |
| Medium, latent | `DataModel.cpp:50-51` | The lock re-entrancy counter and the deferred-violation slot are process-wide `thread_local`s rather than per world. A thread holding one world's lock skips another world's mutex entirely. Several worlds coexist in one process, but none takes the lock today. |
| Medium, latent | `TaskScheduler.cpp:200-251` | `cancel_session_jobs` rebuilds the job vector while parked fibers keep raw pointers into the old buffer. Nothing in production calls `Signal::wait`, and the fiber code compiles out on Windows (lines 54 and 87-89). |

### 3.2 Unbounded memory growth

| Severity | Location | Problem |
|---|---|---|
| Medium | `ChangeHistoryService.hpp:204-207` | Edit undo and redo stacks have no cap. A destroy record stores the whole subtree including every script's source, and each script edit stores the full text before and after. |
| Medium | `ScriptAnalysis.cpp:992-998` | Each script id becomes a module in the Luau Frontend that is never evicted. Ids carry a generation, so creating and destroying scripts grows the frontend for the life of the process. |
| Medium | `InputRouter.hpp:54`, `IdeLayout.cpp:2776` | A text undo stack is created for every script ever edited and never erased, even across project opens. Each keeps the full text and every edit, uncoalesced and uncapped. |
| Medium | `ConsoleLog.cpp:154`, `318` | The console keeps every line and every printed table snapshot for the session. The runtime caps its own queue at 1,024 lines (`ScriptRuntime.cpp:187`), but the console's store has no limit. |

### 3.3 Behavior bugs

| Severity | Location | Problem |
|---|---|---|
| Medium | `IdeLayout.cpp:2638`, `2685`, `2758` | Cut, Paste, and Edit give up silently when a 5 ms read lock times out. Under simulation load the action does nothing, and nothing says why. |
| Medium | `IdeExplorer.cpp:333-336`, `358-367` | Each instance in a multi-select Delete or Cut takes its own 1 ms lock. Instances whose lock times out are silently left out of the operation. |
| Medium | `IdeLayout.cpp:2530-2536` | During a test, disk-scan failures are swallowed by an empty `catch`. The scan re-runs and fails again on every focus change. |
| Medium | `IdeLayout.cpp:3355-3357` | `confirm_overwrite` returns early when a prompt is already open and drops the continuation it was given, so "Save, then close" can end with neither. |
| Medium | `IdeScriptEditor.cpp:675`, `1023` | `missing_` is set when a script disappears and is never cleared. If undo brings the script back, its editor stays read-only and stops saving. |
| Medium | `UserInputService.cpp:134-140` | `set_active` clears queued input, keys, and buttons but not the last mouse position, so the first mouse move of a session reports a delta from the previous session. |
| Medium | `TestTriangle.cpp:28-40` | `set_position` has no thread check and emits no change signal, unlike every other setter in the package. |
| Medium, latent | `Project.cpp:1066-1071` | `Project::create(root, into)` clears the world, with undo disabled, before it knows the disk is writable. The IDE does not call this overload today. |
| Low | `ScriptRuntime.cpp:366-374` | Any Parent change on a running script kills it, and restarts it only when the new parent is not nil. The Roblox idiom `script.Parent = nil` therefore stops the script, and `script.Parent = x` at the top level runs the top level twice. |
| Low | `Project.cpp:128-139` | `write_file` leaves a `.tmp` file behind when the write fails, and reports a failed rename without its reason. |

### 3.4 Rendering

| Severity | Location | Problem |
|---|---|---|
| Medium | `Renderer.cpp:58-63` | Setup reads `glGetError` without first draining errors left by the UI renderer, and the scene view attempts setup only once. One stale error leaves the Scene View black for the rest of the session. |
| Medium | `GameView.cpp:175-186` | Switching tabs away from the Scene View deletes its GL program and buffers. Switching back re-reads the shader files and recompiles them. |
| Medium | `GameView.cpp:42`, `Renderer.cpp:56`, `149` | The viewport color is hard-coded three times, so the theme's Scene View background setting is painted over. |
| Medium | `Renderer.hpp:18-44` | `Renderer` owns three GL objects and has no destructor. They are freed only from a scene callback. |

---

## 4. Subsystem reviews

### 4.1 Script runtime and Lua bindings

**Files:** `ScriptRuntime` (2,520 + 268 lines), `LuaApi` (879 + 263), `LuaReflect` (717), `LuaEngine` (454 + 123), `ScriptHost` (28). Total 5,252.

**Verdict.** Careful in the small, unsound in the large. Stale handles fail closed everywhere, the output log is well built, Lua stack discipline is balanced, and `require` uses correct RAII guards. But everything a session creates lives as long as the VM ([C1](#c1-every-script-coroutine-is-kept-alive-until-stop), [C2](#c2-signal-callbacks-are-kept-alive-until-stop)), and the failure path is process termination ([C3](#c3-running-out-of-lua-memory-terminates-the-studio)).

**Main problems**

- **God class.** `ScriptRuntime` holds two VMs, a five-state coroutine scheduler, `WaitForChild`, the console log, two `require` caches, 34 Lua bindings, and input marshalling in one 2,520-line file. The bindings are friends and mutate the scheduler queues directly, so invariants such as "a thread is in at most one queue" are kept by convention across about 15 sites.
- **Three Luau state factories.** `LuaEngine::start`, `ScriptRuntime::create_state` with `open_host_libraries`, and `LuaReflect`'s `reflect_state` each open libraries and install callbacks. `LuaEngine` copies the library table, removed-globals list, allocator, interrupt, panic handler, and `print` from `ScriptRuntime`, and the copies already disagree: the execution budget is 1,000,000 in `LuaEngine.hpp:60` and 50,000 in `ScriptRuntime.hpp:161`. `LuaEngine` is used only by the runner and its own test.
- **Type-unsafe bindings.** C functions are stored as `void*` and restored with 17 `reinterpret_cast`s (`ScriptRuntime.cpp:1861`, `2468-2515`). Property writes dispatch by comparing type-name strings (1922-1971) and have no branch for numbers, so the first writable numeric property will fail with "cannot set".
- **Two sources of truth for services.** Services register through `register_lua_service`, but the runtime also hard-codes `kServiceClasses` and a magic index (`ScriptRuntime.cpp:60-61`). `is_a()` hard-codes `dynamic_cast`s that duplicate the class registry.
- **Documentation as code.** `LuaApi::build_docs` (`LuaApi.cpp:414-839`) is a 426-line function of about 330 hand-typed doc strings that nothing checks against the registry.
- **Unsynchronized globals.** `LuaReflect` keeps a process-global `lua_State`, a global `Job*`, and a step counter with no synchronization (`LuaReflect.cpp:36-45`, `91`, `108`). This is safe only while completion stays on the UI thread.
- **Permanent callbacks.** `ScriptRuntime::attach` binds four `this`-capturing jobs to `TaskScheduler`, whose `bind()` is permanent, so `detach()` cannot remove them. This is safe only because `Engine` destroys the runtime before the scheduler and never re-attaches.
- **Magic numbers with behavioral weight.** `kScriptTimeout = 50000` interrupts per resume kills a legitimate loop that fills a large grid; `kResumeBudget = 32` threads per step; `lua_pop(state, 6)` must track the metatable count by hand (line 592); `blocked_name[24]` truncates silently.

**Duplication**

- `task.spawn`, `task.defer`, and `task.delay` are three copies of one 20-line body (`ScriptRuntime.cpp:1627-1701`).
- `make_ready`, `make_ready_number`, and `make_ready_input` repeat the same unpark-and-push logic (930-956, 1462-1474).
- The signal lookup in `signal_connect` and `signal_wait` is duplicated (2127-2140, 2178-2192).
- The compile prologue appears five times across `ScriptRuntime`, `LuaEngine`, and `LuaReflect`.

**Longest functions:** `build_docs` 426, `require_module` 119, `dummy_index` 110, `open_host_libraries` 100, `instance_newindex` 73.

**Worth keeping:** instance handles that carry a world generation and are checked at every entry; the mutex-guarded, bounded, epoch-versioned output log; `require_module`'s RAII guards; sandboxing with per-thread sandboxes and memory and interrupt budgets.

### 4.2 Core world model

**Files:** `DataModel` (2,331 + 511), `Events` (509 + 231), `TaskScheduler` (415 + 85), `Engine` (394 + 138), `SnapshotPump` (199 + 85), plus `TableSnapshot`, `InvalidationQueue`, `DataModelLock`, `Contract`, and `types`. Total 5,452.

**Verdict.** Mechanically sound. Ownership is RAII, stale ids fail closed, the event queue handles connect and disconnect during dispatch correctly, and the thread contract is enforced at runtime. The problem is shape: `DataModel` is at once the instance base class, the world container and allocator, the lock, the hierarchy, the signal registry, the undo applier, the snapshot serializer, the physics integrator, and a Lua registrar.

**Main problems**

- **God class.** `DataModel` declares 158 member functions (83 public, 16 protected, 59 private), has a 34-field `State` struct, 6 friends, 15 `dynamic_cast`s, and 47 `contract_fail` sites. Every instance object carries fields only the root uses.
- **The core knows its leaves.** `DataModel.cpp` includes `GameObject`, `Script`, and `TestTriangle`. `HistoryProp::Position` and `record_position` exist only for the `TestTriangle` demo type. `DataModel.hpp` pulls three `engine_services` headers into every file that touches an instance.
- **Two record formats for one thing.** `PlaceRecord` (`DataModel.hpp:389-405`) and `AuthoredRecord` (`ChangeHistoryService.hpp:51-74`) share ten fields, and each has its own capture and restore code (`DataModel.cpp:1341-1381` against `1783-1823`, and `1464-1491` against `1825-1854`). A new authored field must be added in four places.
- **Performance near the instance cap.** `link_child` walks to the end of the sibling list on every insert (`DataModel.cpp:1011-1014`), so restoring the place at Stop is quadratic in sibling count. `SnapshotPump::apply_overrides` is O(overrides × instances) (146-162). `prepare_copy` copies the whole back buffer while holding the 2 ms render lock (164-181). `integrate_simulated` does a `dynamic_cast` per simulated object per physics substep.
- **Fiber scheduler with no production user.** About 250 lines of two-ABI naked assembly, sanitizer hooks, and stack bookkeeping in `TaskScheduler.cpp` serve only `Signal::wait`, which only the Catch2 suite calls. The code compiles out on Windows.
- **Dead code.** The move constructor and move assignment (`DataModel.cpp:203-245`, 43 lines of intricate rebinding) have no callers.
- **Inconsistent error channel.** A rejected write either aborts or is parked in a single thread-local, depending on whether the caller holds a lock (`reject_write`, 369-385). Multiple rejections collapse into one.

**Duplication**

- `spawn` and `adopt_slot` construct pooled instances the same way (`DataModel.cpp:516-538`, `1438-1461`).
- `destroy` and `retire_slot` tear them down the same way (588-603, 1398-1419).
- `apply_transform` and `apply_color` are 35 identical lines apart from the field (610-646, 648-682).
- Four separate descendant walks: `step_descendants`, `emit_ancestry`, `destroy_tree`, `authored_tree`.
- The id bit layout is retyped in about 16 places in `DataModel.cpp` and again in `SnapshotPump.cpp`.

**Longest functions:** `Engine::render_loop` 110, `Engine::simulation_loop` 99, `authored_tree` 85, `spawn` 78, `restore_place_unlocked` 72. The size problem here is at the class level, not the function level.

**Worth keeping:** the generation-checked id and slot scheme; the event queue's re-entrancy design; `InvalidationQueue`'s overflow-to-resync behavior; the small RAII guard types.

### 4.3 Datatypes, instances, and services

**Files:** `engine_datatypes` (`Vector2`, `Vector3`, `Color3`, `Enum`, `Transform`, `Color`), `engine_instances` (`GameObject`, `Script`, `ModuleScript`, `LuaSource`, `Folder`, `TestTriangle`), `engine_services` (`ChangeHistoryService`, `Game`, `RunService`, `SelectionService`, `UserInputService`). Total 3,768.

**Verdict.** Small and disciplined: no `new` or `delete`, no `const_cast`, no `catch`, and every `reinterpret_cast` is byte serialization. One script-reachable UB ([C7](#c7-color3fromhsv-reads-outside-an-array-for-a-nan-or-infinite-hue)). The dominant problem is copy and paste.

**Main problems**

- **One Luau type template, expanded by hand three times.** `Vector2.cpp`, `Vector3.cpp`, and `Color3.cpp` repeat the same userdata plumbing and arithmetic. About 230 of `Vector3.cpp`'s 335 lines have a near-verbatim twin in `Vector2.cpp`. The copies have drifted: `Vector2` `Max` and `Min` are variadic, while `Vector3`'s take two arguments.
- **Properties are spelled out, not declared.** Each exposed property appears in up to six places: `save_properties`, `default_properties`, `load_property`, a pair of `dynamic_cast` read and write shims, the registrar row, and the `HistoryProp` enum. `GameObject`'s default size of 1, 1, 1 is written in five places.
- **A second model of the world.** In `ChangeHistoryService`, `HistoryProp` parallels `Field`, joined by hand-written switches in five places across three files. `PropertyValue` is a seven-member pseudo-union, and `Mutation` embeds a full `AuthoredRecord` even for a one-property change. The service is a friend of `DataModel`, which also owns it.
- **Registration depends on the linker.** Two classes define `class_name()` out of line only so that their object files, and their static Lua registrars, get linked (`Folder.cpp:7`, instances README).
- **Key codes in three tables.** A 113-line switch of bare GLFW integers (`UserInputService.cpp:20-132`), the enum table (`Enum.cpp:32-142`), and constants in `UserInputService.hpp`.
- **One Lua class registered from two libraries.** `UserInputService` and `Selection` get their signals in `engine_services` and their methods in `ScriptRuntime`. This relies on `append_unique` replacing fields, which contradicts the contract written in `LuaApi.hpp`.
- **No tests** for `UserInputService`, the only cross-thread input queue.

**Duplication**

- `fuzzy_component`, `sign_of`, and `lerp_component` are identical in `Vector2.cpp:16-37` and `Vector3.cpp:19-40`.
- The userdata push, check, and convert functions appear three times (`Vector2`, `Color3`, `Enum`).
- `Vector3`'s normal and axis tables restate `Enum`'s.
- `undo()` mirrors `redo()` (`ChangeHistoryService.cpp:291-321`).

**Longest function:** `key_code_from_glfw` at 113 lines; everything else is under 40.

**Worth keeping:** `SelectionService`'s lock and revision discipline; `UserInputService`'s bounded queue that never drops a key release; `Vector2`'s operator table that drives both the metatable and the analysis registry, which is the pattern to extend to the other types; cheap `static_assert` drift guards.

### 4.4 Project load, save, and JSON

**Files:** `Project` (2,283 + 249), `PropertyBag` (664 + 88), `JsonMerge` (83 + 38). Total 3,405.

**Verdict.** Careful at the file level, weak at the transaction and scale levels. Each file write is atomic, GUID-based file names prevent collisions and case problems, every filesystem call uses `error_code`, and one exception type names the failing file. But a save as a whole is not atomic ([C6](#c6-a-failed-save-can-leave-the-project-half-written-and-unloadable)), disk input can abort the process ([C5](#c5-an-oversized-project-aborts-the-studio)), and the comparison machinery will not hold up near the 16,384-instance cap.

**Main problems**

- **Two reconciliation engines.** `outside_changes` (230 lines, 1297-1526) guards a save, and `compare_disk` (203 lines, 1559-1761) drives scan and apply. Both implement the same base, disk, and studio comparison, with different normalization and different failure behavior: a bad value on disk is a per-key conflict in one path and "Can't read the project" in the other. With `settle`, `apply_changes`, and `refresh_base`, this is 717 lines, 31 percent of the file.
- **`save_tree` has no transaction boundary.** It is 176 lines in seven phases, and its shape is the partial-save defect.
- **Whole-project work on every edit.** `unsaved()` re-serializes the entire project on every authored change, and every property write triggers it through the IDE's modified-flag refresh. It compares values by serializing each one up to four times.
- **Brute-force number formatting.** The shortest round-trip form is found with up to 17 `snprintf` and `istringstream` rounds per value (`PropertyBag.cpp:644-662`), with locale commas patched by hand. `std::to_chars` does this directly.
- **Quadratic walks.** `order_children_as` runs a linear search inside a sort comparator, then detaches and reattaches every child, emitting `ChildRemoved` and `ChildAdded` for a pure reorder. `outside_changes` prefix-scans every claimed file per moved folder. `apply_changes` calls the linear `find_guid` for each action.
- **Repeated full reads.** Every scan builds a full scratch world and re-reads every file, and a save walks `src/` three times.
- **Hand-kept invariants.** The in-memory base holds two representations of each file with three derivations and is rewritten from six sites. Its invariants live only in comments.
- **Ownership and dead code.** `Project` keeps a raw alias `game_` beside `owned_` with defaulted moves, and the two `load` overloads are copies of each other. `save_as`, `instance_for` with its map, and `register_project_class` have no callers.

**On replacing the JSON code with a library.** The canonical writer, with sorted keys, inline numeric arrays, and shortest floats, is exactly what general libraries do not produce by default, so keeping it is reasonable. The strict parser is sound (depth limit, BOM handling, surrogate pairs, duplicate-key rejection) but has no direct tests. The highest-value changes are `std::to_chars` and comparing with `operator==`, not a library.

**Longest functions:** `outside_changes` 230, `compare_disk` 203, `save_tree` 176, `apply_changes` 156, `refresh_base` 83. `PlanReader` is a 245-line class and the JSON `Parser` a 310-line class.

**Worth keeping:** atomic temp-and-rename writes; the `Name.guid` file layout; one error type with messages that name the file; `load(root, into)` validating in a scratch world before touching the live one.

### 4.5 Script analysis and completion

**Files:** `ScriptAnalysis` (1,822 + 156), `AnalysisDefinitions` (358), `LuauComplete` (3,718 + 110), `CompletionPopup` (1,014 + 75). Total 7,253.

**Verdict.** The analysis half is good: immutable per-job world snapshots, cancellation tokens, a single publisher, and a type definition file generated from the live class registry rather than written by hand. The completion half is a complete second Luau front end: a 185-line lexer and a 1,936-line `Resolver` class that parses, scopes, and infers types, running beside the `Luau::Frontend` the analyzer already drives. `Luau::autocomplete` from the vendored tree goes unused, so hover and completion can disagree with diagnostics.

**Main problems**

- **Mode-switched god class.** `Resolver` (`LuauComplete.cpp:750-2685`) has about 60 methods whose behavior changes with the flags `retain_`, `signing_`, `hover_at_`, and `cut_`.
- **Flag soup.** `Shape` (25-70) has 31 fields, 16 of them booleans, and acts as a tagged union by convention. Return inference is written three times (`note_return`, `copy_signature`, `DescribeSymbol`), and the copies already differ.
- **`require` runs module code on the UI thread.** Completing a `require` executes the module's Lua (`LuauComplete.cpp:1084`, `LuaReflect.cpp:697`) with only an instruction budget. The cache meant to avoid this is keyed on a hash of every script's source, including the live buffer, so every keystroke invalidates it.
- **Whole-tree copies as the unit of work.** `ScriptAnalysis::schedule` captures the world, including every script's source, before checking whether analysis is enabled (1429-1434). Completion copies the tree with sources on every keystroke and hover (`CompletionPopup.cpp:982-1012`).
- **Polling next to an unused signal.** The editor polls diagnostics every frame with full source copies, while `DiagnosticsSignal` and seven other public `ScriptAnalysis` methods have no callers.
- **Source rewriting.** `--!nonstrict` is rewritten to `--!strict` in the source text, but only when it is on the first line (963-971).
- **Linear lookups inside loops.** `WorldSnap::find`, `FindNode`, and `InstancePath` are all linear and are called per child or per ancestor.

**Duplication**

- The keyword table appears twice in one file (`LuauComplete.cpp:164-166`, `175-177`), and a third, different list in `LuauHighlight.cpp:22-25` includes `const` and `type`.
- The 46-line UTF-8 decoder `Utf32` is byte-identical in `LuauComplete.cpp`, `LuauHighlight.cpp`, and `ScriptPairs.cpp`.
- `require` path resolution is written twice in `ScriptAnalysis.cpp` (200-258, 498-540) and a third time in `LuauComplete.cpp`.
- Six primitive-type lists already disagree about `thread` and `buffer`.
- Parameter lists are formatted five different ways.

**Longest functions:** `Tokenize` 185, `analyze_job` 182, `Resolver::parse_stmt` 175, `member_of` 133, `DescribeSymbol` 133, `parse_function` 115.

**Suggested split of `LuauComplete.cpp`:** a shared lexer (or `Luau::Lexer`) used by highlighting, pairing, and completion; a `Utf8.hpp`; `Shape` split into value, signature, and return information; the resolver core; completion sites; and hover. Longer term, drive `Luau::autocomplete` on the analyzer's frontend and keep hand-written code only for `require` paths and directives.

**Worth keeping:** `AnalysisDefinitions` generated from the registry; `ScriptAnalysis`'s concurrency design; `PlaceTypes` with `FindChildMagic`, which gives scripts real per-instance types; `LuauCompleteTest`'s 24 scenario groups, which make a refactor safe.

### 4.6 Studio shell

**Files:** `IdeLayout` (3,491 + 406), `IdeDock`, `DockArrange`, `SavedLayout`, `IdePane`, `InputRouter`, `StudioRegistry`, `main.cpp`. Total 5,333.

**Verdict.** Careful about the usual failures of a shared-pointer scene graph: weak pointers for tabs, docks, and panes; an `alive_` token for MCP hops; a destructor that clears window hooks. No reachable use-after-free was found. The dominant problem is that `IdeLayout` is a god class.

**Main problems**

- **God class.** `IdeLayout` has 99 member functions, 66 data members (10 booleans, 9 raw pointers), and 96 lambdas that capture `this`. It owns docking, drag and drop, floating windows, layout persistence, the Window menu, project open and save with conflict handling, the play session, script-editor bookkeeping, MCP identity, key routing, cut and paste, toasts, and the window title. It is also the most-changed file in the repository: 55 of 165 commits touch it. The script-editor bookkeeping here includes the closed-tab replay behind [C9](#c9-stop-writes-old-closed-tab-text-over-newer-script-changes), which this report counts under the script editor.
- **God functions.** The constructor is 263 lines (488-750), `start_mcp` is 119, and `flushFrame` does six unrelated jobs every frame.
- **Lifetime by convention.** About 80 `this`-capturing callbacks are stored on objects that the main `Scene` also owns: key hooks, the frame tail, menus, and the explorer and search hosts. Only the 16 MCP hops check `alive_`. The rest are safe because JadeFX's `Application::launch` destroys its `Stage` before the app object, a fact that lives in another repository and is not written down here.
- **Member order contradicts dependencies.** `undo_router_` is destroyed before the console and editor tabs that hold raw pointers into it, and `properties_` is destroyed before its own dock page. This is benign today only because no callback fires during destruction.
- **State as booleans.** `testing_` and `stepping_` encode a three-state session with one impossible combination. `check_pending_`, `was_focused_`, and `noted_play_check_` form a disk-check state machine spread over four functions. `dialog_open_` and `prompt_open_` are always tested together.
- **Awkward contracts.** `save_open_project` never runs its continuation on success, so all three callers repeat `if (save_open_project(then) && then) then();`, and Overwrite All runs two full disk scans.
- **Linkage.** Seven helpers (`DragPoint`, `LocateDrag`, `DragKind`, `DragChoice`, `SmallestDockAt`, `ChooseDrop`, `GrowToFit`) have external linkage in namespace `ide`, outside the anonymous namespace.
- **Demo code in the product.** The shell hard-codes the `TestTriangle` demo type (Insert Triangle), `main.cpp` builds demo content into the production entry point, the View menu ships a placeholder item named "Maybe :)", and `IdeTreeTest` is compiled into the app but never used.

**Proposed decomposition.** Each class takes the listed state and methods; `IdeLayout` remains the composition root that builds menus and the ribbon.

| New class | Takes |
|---|---|
| `DockShell` | Docks, floating windows, drag and drop, drop marks, and the free drag helpers (about 850 lines) |
| `LayoutPersistence` | `save_layout`, `capture_layout`, `restore_layout`, `restore_floating`, `restore_window`, `default_layout`, `reset_layout` (about 500 lines) |
| `WindowMenu` | Window entries, `toggle_window`, `show_window`, `watch_close`, `new_scene_view` |
| `ProjectSession` | Open, save, conflicts, unsaved-change tracking, `check_disk`, alerts (about 650 lines) |
| `PlaySession` | Test, pause, resume, stop as one enum instead of two booleans |
| `ScriptEditors` | Open editors, `kept_sources_`, undo routing, flush and reapply; erase undo stacks on close |
| `StudioMcp` | `start_mcp`, identity, `alive_`, `OnUiThread` |
| `KeyRouter` | `routeUndo`, `routeDelete`, `routeReveal`, `routeSearch`, both key hooks |
| `InstanceCommands` | Cut, paste, move, rename, delete, `run_action` |

**Duplication**

- The two key hooks run identical routing blocks (1481-1486, 1815-1820).
- The folder-dialog handling in `open_project` and `save_project_as` is copied, with different error text.
- The `share` and `replaced` lambda pair is recreated three times.
- "Make a fresh dock the root" is written four times.
- The 0.12 to 0.5 split-fraction clamp is written three different ways.
- Floating-window size constants appear three times.

**Longest functions:** constructor 263, `start_mcp` 119, `ChooseDrop` 94, `open_floating` 75, `reset_layout` 74, `confirm_overwrite` 73, `restore_layout` 72, `routeUndo` 68.

**Worth keeping:** `OnUiThread`'s shared wait record with the `alive_` check; weak-pointer discipline for docks and panes; deferring dock removal out of the layout pass; `DockArrange` and `SavedLayout` as pure, tested functions.

### 4.7 Panels

**Files:** `IdeExplorer` (1,322 + 222), `PropertiesPanel` (1,129 + 89), `PropertySheet` (500 + 107), `InsertPopup`, `IdeConflicts`, `IdeConsole`, `ConsoleLog`, `CutSet`, `ClassFilter`. Total 5,862.

**Verdict.** Line-level quality is high. Cross-thread handoffs use explicit release and acquire, closures capture by value, and every ownership suspicion that was checked against JadeFX came out clean. The problems are architectural: the layout pass doubles as a polling loop.

**Main problems**

- **The explorer copies the world every frame.** Every layout pass copies each instance's id, child count, name, and class under a read lock and diffs by comparing string vectors (`IdeExplorer.cpp:781-783`, `948-959`). `DataModel::authored_revision()` exists for exactly this purpose, and no panel uses it.
- **Properties rebuilds its sheet every frame.** It reads every property of every selected instance through the Lua readers, and only then checks whether anything changed (`PropertiesPanel.cpp:398-432`, `PropertySheet.cpp:219-311`).
- **The console runs Lua from layout.** Command chunks execute inside `layoutChildren`, gated by a paint flag, with re-entry guarded by a flag that is set in three copy-pasted places.
- **God classes.** `IdeExplorer` has 44 methods, 40 data members, and 9 booleans encoding five overlapping state machines (rename, slow click, filter, reveal, insert), including a hand-written double-click detector. `PropertiesPanel::Impl` is a 681-line struct, and `PropertyKind` is dispatched in ten switch statements across two files, so a new property type touches about eleven places.
- **A string protocol.** Actions cross from the explorer to the layout as strings (`"Delete"`, `"Cut"`, `"Rename"`) matched on both sides with no compiler help.
- **Two ownership conventions.** `PropertiesPanel` guards every callback with weak pointers, while the other panes store a raw `this` in node callbacks. Both are safe today; pick one.
- **Linear row lookup.** `find_id` scans every row and is called on every click, context menu, and drop.

**Duplication**

- The undo-diff routine in `PropertiesPanel.cpp:58-82` and `IdeConsole.cpp:264-292` is identical apart from its signature.
- The console's command field re-implements the script editor's bracket pairing, Enter handling, and completion commit.
- Three ancestor-cycle checks use three different termination guards (`PropertySheet`, `CutSet`, `IdeLayout`).

**Longest functions:** `apply_edit` 120, `read_sheet` 93, `Impl::layout` 84, `make_view` 79. None is over 150 lines.

**Worth keeping:** `PropertySheet` as a pure, tested model layer; `PropertiesPanel`'s weak-pointer callback pattern; the explorer's snapshot diff; `cut_set`.

### 4.8 Script editor

**Files:** `IdeScriptEditor` (1,592 + 188), `ScriptPairs` (1,137), `IdeSearch` (699), `FindBar` (525), `LuauHighlight`, `TextSearch`, `ColorLiterals`, `ScriptMarks`, `TextUndoStack`, `TextWrap`, with headers. Total 6,147.

**Verdict.** The pure units (`TextSearch`, `ScriptMarks`, `LuauHighlight`, `ColorLiterals`, `TextWrap`, `ScriptPairs`) take `string_view`, return values, and have tests, and the cross-thread Source write is well designed. The trouble is in how they are driven: there is no incremental text model, and Source synchronization is spread thin. Two real bugs came out of that ([C9](#c9-stop-writes-old-closed-tab-text-over-newer-script-changes), [C10](#c10-the-color-picker-key-hook-can-outlive-its-editor)).

**Main problems**

- **Whole-document work on every keystroke.** JadeFX's `getText()` rebuilds the document from its paragraphs. One typed character causes four to six rebuilds, three UTF-32 decodes, two full highlighter passes (painting and the `Color3` swatch scan), a full find, and a walk of the DataModel that copies every script's Source for completion. Quotes, brackets, and Enter add a full pairing tokenizer pass.
- **Per-frame work for each open editor.** `layoutChildren` takes a 5 ms read lock, copies the Source, rebuilds the text three more times, and re-projects every diagnostic with linear scans. JadeFX lays out every frame.
- **A place snapshot every 50 ms.** While stopped, each flush that changes the text (`kSaveDelay`, line 24) adds a history waypoint and calls `capture_place` (line 992), which snapshots the whole place. Typing with short pauses, or dragging the color picker, does this continuously.
- **Three undo histories for one document.** `TextUndoStack` (never coalesced, uncapped, never freed), JadeFX's own history (unused), and `ChangeHistoryService` waypoints.
- **Source sync in eleven places.** Synchronization is spread across `IdeScriptEditor` and `IdeLayout` with three mechanisms (timed flush, generation-gated reapply, closed-tab replay), and no single place states which side wins when.
- **God class with a back door.** `IdeScriptEditor` has 32 private methods covering six concerns and shares its internals with a subclass defined in the `.cpp` through `friend` and a raw back pointer. It downcasts its own member six times.
- **Pairing is a tangle of special cases.** `ScriptPairs` spends 1,137 lines on quote, bracket, and Enter handling around a 228-line tokenizer (372-599) and a 118-line `enter_luau` (1018-1135) with four different exit paths.
- **Search copies its sibling and polls.** `IdeSearch` re-implements `FindBar`'s controls instead of composing them, rebuilds its query every frame, and re-collects every script's Source once a second while a pattern is present.

**Duplication**

- UTF-8 decoding is implemented ten times in `src/ide` with three different validation rules. See [5.1](#51-duplicated-helpers).
- Two Luau lexers (`ScriptPairs`, `LuauHighlight`) plus completion's, with three keyword tables and five identifier predicates that disagree about non-ASCII names, `const`, and `type`.
- `FindBar` and `IdeSearch` share copied chevron, replace-row, Tab-cycling, and key-constant code.
- Five `kLockWait` constants hold two different values.

**Longest functions:** `Scan::tokenize` 228, `enter_luau` 118, `TextSearch::find_all` 92, `FindBar::FindBar` 87.

**Worth keeping:** the commit epoch and acknowledgement design for editor writes; `TextSearch`; the `string_view` analysis units; `setProblems` comparing before touching the widget.

### 4.9 Rendering, MCP server, bridge, and preferences

**Files:** `runner/` (`GameView`, `Renderer`, `Runner`, `ShaderFile`, `ViewCapture`, `gl`), `McpServer`, `McpTools`, `bridge/`, `Preferences`, `PreferencesPanel`, `IdeTheme`, `ThemeLibrary`, `IdeResources`, `IdeIcons`. Total 6,419.

**Verdict.** Better than the size of `McpTools` suggests. There is no `new` or `delete`, every cross-thread wait is bounded and uses a shared wait record, file writes are atomic with a path-traversal guard, and the MCP protocol layer is solid. No Critical or High defects. The problems are one monolithic function, copied utilities, and GL ownership by convention.

**Main problems**

- **One 777-line function.** `add_engine_tools` (`McpTools.cpp:694-1470`) defines 21 tools as inline lambdas, with three different argument-clamping styles and 49 `throw` sites as the error channel.
- **MCP issues** listed in [3.1](#31-security-and-robustness): unauthenticated by default, casts before range checks, analysis pumped on the HTTP thread, and a slow shutdown during a call.
- **A heavy bridge.** The `anarchy-mcp` bridge constructs a full `Engine`, including the Lua runtime and analysis worker thread, only to list tool names and schemas, because tool metadata is welded to live lambdas.
- **GL ownership by convention.** See [3.4](#34-rendering).
- **A global macro layer.** `gl.hpp` `#define`s 37 `gl*` names to function pointers for every file that includes it, and its `#undef` guard block is incomplete.
- **Preferences.** `PreferencesPanel::build` is 212 lines. "Meta variable" is defined three times with two meanings (`PreferencesPanel.cpp:135`, `IdeTheme.cpp:73-75`, `ThemeLibrary.cpp:20-24`).
- **A placeholder icon.** The fallback class icon is `wat.gif` (`IdeIcons.cpp:52-55`).

**Duplication**

- `ExecutableDirectory`, 48 lines, is byte-identical in `ShaderFile.cpp:29-76` and `IdeResources.cpp:38-85`.
- Shader lookup and file reading re-implement `find_resource` and `read_file`.
- `McpServer.cpp:38-114` is a second JSON serializer beside `write_json`.
- `Lower` is written four times, `Trim` twice with different behavior, and the alert plumbing twice in `PreferencesPanel`.

**Longest functions:** `add_engine_tools` 777 (with a 95-line `playtest` lambda inside), `PreferencesPanel::build` 212, `StudioBridge::forward` 76.

**Worth keeping:** timeout-bounded, dangling-safe cross-thread waits; MCP's loopback bind, `Origin` allow-list, and exclusive port; atomic preference writes that preserve unknown keys; `Renderer::draw` saving and restoring GL state and clipping to its pane.

### 4.10 Tests

**Trees:** `tests/` (20 files, 9,214 lines, a hand-written harness, 7 executables) and `sandbox/` (5 files, 8,051 lines, 228 Catch2 test cases).

**Verdict.** The UI tests do something hard well: they drive real JadeFX scenes headlessly with a simulated clock, so double-click, rename, and focus timing is deterministic without sleeping. The weaknesses are operational and structural.

**Main problems**

- **Tests that never run.** `make test` never runs `shell-tests` or `explorer-tests` (47 tests between them), and CMake registers nothing with CTest.
- **Two frameworks with no layering reason.** Every file in `tests/` declares its own `int gFailures` and assertion helper, in four spellings, although Catch2 is already a dependency. Engine tests live in `tests/` (`LuaEngineTest`), while studio code is compiled into the Catch2 binary (`TextUndoStack`, `InputRouter`). The name `sandbox` hides that it is the engine's real acceptance suite.
- **Unhelpful failures.** Assertions print a label only, with no file, line, expected value, or actual value. 327 of them are compound `a && b && c` checks that cannot say which part failed.
- **Crashes instead of failures.** `McpTest` dereferences 95 JSON lookups without checking them. A missing member crashes the process and loses every later test's output and cleanup.
- **Shared mutable state.** `StudioLayoutTest` threads one `IdeLayout`, `Scene`, and `Engine` through five suites in other files that change it destructively, and the order matters ("Last, since it replaces the place").
- **Timing-dependent tests.** `FindReplaceTest` polls with 10 ms sleeps for up to 5 seconds. The Catch2 suite asserts a measured 57 to 63 Hz step rate over one real second, and wall-clock idle counts.
- **Global state left behind.** Tests register Lua classes and services and change the theme without restoring them, and one Catch2 file installs a contract handler that changes behavior for all five.
- **Leftover temp directories.** Cleanup runs after the assertions, so a failure leaves directories behind. Only two of seven temp-directory helpers are RAII.

**Coverage gaps.** Nothing in either tree names `InvalidationQueue`, `CompletionPopup`, `GameView`, `ShaderFile`, `Renderer`, `restore_place`, `task.defer` or `task.delay` scheduling, the bridge's stdio loop, or `UserInputService`'s posting side. `SavedLayout` is tested only indirectly. No test covers a failed save, a leftover `.tmp` file, or the instance cap.

**Duplicated helpers across the two trees:** `SimRole` 5 times, `add_script` 5, `ScriptRig` 3 (identical), temp directories 7, click-at-center 8, `rgb` 3, color compare 4, wait-for-worker 4.

**Worth keeping:** the simulated clock; fake hosts that record calls; registry-driven tests (every theme against every variable, every global against the datatypes); `sandbox/project_tests.cpp` as the model file, with an RAII temp directory, whole-tree diffing, `SECTION`s, and informative failure output.

---

## 5. Cross-cutting problems

### 5.1 Duplicated helpers

Each row was confirmed by comparing the two sides directly.

| Helper | Copies | Where | Status |
|---|---:|---|---|
| UTF-8 to UTF-32 decoder, 46 lines | 3 | `LuauComplete.cpp:94-139`, `LuauHighlight.cpp:63-108`, `ScriptPairs.cpp:20-65` | Identical |
| Other UTF-8 walkers | 7 | `ScriptMarks`, `TextUndoStack`, `TextWrap`, `TextSearch`, `IdeSearch`, `IdeScriptEditor`, `ColorLiterals` | Validation rules differ |
| `code_points` | 2 | `TextSearch.cpp:92`, `TextUndoStack.cpp:24` | Different algorithms that disagree on malformed input |
| `ExecutableDirectory`, 48 lines | 2 | `runner/ShaderFile.cpp:29-76`, `ide/IdeResources.cpp:38-85` | Identical |
| `fuzzy_component`, `sign_of`, `lerp_component` | 2 | `Vector2.cpp:16-37`, `Vector3.cpp:19-40` | Identical |
| Text undo diff | 2 | `PropertiesPanel.cpp:58-82`, `IdeConsole.cpp:264-292` | Identical body |
| `has_class` | 2 | `FindBar.cpp:43`, `IdeSearch.cpp:203` | Identical |
| `counted` | 2 | `IdeConflicts.cpp:138`, `IdeSearch.cpp:199` | Identical |
| `fit_utf8` | 2 | `ScriptRuntime.cpp:190`, `TableSnapshot.cpp:41` | Identical logic |
| `same_transform` | 2 | `DataModel.cpp:31`, `ChangeHistoryService.cpp:46` | Identical |
| `one_line` | 2 | `ScriptAnalysis.cpp:177`, `ScriptMarks.cpp:75` | Drifted: one also truncates |
| Luau keyword table | 3 | `LuauComplete.cpp:164`, `LuauComplete.cpp:175`, `LuauHighlight.cpp:22` | The third differs |
| Luau state setup | 3 | `LuaEngine.cpp`, `ScriptRuntime.cpp`, `LuaReflect.cpp` | Budgets differ |
| JSON serializer | 2 | `McpServer.cpp:38-114`, `PropertyBag.cpp` | Separate escapers |
| `read_file`, `write_file` | 2 | `Project.cpp:103-140`, `IdeResources.cpp:158-198` | One throws, one returns an error string |
| ASCII lowercase | 7 | `IdeExplorer`, `ClassFilter`, `PreferencesPanel`, `ThemeLibrary`, `TextSearch`, `McpTools`, `ColorLiterals` | Equivalent |

A small `ide/Utf8.hpp`, an `ide/Strings.hpp`, and one shared Luau lexer would remove most of this table.

### 5.2 Layering

- **Header cycles between packages.** The four engine libraries link each other in a documented cycle, but the header dependencies are the practical cost. `DataModel.hpp` includes `ChangeHistoryService`, `SelectionService`, and `UserInputService`; `types.hpp` includes `Color3`, `Vector2`, and `Vector3`; `ScriptRuntime.hpp` includes `RunService`. No package can be built or tested alone.
- **`ide` and `runner` include each other** through relative paths: `IdeLayout.hpp` includes `../runner/Runner.hpp`, and `GameView.hpp` includes `../ide/IdePane.hpp`.
- **Heavy interface headers.** `IRenderer.hpp` includes `SnapshotPump.hpp`, which includes `DataModel.hpp`, so the renderer interface compiles the entire world model.

### 5.3 Polling instead of change notification

The UI reads the world by polling from `layoutChildren`, which JadeFX runs every frame (120 frames per second by default). Each frame the explorer copies every name, Properties re-reads every property, every open script editor copies its Source and rebuilds its text, and Search rebuilds its query. The engine already offers `authored_revision()`, `SelectionService::revision()`, per-instance signals, and `ScriptAnalysis::diagnostics_changed()`, and the UI uses almost none of them. Refreshing on revision changes would make the idle cost constant and remove most of the 1 to 5 ms lock waits on the UI thread.

### 5.4 Failure handling

- **Capacity limits abort.** The event queue, command queue, instance slots, phase jobs, and connection slots all end in `contract_fail`, which aborts in the studio. There are 67 `contract_fail` call sites in `engine_core`.
- **Engine threads catch one exception type.** Anything other than `ContractViolation` terminates the process, including exceptions from paused edits and from `set_guid`.
- **UI actions time out silently.** Several paths take a 1 ms or 5 ms lock and return without feedback when it is busy.
- **Few swallowed exceptions.** Of the seven `catch (...)` blocks, six rethrow or record the error, and only `LuaReflect.cpp:312` swallows it. The problem is missing handlers, not swallowing ones.

### 5.5 Naming

Method definitions by package, counting only names that are clearly one style or the other:

| Package | Definitions | camelCase | snake_case |
|---|---:|---:|---:|
| `engine_core` | 432 | 21 | 288 |
| `engine_services` | 48 | 0 | 30 |
| `engine_instances` | 64 | 0 | 61 |
| `ide` | 508 | 197 | 186 |
| `runner` | 36 | 27 | 0 |

The `ide` package is split almost evenly, often inside one class: `IdeLayout` declares `attachFrame` and `flushFrame` beside `open_project_at` and `save_layout`, and `IdeConsole` and `IdeConflicts` do the same. Free functions in single files use PascalCase, camelCase, and snake_case.

### 5.6 Build and test infrastructure

- **No IDE library target.** Each test executable lists IDE sources by hand, so `IdeResources.cpp` compiles into seven targets and `IdePane`, `IdeIcons`, `IdeTheme`, `PropertySheet`, and `TextSearch` into four each. Source lists must be kept in sync manually.
- **Repeated warning settings.** The warning block is repeated for eight targets (`CMakeLists.txt:465-489`).
- **No CTest registration,** and `make test` omits two test binaries.

### 5.7 Compiler warnings

The core libraries, the studio, the MCP bridge, `engine-tests`, and `sandbox` were rebuilt at `/W4` with MSVC 2019. Project code produced nine warning sites:

| Warning | Sites |
|---|---|
| Shadowed local (C4456) | `Project.cpp:1432`, `LuauComplete.cpp:2233`, `tests/LuauCompleteTest.cpp:1103`, `tests/ScriptPairsTest.cpp:71` |
| Deprecated `getenv` or `strncpy` (C4996) | `IdeLayout.cpp:1304`, `1310`, `1401`; `bridge/main.cpp:78`; `ScriptRuntime.cpp:2292` |

A further 68 warnings came from Luau and cpp-httplib headers, which are not included as system headers, and two more from standard-library templates that a Catch2 comparison instantiates. Marking those include directories `SYSTEM` would leave only the project's own warnings.

---

## 6. What is working well

- **Fail-closed identity.** Instance ids carry a generation, script handles carry a world generation, and both are checked at every entry point.
- **The event queue's re-entrancy handling.** Index snapshots, epochs, and tombstones make connecting, disconnecting, and destroying during dispatch safe.
- **An enforced threading contract.** The rules are written in `DataModel.hpp`, checked at runtime, and backed by a ThreadSanitizer build.
- **Good overflow behavior where it exists.** `InvalidationQueue` drops and resyncs rather than aborting. It is the model for the other queues.
- **RAII throughout.** Almost no manual memory management, and small guard types wherever state must be restored.
- **Safe project files.** Atomic temp-and-rename writes and GUID-based file names.
- **Bounded cross-thread waits.** MCP calls, view capture, and editor writes all use shared wait records with timeouts, so a late completion never writes into freed memory.
- **Generated analysis definitions.** The Luau type definitions come from the live class registry, so the analyzer cannot drift from the runtime.
- **Pure, tested units.** `PropertySheet`, `TextSearch`, `DockArrange`, `SavedLayout`, `ScriptMarks`, `LuauHighlight`, and `ColorLiterals` take plain data and return plain data.
- **Deterministic UI tests.** A simulated clock and fake hosts make timing-sensitive UI behavior testable without sleeping, and `sandbox/project_tests.cpp` shows what the rest of the suite could look like.

---

## 7. Recommended order of work

### Phase 1: stop crashes and data loss

1. Release coroutine and callback registry refs ([C1](#c1-every-script-coroutine-is-kept-alive-until-stop), [C2](#c2-signal-callbacks-are-kept-alive-until-stop)).
2. Contain Lua failures and add last-resort handlers to both engine loops ([C3](#c3-running-out-of-lua-memory-terminates-the-studio)).
3. Make queue overflow recoverable ([C4](#c4-a-full-event-queue-aborts-the-process)).
4. Make `save_tree` safe to fail partway through ([C6](#c6-a-failed-save-can-leave-the-project-half-written-and-unloadable)).
5. Fix closed-tab buffer storage ([C9](#c9-stop-writes-old-closed-tab-text-over-newer-script-changes)).
6. Bound instance counts on read ([C5](#c5-an-oversized-project-aborts-the-studio)).
7. Apply the small fixes: C7, C8, C10, C11, `finish_recording` taking its id by value, and JSON comparison with `operator==`.

Add a regression test for each. Several of these (a failed write, the instance cap, queue overflow) have no test today.

### Phase 2: bound growth and make failures visible

- Cap undo history, console lines, and text undo stacks. Evict analysis modules for destroyed scripts. Erase undo stacks when a project closes.
- Replace silent lock timeouts with a retry or a status message.
- Generate a random MCP token per session and publish it in the registry entry with user-only permissions.
- Register every test binary with CTest and make `make test` run all of them.

### Phase 3: structure

- Split `IdeLayout` along the lines in [4.6](#46-studio-shell).
- Split `ScriptRuntime` into a coroutine scheduler, an output log, a VM factory, and bindings. Delete `LuaEngine` or build it on the same factory.
- Split `DataModel` into world storage, place snapshots, and history application, with one record type shared with `ChangeHistoryService`.
- Merge `Project`'s two reconciliation engines, and format numbers with `std::to_chars`.
- Drive the panels and editors from revision counters instead of per-frame polling.
- Introduce one Luau lexer and one UTF-8 header for `src/ide`, and move completion toward `Luau::autocomplete`.
- Replace the three datatype files with one Luau userdata template.
- Move the tests to one framework with shared helpers, and build `src/ide` as a static library so each source compiles once.

---

## 8. Appendix: metrics

### 8.1 Largest source files

| File | Lines |
|---|---:|
| `src/ide/LuauComplete.cpp` | 3,718 |
| `src/ide/IdeLayout.cpp` | 3,491 |
| `src/engine_core/ScriptRuntime.cpp` | 2,520 |
| `src/engine_core/DataModel.cpp` | 2,331 |
| `src/engine_core/Project.cpp` | 2,283 |
| `src/engine_core/ScriptAnalysis.cpp` | 1,822 |
| `src/ide/IdeScriptEditor.cpp` | 1,592 |
| `src/ide/McpTools.cpp` | 1,472 |
| `src/ide/IdeExplorer.cpp` | 1,322 |
| `src/ide/ScriptPairs.cpp` | 1,137 |
| `src/ide/PropertiesPanel.cpp` | 1,129 |
| `src/ide/CompletionPopup.cpp` | 1,014 |

### 8.2 Longest functions

| Function | Location | Lines |
|---|---|---:|
| `add_engine_tools` | `McpTools.cpp:694` | 777 |
| `build_docs` | `LuaApi.cpp:414` | 426 |
| `IdeLayout::IdeLayout` | `IdeLayout.cpp:488` | 263 |
| `Project::outside_changes` | `Project.cpp:1297` | 230 |
| `Scan::tokenize` | `ScriptPairs.cpp:372` | 228 |
| `PreferencesPanel::build` | `PreferencesPanel.cpp:210` | 212 |
| `Project::compare_disk` | `Project.cpp:1559` | 203 |
| `Tokenize` | `LuauComplete.cpp:211` | 185 |
| `analyze_job` | `ScriptAnalysis.cpp:874` | 182 |
| `Project::save_tree` | `Project.cpp:2106` | 176 |
| `Resolver::parse_stmt` | `LuauComplete.cpp:1711` | 175 |
| `Project::apply_changes` | `Project.cpp:1865` | 156 |
| `Resolver::member_of` | `LuauComplete.cpp:895` | 133 |
| `DescribeSymbol` | `LuauComplete.cpp:2798` | 133 |
| `IdeLayout::start_mcp` | `IdeLayout.cpp:1303` | 119 |
| `ScriptRuntime::require_module` | `ScriptRuntime.cpp:1483` | 119 |
| `enter_luau` | `ScriptPairs.cpp:1018` | 118 |
| `key_code_from_glfw` | `UserInputService.cpp:20` | 113 |

### 8.3 Largest classes

| Class | Location | Size |
|---|---|---|
| `Resolver` | `LuauComplete.cpp:750-2685` | 1,936 lines, about 60 methods |
| `DataModel` | `DataModel.hpp`, `DataModel.cpp` | 158 declared member functions, 34-field `State` |
| `IdeLayout` | `IdeLayout.hpp`, `IdeLayout.cpp` | 99 member functions, 66 data members |
| `PropertiesPanel::Impl` | `PropertiesPanel.cpp:348` | 681 lines |
| `InsertList` | `InsertPopup.cpp` | 454 lines |
| `CompletionListPopup` | `CompletionPopup.cpp` | 425 lines |
| JSON `Parser` | `PropertyBag.cpp:42` | 310 lines |
| `PlanReader` | `Project.cpp:294` | 245 lines |
| `IdeExplorer` | `IdeExplorer.hpp`, `IdeExplorer.cpp` | 44 methods, 40 data members, 9 booleans |

### 8.4 Code-smell counts

| Measure | Count | Notes |
|---|---:|---|
| Non-placement `new` | 1 | `TaskScheduler.cpp:293`, owned by `unique_ptr`; the instance pools also use a paired `::operator new` |
| `delete` expressions | 0 | |
| `reinterpret_cast` | 34 | 17 in `ScriptRuntime.cpp` (C functions stored as `void*`), 6 in `TaskScheduler.cpp` (fiber stacks) |
| `const_cast` | 8 | Mostly the const-overload idiom |
| `catch (...)` | 7 | Six rethrow or record; `LuaReflect.cpp:312` swallows |
| `friend` declarations | 23 | 6 in `DataModel.hpp`, 6 in `Events.hpp` |
| `dynamic_cast` in `DataModel.cpp` | 15 | Including one per simulated object per physics substep |
| `contract_fail` sites in `engine_core` | 67 | Each aborts the studio |
| TODO, FIXME, HACK | 0 | |

### 8.5 Most-changed files

The files that change most are the same ones that carry the most responsibilities.

| File | Commits touching it (of 165) |
|---|---:|
| `src/ide/IdeLayout.cpp` | 55 |
| `sandbox/tests.cpp` | 40 |
| `src/ide/IdeLayout.hpp` | 38 |
| `src/engine_core/ScriptRuntime.cpp` | 30 |
| `src/engine_core/DataModel.cpp` | 29 |
| `src/engine_core/DataModel.hpp` | 25 |
| `src/ide/IdeScriptEditor.cpp` | 23 |
| `tests/LuauCompleteTest.cpp` | 22 |
| `tests/LuaEngineTest.cpp` | 20 |
| `src/ide/IdeExplorer.cpp` | 20 |
