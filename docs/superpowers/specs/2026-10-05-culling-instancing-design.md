# Frustum culling and GameObject instancing

2026-10-05 · Builds I9 from `docs/research/2026-09-30-legacy-renderer-port.md`, less its LOD selection, which this design leaves room for. Builds on shadows (`2026-10-01-shadows-design.md`), whose `WorldBounds`, `MakeFrustum` and `SphereInFrustum` it reuses.

## Goal

A place with many objects draws faster in two ways:

- **Culling.** A mesh whose bounds are outside the camera's view is not drawn by the geometry or transparency pass.
- **Instancing.** GameObjects that share a Prefab draw each of its Models in one instanced call, not one call per GameObject.

Today `GameView::collectMeshes` makes one `MeshDraw` per GameObject and Model, and `Renderer::geometryPass` sets every material uniform and issues one `glDrawElements` for each, whether on screen or not.

Efficiency is a design rule:

- one vertex shader path, with no instanced and non-instanced variants;
- the per-vertex `transpose(inverse(mat3(uModel)))` moves to the CPU, once per instance;
- the frame's instance data is uploaded once per buffer, with no stall on the GPU;
- a frame that redraws no shadow map pays nothing for shadow instancing;
- no per-frame allocation once buffers have grown.

Out of scope, with room left for each: LOD selection, a box test after the sphere test, merging batches across Prefabs that share a mesh and Material, profiler overlay counters, and caching spheres of objects that do not move.

## Frame outline

1. `GameView::collectMeshes` builds `MeshDraw`s as now, plus `slot` and `tint`.
2. `FindVisible` (Visibility) works out each draw's world sphere and keeps the visible ones.
3. `BuildBatches` (DrawBatches) sorts the visible opaque draws into runs and writes every instance's data.
4. The geometry pass uploads the instances once and draws one instanced call per run.
5. The transparency pass draws each visible transparent draw as a run of 1, back to front.
6. The shadow pass sees every draw, not the culled list (an object off screen can shadow one on it), reuses the spheres, and draws each tile's casters in instanced runs.

Nothing after the G-buffer changes: lighting, sky, reflections, bloom, tone map and FXAA read the same buffers. The snapshot and `SceneFeed` are unchanged.

## MeshDraw

Two fields are added:

| Field | Type | Meaning |
| --- | --- | --- |
| `slot` | `std::uint32_t` | Which Prefab Model the draw came from: its entry in `GameView::prefabMeshes_`, counted from 1 across all Prefabs. 0 never batches. |
| `tint` | `float[3]` | The GameObject's Color. `color` is now the Material's Color alone; before, it was the two multiplied. |

Draws with the same nonzero `slot` share their mesh, every texture and every Material value. Per GameObject, `GameView` sets only `model`, `owner`, `tint` and `transparency`.

## Visibility

`src/runner/Visibility.hpp/.cpp`, with no GL, tested in `sandbox/visibility_tests.cpp`. It is in `studio_core`, which the sandbox links, so like `ShadowCaster` it reads a plain `DrawItem` the renderer fills from each `MeshDraw`, not the `MeshDraw` itself (`GpuMesh::valid` is only in `studio`).

```cpp
// What visibility and batching read from one MeshDraw. Pointers are into that draw.
struct DrawItem {
    const engine_core::Matrix4* model = nullptr;
    const float* boundsMin = nullptr;  // the mesh's local box
    const float* boundsMax = nullptr;
    const float* tint = nullptr;       // the GameObject's Color, as the Color3 holds it; null is white
    float transparency = 0.f;
    std::uint32_t slot = 0;
    bool drawable = false;             // an uploaded mesh, and transparency below 1
};

struct VisibleDraw {
    int index = 0;             // into the frame's MeshDraws
    float screenRadius = 0.f;  // the sphere's projected radius in pixels; +inf with the camera inside it
    std::uint8_t lod = 0;      // 0 until LOD selection fills it from screenRadius
};

struct VisibilityResult {
    std::vector<Sphere> spheres;           // one per MeshDraw, world space
    std::vector<VisibleDraw> opaque;       // in MeshDraw order
    std::vector<VisibleDraw> transparent;  // in MeshDraw order
    int culled = 0;                        // drawable, but outside the view
};

// out is reused frame to frame. With cull false every drawable draw is visible.
void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out);
```

