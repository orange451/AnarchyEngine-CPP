# engine_core

Simulation and render are two threads. The DataModel is live memory. The GPU reads a `VisualSnapshot` only.

`GameObject` inherits `DataModel` and is the instance that carries transform, color, size, and velocity. A plain instance has hierarchy and signals, and none of those fields.

Prepare holds the DataModel write lock: PreRender, then the dirty copy, then path-C overrides. Perform/Present runs with the lock released and reads the front snapshot. If the lock is not acquired within 2 ms, Present repeats the previous snapshot.

| Path | Who | What it changes |
| --- | --- | --- |
| A | SimulationThread phases | DataModel. Snapshot on the next Prepare. |
| B | PreRender, visual-only or `ForceSimWrite` | DataModel, and this frame's snapshot. |
| C | `SnapshotPump::override_visual` | This frame's snapshot only. |
| D | RenderThread outside PreRender | Contract failure (abort, or the test handler). |
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
