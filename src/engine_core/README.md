# engine_core

Simulation and render are two threads. The DataModel is live memory. The GPU reads a `VisualSnapshot` only.

`DataModel` is the tree every other package builds on. The instance classes are in `engine_instances`, `Game` and the services are in `engine_services`, and value types such as `Vector3` are in `engine_datatypes`. `GameObject` inherits `DataModel` and is the instance that carries a transform, a velocity, and a Prefab. A plain instance has hierarchy and signals, and none of those fields.

The root holds only the scene services, which cannot be moved, renamed, or destroyed. `parent_error`, `rename_error`, and `destroy_error` say why a change is refused; every caller that takes input from a script or a person asks them first, and `set_parent`, `set_name`, and `destroy` fail the contract on anything they refuse.

`Containment` is one table of what may go where, by class name, so a project read can check a plan before it makes anything; `parent_error` asks the same rules. `InstanceRef` is a reference to another instance, held by its GUID rather than its id, so a load, Stop, or undo can set one before its target exists. A saved property whose type is a registered class name with `?`, such as `Texture?`, is a reference: it takes `nil` or a live instance of that class, reads the live instance or `nil` when there is none, and its JSON is the GUID or `null`.

A class's properties are listed once, in its Lua registration (`LuaApi.hpp`). One registered with `lua_saved_property` needs nothing else from the core: `DataModel` saves it, loads it, gives it a default, puts it back at Stop, and undoes it, all through its registered read and write. Its setter validates the value, stores it, and calls `note_property_change`, which records undo and fires `Changed` with the property's name. The `Field` enum is only for the properties the core itself tracks, such as Transform and Parent.

Prepare holds the DataModel write lock: RenderStepped, PreRender, then the dirty copy, then path-C overrides. Perform/Present runs with the lock released and reads the front snapshot. PostRender runs after Present, still on the render thread, without that lock. If the lock is not acquired within 2 ms, Present repeats the previous snapshot and PostRender still runs.

Roblox fires `RenderStepped`, then `PreRender`, before the frame is drawn. Both are that locked window here, RenderStepped first. A write from either one is path B and records `PreRenderDataModel`. Job priority inside a phase is unchanged: a larger value runs first. Roblox does not publish a PostRender script event; the task scheduler's render section ends at the asynchronous render. PostRender is the boundary after this frame's snapshot has been presented. A DataModel write there is path D, and a snapshot override is rejected. It is outside the 2 ms budget and does not drain signals.

| Path | Who | What it changes |
| --- | --- | --- |
| A | SimulationThread phases | DataModel. Snapshot on the next Prepare. |
| B | RenderStepped or PreRender, visual-only or `ForceSimWrite` | DataModel, and this frame's snapshot. |
| C | `SnapshotPump::override_visual` | This frame's snapshot only. |
| D | RenderThread outside that window, including Perform, Present, and PostRender | Contract failure (abort, or the test handler). |
| E | Any other thread | Command queue, applied on the next sim step. |

`print(transform)` reads the GameObject. Pixels read the snapshot. A path-C write changes pixels and not the print. A path-B write changes both. A write after the copy changes the print and not this frame's pixels.

Physics may substep inside one sim frame (240 Hz). Display rate does not set that count. Pools are reserved at startup; the step does not allocate.

## ThreadSanitizer

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENGINE_CORE_TSAN=ON
cmake --build build-tsan --target sandbox --parallel
./build-tsan/sandbox "[T3]"
```

`[T1]` through `[T11]` are the acceptance tests. `./build/sandbox` runs them without the sanitizer.