A draw is drawable when its mesh is non-null and valid and its transparency is below 1, as the geometry pass checks today. Its sphere is `WorldBounds(model, bounds_min, bounds_max)`, and it is visible when `SphereInFrustum` passes against `MakeFrustum` of the camera's view-projection, made once per call. Transparency above 0 puts it in `transparent`, otherwise `opaque`. A draw that is not drawable gets a zero sphere and is in neither list.

`screenRadius` is `radius * paneHeight / (2 * distance * tan(fovY / 2))`, with `distance` from the camera to the center, and +inf when the distance is no more than the radius.

Rules:

- Culling is per Model draw, not per GameObject: part of a Prefab can be culled while the rest draws.
- The sphere test is conservative. A draw just past a corner of the view may still draw; a visible one is never dropped.
- Beyond `kSceneFar` is culled, as the GPU clips it anyway.
- A Scale of 0, or an empty box, is a radius-0 sphere: culled unless its center is in view.
- Play-session geometry is culled by the box of its latest upload (`GpuMesh::keep_bounds`).
- Skinned meshes (X2, not built yet) can leave their baked box when posed; that spec must pad the bounds.
- Screen-space reflections and the billboard depth probe read only what is on screen, so culling does not change them.

It is profiled as the CPU scope `"Visibility"`.

`Renderer::setCulling(bool)`, true by default, turns culling off for comparisons. `Renderer::stats()` returns the last frame's `RenderStats`:

| Field | Meaning |
| --- | --- |
| `draws` | MeshDraws given |
| `visible` | opaque plus transparent visible |
| `culled` | drawable but outside the view |
| `runs` | runs the geometry pass drew |
| `instancedCalls` | `draw_instanced` calls in every pass, shadows included |

## Instancing on the GPU

### Loader and mesh

`gl.hpp`/`gl.cpp` load `glDrawElementsInstanced` and `glVertexAttribDivisor`, both core in 3.3.

`amesh.hpp` adds:

```cpp
inline constexpr unsigned kAttribInstanceModel = 7;    // mat4: 7–10
inline constexpr unsigned kAttribInstanceNormal = 11;  // mat3: 11–13
inline constexpr unsigned kAttribInstanceTint = 14;    // vec3

// With the mesh and its instance attributes bound. Throws std::out_of_range
// for a missing LOD, as draw does. A no-op before an upload and under AE_MESH_NO_GL.
void draw_instanced(int lod, int count) const;
```

Slot 15 stays free; GL guarantees 16. `upload` does not touch slots 7–14, so other users of `GpuMesh` (the demos, `AmeshGlCheck`) are unchanged.

### InstanceBuffer

`src/runner/InstanceBuffer.hpp/.cpp`.

```cpp
struct InstanceData {  // 112 bytes; slots 7–14 in order
    float model[16];   // column-major
    float normal[9];   // inverse transpose of model's 3x3, column-major
    float tint[3];     // linear: the GameObject's Color to the power 2.2, as surface.glsl's toLinear
};

class InstanceBuffer {
public:
    // glBufferData with the data, GL_STREAM_DRAW: new storage each upload, so
    // draws still reading the last upload never stall the CPU.
    void upload(const InstanceData* data, int count);
    // With a mesh's VAO bound: points slots 7–14 at instance first, divisor 1, enabled.
    void attach(int first) const;
    void destroy();
};
```

The renderer owns one and `ShadowRenderer` owns another. `attach` enables, points and sets the divisor of 8 slots per run (24 cheap state calls), since GL 3.3 and macOS's 4.1 have no base instance. `InstanceData` is declared in `DrawBatches.hpp`, which has no GL.

