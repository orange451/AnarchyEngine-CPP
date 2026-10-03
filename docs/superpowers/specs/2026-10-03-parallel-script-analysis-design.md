# Parallel Whole-Place Script Analysis Design

2026-10-03 · Replaces the open-scripts-only scope of `35d529f` (Analyze only open scripts and the modules they require). Every Script and ModuleScript in the place is checked at all times, on several threads, and a change rechecks only the scripts it can affect. This is the first of two projects; the second is a Problems pane that lists the results, specced separately.

## Goal

The studio knows every error and warning in every script, whether or not an editor or any pane is showing it. The cost of knowing stays small:

- A source edit rechecks that script and the scripts that require it, transitively, and nothing else.
- A tree change (add, destroy, rename, reparent, reorder) rechecks only the scripts that reached the part of the tree that changed.
- A property change rechecks nothing.
- A batch of checks (place load, a widely required module edited) runs on all but one core.

Measured before this change, in the Release build: a 1000-line script takes about 31–38 ms to check (106–113 ms from edit to result, of which 75 ms is the debounce), and 20 such scripts take 680 ms on the one worker thread. Today any tree change rechecks every analyzed script.

## Decisions

| Question | Decision |
| --- | --- |
| Scope | Always every script. `AnalysisScope::Open`, `watch`/`unwatch`, `active_locked`, `drop_inactive_locked`, and `to_schedule` are removed, with the `set_scope(Open)` line in `IdeLayout.cpp`. |
| What rechecks a script | Its own source; any module it requires, transitively (Luau's `markDirty` already propagates to dependents); a tree change at an instance its check reached. |
| Parallelism | Luau's own: `Frontend::queueModuleCheck` plus `checkQueuedModules` with an `executeTasks` that runs on our thread pool. One shared module cache; Luau orders checks by require. |
| Pool size | `max(1, hardware_concurrency - 1)`. Normal priority. |
| Editor requests | A second, separate Frontend (the editor checker) with its own thread answers `luau_facts` / `luau_facts_later` for completion, hover, and the console. Typing never waits behind a place batch. |
| Instance types | One stable `ExternType` per instance for the life of the place, updated in place between batches. Never rebuilt on a tree change. |
| Reached set | After each module is checked, its expression types are scanned for instance types (`InstanceTag`). The instances found are the script's reached set. |
| Play | Tree changes during a playtest are ignored, as now. A batch running when play starts finishes against its pre-play snapshot. |
| Player | AnarchyPlayer turns analysis off at startup. Nothing there reads diagnostics. |
| Publishing | Still only `pump()`, on the gameplay thread or the UI thread. Results are handed over per module as each finishes, through the progress callback, so a large batch fills in gradually. `diagnostics_changed` keeps firing per script. |

## Architecture

### 1. Two checkers

`WorkerEnv` splits into:

- **Place checker.** Owns the Frontend whose cache holds every script in the place, the place types (section 3), and the reached sets. Driven by one coordinator thread and the pool.
- **Editor checker.** Owns a Frontend used only for `luau_facts` requests, served by its own thread exactly as the single worker serves them today (lanes, replacement, `ready`). Its cache holds the modules the asked buffers require. When the place checker marks a module dirty because its Source changed, it also tells the editor checker to mark that module dirty. A buffer's unsaved text never reaches the place checker, which keeps the existing guarantee that an unsaved edit never reaches another script's diagnostics.

Both rebuild on a registry revision change, as `WorkerEnv` does now.

### 2. Batches

The coordinator loops:

1. Wait until something is dirty and its debounce (75 ms, unchanged) has passed.
2. Capture one `WorldSnap`.
3. Parse and lint each dirty script on the pool. These stages read only the snapshot, the frozen global scope, and the `untyped` module, so they run in parallel without locks. A parse failure or `--!nocheck` ends that script's pipeline here, as now.
4. Diff the snapshot against the last one and update the place types (section 3). Collect the scripts the tree diff affects (section 4) and add them to the dirty set.
5. `markDirty` each dirty module, `queueModuleCheck` them, and `checkQueuedModules` with the pool as `executeTasks`. The progress callback hands each finished module's diagnostics, requires, and reached set to `results` for `pump()`.
6. Repeat.

Changes that arrive during a batch wait for the next one. The existing generation check drops a result whose script changed again after its job was taken. A batch whose scripts are mostly superseded is cancelled through `FrontendCancellationToken`.

### 3. Place types updated in place

`PlaceTypes` lives with the place checker for as long as the place does. Between batches:

- A new instance gets a new `ExternType` tagged with its id.
- An instance whose name, parent, or sibling order changed: its old parent's and new parent's child props are recomputed, and its own `Parent` prop is rewritten. The types keep their identity.
- A destroyed instance's type is detached (no child props, no `Parent`) and kept, because cached modules may still hold it.

No mutation happens while a batch is checking. This is what lets cached modules and fresh checks agree: a module checked before a rename and a script checked after it see the same type for `workspace.Door`, so no spurious type mismatch appears.

### 4. Reached sets and the tree diff

A module's reached set is every instance whose `InstanceTag`-ed type appears in its expression types. It covers locals, parameters, `script.Parent`, and the `FindFirstChild` / `WaitForChild` / `GetService` magic, because it reads what the type checker inferred rather than the code text.

The diff of two snapshots yields:

- Parents whose child list changed (an add, destroy, rename, reparent, or reorder under them).
- Instances that were renamed, reparented, or destroyed.

A script is affected when its reached set contains either kind. Affected scripts are marked dirty, and Luau's dependents propagation adds whatever requires them. Added scripts are dirty. A destroyed script's module is cleared, its results dropped (as `remove` does now), and its requirers marked dirty.

Consequence, accepted: a script that touches `workspace` is rechecked whenever a direct child of `workspace` is added, renamed, or removed. Changes deeper in the tree reach only the scripts that reached that folder.

Today the checker passes `retainFullTypeGraphs = false`, which drops expression types after the check. The scan needs them until it has run. Either the place checker retains them and clears each module's `astTypes` / `astExprTypes` itself after the scan, or the scan runs in a per-module hook before they are dropped. Step one of the implementation proves one of these works under `checkQueuedModules` (see Order of work).

### 5. Thread safety of our hooks

- `WorkerEnv::self` is removed. `prepareModuleScope` and `FindChildMagic::infer` take the script from the module name (`instance_of_module`), which `FindChildMagic` already prefers.
- `WorkerEnv::world` and `PlaceTypes` are read-only during a batch and change only between batches (section 3).
- `SourceFileResolver` and any module-name ↔ instance maps the hooks consult are guarded by a mutex, or made read-only for the batch.
- `global_functions` and the frozen global types are read-only after `init`.

### 6. Snapshots

`WorldSnap` gets an id → node index, replacing the linear `find`, which the hooks call constantly and which made lookups quadratic in place size. `NodeSnap::source` becomes a `std::shared_ptr<const std::string>` reused from the previous snapshot when the instance's source is unchanged, so a capture is one tree walk and copies only edited sources.

### 7. Callers

| Caller | Change |
| --- | --- |
| `IdeLayout.cpp` | Drop `set_scope(Open)`. |
| `IdeScriptEditor` | Drop `watch` / `unwatch`. Squiggles unchanged: compare `analyzed_source` with the buffer. |
| `LuauComplete` / `IdeConsole` | Unchanged API; requests now land on the editor checker. |
| `McpTools` `CheckScripts` | Drop the `Watching` guard. Keep the wait-for-`settled` loop. |
| AnarchyPlayer | `set_enabled(false)` at startup. |
| `DataModel::note_tree_changed` | Unchanged signal; the coordinator diffs instead of `invalidate_all`. |

`invalidate_all` remains for a registry rebuild and for re-enabling analysis.

## Failure handling

- An exception or Luau internal error while checking one module gives that script one `Analysis` diagnostic ("could not be checked: …"). The rest of the batch continues.
- Analysis turned off or the checker destroyed mid-batch: the batch is cancelled, the pool joined, published results cleared, as turning analysis off does now.
- A registry revision change rebuilds both checkers and the place types, then rechecks everything once.

## Testing

In `sandbox/analysis_tests.cpp`, Catch2 with `ScriptRig`, numbered from A34. A14 and A15 (open scope) are removed. A test-only per-script check counter lets tests assert exactly what was rechecked.

1. **Parallel equals serial.** A place of about 30 scripts with modules and require chains gives identical diagnostics with a pool of 1 and a pool of N.
2. **Stable instance types.** Module M returns `workspace.Door`; script S requires M and passes the result where a `Part` is expected. Rename an unrelated part: S has no new diagnostic.
3. **Recheck rules.**
   - Editing a module rechecks its requirers transitively and nothing else.
   - Adding `Door` under Workspace clears the unknown-member error in a script that reached `workspace.Door` directly, through a local, and through a function parameter.
   - Renaming a part inside a folder rechecks only the scripts that reached that folder.
   - A property change rechecks nothing.
   - Adding a script checks only it. Destroying one drops its results and rechecks its requirers.
   - Undo and redo of a rename behave as the rename does.
4. **Editor checker isolation.** A completion request is answered while a large batch is running. An unsaved buffer never appears in place diagnostics (existing test kept).
5. **Lifecycle.** Disable mid-batch, destroy mid-batch, registry rebuild.
6. **ThreadSanitizer.** The analysis tests run clean in `build-tsan` (`ENGINE_CORE_TSAN`).
7. **Timing.** A hidden `[.perf]` test prints the 1000-line single-script and 20 × 1000-line place timings, for before and after comparison.

## Order of work

1. **Spike, kept only if it holds:** on a small place, `checkQueuedModules` on a pool with our hooks, and reading expression types for the reached set after each module, run clean under TSAN. If expression types cannot be read under parallel checking, stop and revisit section 4 before building on it.
2. Snapshot index and shared sources.
3. Editor checker split.
4. Persistent place types.
5. Coordinator, batches, and the pool; remove Open scope.
6. Reached sets and the tree diff.
7. Callers, player, tests, perf comparison.

## Out of scope

- The Problems pane (next spec).
- Persisting results across studio sessions.
- Skipping requirers whose required module's exported types did not change.
- Runtime errors from the Output window.
