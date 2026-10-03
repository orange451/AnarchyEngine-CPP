# Dragger Phases 5–6: Selection and Run-State Events, and the Move Tool

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Selection.SelectionChanged`, `RunService.Started`, and `RunService.Stopped` for scripts, and a built-in Move tool plugin that puts handles on the first selected PVInstance.

**Architecture:** The three are host signals with no arguments. A new SignalUd kind, `kSignalHost`, resolves them through `ScriptRuntime::host_signal(tag)`: Selection's lives in ScriptRuntime, Started and Stopped in RunService. The selection can change on any thread, so `step_tools` (the simulation thread's tool step, in edit and play) fires `SelectionChanged` when the selection's revision moved since the last step, and `Started` or `Stopped` when `simulation_running()` changed since the last step: after Play has captured the place, after Stop has restored it. `MoveTool.luau` connects all three to one update that sets its Core Dragger's `Adornee`.

**Spec:** `docs/superpowers/specs/2026-10-03-dragger-design.md` sections 7 and 8.

## Global Constraints

- Build `cmake --build build --parallel`; tests `./build/sandbox`, `(cd build && ctest -C Release)`.
- The events carry no arguments. A handler reads `Selection:Get()` or `RunService:IsRunning()`.
- `SelectionChanged` fires once per change seen by a step, never for a set to the same list.

## Review Focus

- Several selection changes within one step: one event, and `Get()` sees the last list.
- Play and Stop within one step (no step in between): no event, since the state did not change between steps. Accepted.
- MoveTool when its Dragger is destroyed by someone else (the command line): the next update makes a new one.

---

### Task 1: The events
**Files:** `src/engine_core/LuaApi.hpp` (`LuaField::host_signal`, `lua_host_signal`), `src/engine_services/RunService.{hpp,cpp}` (Started, Stopped), `src/engine_services/SelectionService.cpp` (declare SelectionChanged), `src/engine_core/ScriptBindings.{hpp,cpp}` (`kSignalHost`), `src/engine_core/ScriptRuntime.{hpp,cpp}` (`selection_changed_`, `host_signal`, firing in `step_tools`), `sandbox/move_tool_tests.cpp` (new).
- [ ] EV1–EV3 failing; implement; pass; commit.

### Task 2: The Move tool
**Files:** `resources/plugins/MoveTool.luau`, `src/ide/PluginLoader.hpp` (`kBuiltinPlugins`), `sandbox/move_tool_tests.cpp`.
- [ ] MT1–MT3 failing; implement; pass; commit.