### Shaders

`geometry.vert` drops `uModel` for `layout(location = 7) in mat4 aModel`, `layout(location = 11) in mat3 aNormalMatrix` and `layout(location = 14) in vec3 aTint`. The normal is `mat3(uView) * (aNormalMatrix * aNormal)`, and the tint is folded into the vertex color, `vColor = aColor * vec4(aTint, 1.0)`, so `surface.glsl` and the fragment shaders do not change.

`shadow.vert` drops `uModel` for `layout(location = 7) in mat4 aModel`.

`light.vert` keeps `uModel`: it draws light volumes, not meshes.

## Batches

`src/runner/DrawBatches.hpp/.cpp`, with no GL, tested in `sandbox/draw_batches_tests.cpp`.

```cpp
struct DrawRun {
    int first = 0;      // into the frame's InstanceData
    int count = 0;
    int draw = 0;       // the run's first MeshDraw: its mesh and Material
    std::uint8_t lod = 0;
    bool mirrored = false;
};

struct DrawBatches {
    std::vector<DrawRun> runs;            // opaque runs first, then one run of 1 per transparent draw
    std::vector<InstanceData> instances;
    int opaqueRuns = 0;
};

// Reuses out's vectors frame to frame.
void BuildBatches(const DrawItem* items, const VisibilityResult& visible, const engine_core::Matrix4& view,
                  DrawBatches& out);
```

- The key is `slot << 32 | lod << 1 | mirrored`, where mirrored is a negative 3x3 determinant (what `CullBackFaces` tests today). Slot 0 gets a key of its own per draw, so it is always a run of 1.
- Opaque draws sort by key, then view depth near to far, so early depth testing rejects more within a run.
- The normal matrix of a 3x3 that does not invert (a Scale of 0) is all zeros, not NaN.
- Transparent draws sort back to front by the same rule `transparencyPass` sorts them by today, which moves here; each run of 1 still binds its own transparency.

### Geometry and transparency passes

```
instances.upload(all instances)
for each opaque run:
    bindMaterial(draws[run.draw])            // no uModel
    glCullFace(run.mirrored ? GL_FRONT : GL_BACK)
    mesh->bind(); instances.attach(run.first); mesh->draw_instanced(run.lod, run.count)
```

The transparency pass does the same for the runs after `opaqueRuns`. Material uniforms and texture binds happen once per run instead of once per draw.

## Shadow casters

The shadow pass draws with face culling off, and the planner gives each tile and cascade its casters already culled and without the light's own meshes. So its runs key on the mesh alone.

- `ShadowRenderer::draw` takes the spheres from `VisibilityResult` instead of calling `WorldBounds`.
- For each tile drawn this frame, its casters sort by mesh address into runs. All the tiles' runs go into one array, uploaded once before the first tile draws; the sun's cascades do the same in `drawSun`. Cached tiles and unchanged cascades add nothing.
- `drawCasters` binds, attaches and calls `draw_instanced(0, count)` per run, in place of a `glUniformMatrix4fv` and a draw per caster.
- Caster fingerprints read `model` and `revision` as now, so when a map redraws does not change.
- Shadow instances use the same `InstanceData`; the shader reads only the matrix.

## LOD later

The LOD spec fills `VisibleDraw::lod` from `screenRadius`, which visibility already computes. Batches already key on LOD and `draw_instanced` already takes one, so no other part changes.

## Build order

Each phase leaves the renderer working and is committed on its own.

| Phase | Work | Check |
| --- | --- | --- |
| 1. Culling | `Visibility` and its tests; both passes draw only visible draws; shadows reuse the spheres; `setCulling`, `stats()` | Sandbox edge cases; a `ViewCapture` with culling on matches one with it off |
| 2. Instancing plumbing | Loader, `draw_instanced`, `InstanceBuffer`, shaders; every draw a run of 1 | First in a standalone GL demo, since a GL error quits the studio; then captures before and after within 1/255 per channel (the normal matrix now rounds on the CPU) |
| 3. Batching | `slot` and `tint`; `DrawBatches` and its tests; runs in the geometry pass | Key tests: mirrored, slot 0 and different LODs never merge; same captures |
| 4. Shadow instancing | As above | Captures with a sun and a spot shadow |
| 5. Stress test | The place and numbers below | Recorded here |

## Testing and measurement

The stress place is built by a Lua script in a scratch copy of a project, never a real one: about 10,000 GameObjects over 3 Prefabs on a grid, about 50 one-off Prefabs, and a sun with shadows. It is measured from three cameras:

- **Overview**, all of it in view: measures instancing. Mesh draw calls fall from about 10,000 to the number of runs (dozens), with the Geometry pass's GPU time and the Scene View's CPU time falling with them.
- **Corner close-up**, most of it out of view: measures culling. The pose looks diagonally across the whole grid. `culled` is above 80% of `draws`, and the Visibility scope stays well under 1 ms.
- **Ground close-up**, looking down from near ground level: exercises culling more than Corner does, with 74% of draws culled.

Numbers come from the per-pass GPU timers and the profiler's CPU scopes, with the profiler's GPU "Each pass" mode set by hand in the studio. Before and after measurements are written into this spec when it ships.

## Results

Measured on 2026-10-05 on the user's Mac laptop with the stress place (`scripts/stress_place.lua`) saved as the StressTest project, GPU per-pass timing set by hand in the studio.

| Pose | Measure | Before | After |
|---|---|---|---|
| Overview (0,120,260)→(0,0,0) | mesh draw calls | 10,088 (one per draw) | 79 runs |
| Overview | visible / culled | 10,088 / 0 | 9,145 / 943 |
| Overview | GPU Geometry | 32.0 | 8.0 |
| Overview | CPU Geometry pass | 34.1 | 0.74 |
| Overview | CPU Scene View | 45.2 | 17.4 |
| Overview | CPU 3D scene | 43.9 | 15.8 |
| Overview | CPU Visibility | — | 1.75 |
| Corner (-205,6,-205)→(-190,0,-190) | visible / culled | 10,088 / 0 | 10,038 / 50 (pose looks diagonally across the whole grid) |
| Corner | mesh draw calls | 10,088 | 29 runs |
| Corner | GPU Geometry | 31.4 | 9.2 |
| Corner | CPU Scene View | 44.4 | 20.0 |
| Corner | CPU Visibility | — | 2.0 |
| Ground (0,3,0)→(0,1,50) (added: Corner does not exercise culling) | visible / culled | 10,088 / 0 | 2,609 / 7,479 (74% culled) |
| Ground | mesh draw calls | 10,088 | 7 runs |
| Ground | GPU Geometry | ~32 (as every pose before) | 9.8 |
| Ground | CPU Scene View | ~45 | 21.2 |
| Ground | CPU Visibility | — | 2.1 |

Observations:

- Targets met: draw calls fell from ~10k to dozens; Geometry CPU 34 → <1 ms; GPU Geometry 32 → 8–10 ms.
- Target missed: CPU Visibility is ~1.75–2.1 ms for 10,088 draws, not "well under 1 ms" (one sphere + 6 planes + ScreenRadius sqrt/tan per draw). Candidates: cache spheres of unmoved objects (spec "out of scope, with room left"), skip ScreenRadius until LOD needs it.
- Culling barely moves GPU Geometry in the Ground pose (9.8 ms) because the near teapots fill the screen: the pass is now fill-bound, not draw-bound.
- New top CPU cost: "Floor grid" on the Render draw thread, 9.7–11.6 ms (6.9 ms before). Likely a GPU wait landing in that scope rather than the grid's own work; outside this plan.
- Frame time is ~33–36 ms with the scene at ~16–21 ms: frames quantize to vsync (2 × 16.6) because the frame still exceeds one refresh.
