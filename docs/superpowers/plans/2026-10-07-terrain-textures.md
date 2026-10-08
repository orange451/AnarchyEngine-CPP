# Terrain Textures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax. Controllers: dispatch every implementer and reviewer in the foreground (`run_in_background: false`); per task run only the relevant Catch2 tags, the full suite only after Task 5, after Task 8, and in the final review; mid-tier reviewers for Tasks 1, 3, 8, the top model for 2, 4, 5, 6, 7, 9 and the final review.

**Goal:** Terrain drawn with real textured Materials: height-based blending of up to 4 materials per pixel, triplanar projection, anti-tiling, two packed texture arrays per Terrain built in the background, a per-Terrain `TextureSize`, and a global `Lighting.TerrainQuality` for weak hardware.

**Branch:** `terrain` after the LOD branch (`terrain-lod`) is merged into it. Work in a worktree branched from it.

**Spec:** `docs/superpowers/specs/2026-10-07-terrain-textures-design.md`. This plan adds five implementation decisions (Task 1 writes them into the spec as "Implementation notes"):
1. **A geometry shader joins a triangle's materials.** Each vertex carries up to 4 (Id, weight) pairs; a triangle's three vertices may carry different sets. `terrain.geom` passes, to every fragment, the triangle's three vertices' pairs (flat) and its barycentric coordinates; the fragment shader sums `bary_i × weight_i(id)` per Id and keeps the top 4. GL 3.3 core and macOS 4.1 have geometry shaders. This replaces `orient_triangle_materials` (LOD) and the flat provoking-vertex material.
2. **The look table becomes `GL_RGBA32F`, 256 × 4:** row 0 color (sRGB values), row 1 (metalness, roughness, reflectivity, 1), row 2 (layer index, TextureScale, BlendSharpness, HeightStrength), row 3 reserved (0). `TerrainLook::texels` becomes `std::array<float, 256 * 4 * 4>`.
3. **Arrays are `GL_RGBA8`, not sRGB formats**; color stays decoded in the shader (`toLinear`), as the rest of the engine does. CPU mips filter color in linear space and re-encode, so mips do not darken.
4. **Layer building runs in `engine_core`** on its own worker thread. `engine_core` gets stb_image's include directory and one `STB_IMAGE_STATIC` implementation in `LayerBuilder.cpp` (the only includer), as `TextureCache.cpp` has its own in `studio`.
5. **A `RENDER_PASS("Terrain")` GPU timer** wraps the terrain runs (they are the last opaque runs), so budgets measure terrain alone.

## Global Constraints

- New: `Enum.TextureSize { Small = 0, Medium = 1, Large = 2, Max = 3 }` → 256, 512, 1024, 2048 px. `Terrain.TextureSize`, default Large. `Lighting.TerrainQuality: Enum.EffectQuality`, default High.
- Material gains `HeightTexture: Texture?` (6th reference; `ReferenceAsset::kMaxReferences` 5 → 6), `TextureScale` (8, must be finite and > 0: "TextureScale must be greater than 0"), `BlendSharpness` (0.5, slider 0–1; values outside are refused: "BlendSharpness must be from 0 to 1"), `HeightStrength` (1, ≥ 0: "HeightStrength must not be negative"). Saved, undoable, documented.
- Height chain per Material: HeightTexture; else integrated from NormalTexture (high-passed); else DiffuseTexture luminance; else 0.5 flat.
- Arrays per Terrain: A = color RGB + height A; B = normal XY (RG, tangent space, Z reconstructed) + roughness B + metalness A; full mips; layer 0 = (white, 0.5, flat normal, roughness 1, metalness 1). One layer per distinct Material used. Memory = layers × size² × 4 × 2 × 4/3.
- Build off the simulation and render threads; render thread uploads at most 4 layers per frame into a new pair, swapping when complete; old arrays draw until then; nothing re-meshes.
- Vertex weights: up to 4 (Id, weight) per vertex, sum 1, from the 8 cell corners within one VoxelSize of the surface; collision triangles keep one Id.
- Shader: materials under 0.01 skipped; triplanar sharpening power 4 (fixed constant), projections under 0.05 skipped; height score `weight + height × HeightStrength`, blend band `(1 − BlendSharpness) × 0.5`; whiteout normals; anti-tiling noise at 1/40 of TextureScale; LOD level ≥ 2 uses ≤ 2 projections and mip bias +1. Quality Low/Medium/High per the spec's table.
- Budgets (Release, 1920×1080, Terrain Demo island): terrain pass at High ≤ 2.0× and at Low ≤ 1.2× the flat-color terrain pass (same scene, same camera, flat look).
- Never `git stash`; warning-free /W4; MSVC 14.23 quirks; commit messages a plain imperative sentence ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A Material with no textures next to a textured one** blends without black or seams. Task 6 (TX-R2).
2. **A texture file replaced on disk while the studio runs** rebuilds only that layer and the terrain never shows missing texture. Task 5 (TT4).
3. **Three materials meeting inside one triangle** blend smoothly (the geometry-shader merge). Task 6 (TX-R3).
4. **Low quality on a scene with many materials** never exceeds the Low budget. Task 9.
5. **Changing TextureSize repeatedly and quickly**: builds are superseded, not queued forever; memory returns. Task 5 (TT5).

---

### Task 1: Enums and properties

**Files:** `src/engine_datatypes/Enum.{hpp,cpp}` (`texture_size_enum()`, `enum class TextureSize`), `src/engine_instances/AssetInstances.{hpp,cpp}` (Material: 6th reference HeightTexture, three numbers with their range checks — Material's existing `set_number` does not range-check, so add one that does), `src/engine_instances/Terrain.{hpp,cpp}` (TextureSize, `lua_saved_enum`), `src/engine_services/Lighting.{hpp,cpp}` (TerrainQuality, copying Antialiasing's shape), `src/engine_core/SnapshotPump.{hpp,cpp}` (`VisualLighting::terrain_quality = 2` + static_assert), `src/runner/Renderer.hpp` (`SceneLighting::terrainQuality`), `src/runner/GameView.cpp` (carry it), `src/engine_core/LuaApi.cpp` (docs), READMEs; the spec's implementation notes.

- [ ] Tests (`[terrain][textures]`, and the Material/Lighting tags where they live): TE-E1 `Enum.TextureSize` items and values; TE-P1 Material's new properties default, save, undo, and refuse bad values with the exact messages; HeightTexture accepts a Texture and nil; TE-P2 Terrain.TextureSize defaults to Large, saves, undoes; TE-P3 Lighting.TerrainQuality defaults to High and reaches `SceneLighting::terrainQuality` through the snapshot.
- [ ] Fail, implement, pass, commit "Add terrain texture settings to Material, Terrain, and Lighting".

### Task 2: Blend weights in meshes

**Files:** create `src/engine_core/terrain/BlendWeights.{hpp,cpp}`: `struct BlendIds { std::array<std::uint8_t,4> ids; std::array<float,4> weights; }; BlendIds blend_weights(const float distance[8], const std::uint8_t id[8], float voxel_size);` (corners with `|d| ≤ voxel_size` count one each for their Id; no qualifying corner → the lowest-distance corner's Id at 1; normalize; top 4 by weight, ties by lower Id; sorted by descending weight; unused slots id 0 weight 0). Use it in `SurfaceNets.cpp build_vertices` (write `rgba[0..3]` and `t[0..3]`) and in `LodBuilder.cpp reshade_vertices` via a new `VoxelSampler::blend(Vec3 p)` returning the same `BlendIds` from the 8 corners of p's cell. Keep `orient_triangle_materials` until Task 6 (the current shader reads `rgba[0]`, which is still the dominant Id).

- [ ] Tests: BW1 a vertex deep in one material: one Id at weight 1; BW2 a vertex on a two-material border: both Ids, weights sum to 1 within 1e-6, each > 0; BW3 never more than 4 Ids, sorted, sum 1; BW4 a Surface Nets ball half grass half rock: every vertex's weights sum to 1 and vertices near the paint boundary carry both; BW5 a LOD node's re-shaded vertices match `blend_weights` evaluated from the full-resolution field at their positions; all existing `[terrain]`/`[lod]` tests pass (physics Ids unchanged).
- [ ] Commit "Carry up to four blended materials per terrain vertex".

### Task 3: Height from normal maps and brightness

**Files:** create `src/engine_core/terrain/HeightDerive.{hpp,cpp}`: `std::vector<float> height_from_normals(const std::uint8_t* rgba, int w, int h);` (decode XY in [-1,1], slopes `-x/z`, `-y/z` with z clamped ≥ 0.2; integrate by solving the Poisson equation with tileable FFT-free multigrid or 200 Gauss-Seidel iterations on a pyramid; subtract a 1/8-width box-blurred copy (high-pass); normalize to 0..1) and `std::vector<float> height_from_luminance(const std::uint8_t* rgba, int w, int h);` (Rec.709 luminance of sRGB-decoded color, high-pass the same way, normalize).

- [ ] Tests: HD1 a synthetic tileable height field (sum of sines) → its normal map → `height_from_normals` correlates ≥ 0.9 with the original; HD2 a normal map with a constant tilt yields no ramp (high-pass works: the result's low-frequency energy is < 5% of the original's); HD3 luminance of a checkerboard gives two levels; HD4 a 1×1 and a flat-normal map return all 0.5 without NaN.
- [ ] Commit "Derive terrain blend heights from normal maps or brightness".

### Task 4: Building one layer

**Files:** create `src/engine_core/terrain/LayerBuilder.{hpp,cpp}` (the only stb_image includer in engine_core; CMake: `target_include_directories(engine_core SYSTEM PRIVATE "${stb_image_SOURCE_DIR}")`).

```cpp
struct LayerSources {                 // resolved file paths; empty = none
    std::filesystem::path diffuse, normal, roughness, metalness, height;
};
struct LayerBytes {                   // for one layer at `size`
    int size = 0;
    std::vector<std::vector<std::uint8_t>> a_mips, b_mips;   // RGBA8, level 0 first, down to 1x1
    std::string warning;              // a file that could not be read: that map falls back
};
LayerBytes build_layer(const LayerSources& sources, int size);   // any thread
std::size_t layer_bytes(int size);   // both arrays with mips
```

Resize each map to `size` with a separable filter (box when shrinking by an integer factor, bilinear otherwise; never sharpen); height per the chain (Task 3); pack A (color RGB as in the file, height A) and B (normal XY as in the file, roughness R channel, metalness R channel; missing maps take the layer-0 values); mips: average 2×2 with color in linear space (decode pow 2.2, average, re-encode), normal XY renormalized, others plain average.

- [ ] Tests: LBR1 a 300×200 diffuse resizes to 256²/512² with every mip present and the right sizes; LBR2 packing round-trips each channel within 1/255 at level 0; LBR3 a missing normal map gives a flat normal and a luminance height; a missing file sets `warning` and falls back; LBR4 mip color stays at linear-space average (a black/white checker's 1×1 mip encodes to ≈ 186, not 128); LBR5 `layer_bytes(1024)` equals 1024²×4×2×4/3 within one mip's rounding.
- [ ] Commit "Build packed terrain texture layers off the main threads".

### Task 5: TerrainTextures: which layers, caching, background builds, publishing

**Files:** create `src/engine_core/TerrainTextures.{hpp,cpp}` (sim side, owned by the Engine next to `TerrainWorld`, its own worker thread); modify `TerrainWorld.{hpp,cpp}` (`TerrainLook` becomes float RGBA 256×4 per decision 2; `rebuild_look` fills row 2 with the layer index from `TerrainTextures` and the Material's three numbers; change detection includes them and the texture references); `TerrainView` gains `std::shared_ptr<const TerrainTextureSet> textures;`; Engine wiring; the Configure Terrain tab header (`IdeTerrainEditor::show_header`) shows "· N MB textures" from `TerrainTextures::memory_bytes(terrain)` and refreshes when the set's revision changes (not only on tree/voxel changes).

```cpp
struct TerrainTextureSet {               // immutable once published
    int size = 0;
    std::vector<std::shared_ptr<const LayerBytes>> layers;   // [0] is the untextured layer
    std::uint64_t revision = 0;
};
class TerrainTextures {
public:
    void update(DataModel& game);   // SimulationThread: per Terrain in Workspace, the distinct Materials its
                                    // TerrainMaterials use -> layer order (stable: keep existing layers' order,
                                    // append new); queue builds for layers whose (sources, stamps, size) changed;
                                    // publish a new set when all its layers are built
    int layer_of(InstanceId terrain, InstanceId material) const;   // 0 when none
    std::shared_ptr<const TerrainTextureSet> published(InstanceId terrain) const;
    std::size_t memory_bytes(InstanceId terrain) const;
    void wait_idle();   // tests
};
```

Cache: `std::map<key, std::weak_ptr<const LayerBytes>>` keyed by (the five resolved paths, their `last_write_time`s, size) so Terrains share CPU layers; a newer build request for the same Terrain supersedes a queued older one. A file's stamp is checked at most once a second (as `TextureCache` does).

- [ ] Tests: TT1 a Terrain with two textured Materials publishes a set with 3 layers (0 + 2) at its TextureSize, and the look's row 2 points each Id at its layer; TT2 two Terrains using the same Material at the same size share the `LayerBytes` pointer; TT3 changing a Material's TextureScale changes the look only (same set revision); TT4 touching one texture file rebuilds only its layer (the other layer pointer is unchanged) and the old set stays published until the new one is complete; TT5 toggling TextureSize Small↔Max ten times quickly ends with one published set at the last size, no more than two builds in flight, and old sets released (weak cache entries expire); TT6 `memory_bytes` matches the formula.
- [ ] Full suite checkpoint. Commit "Build and publish each Terrain's texture arrays in the background".

### Task 6: Drawing textured terrain

**Files:** `src/runner/gl.{hpp,cpp}` (add `glTexSubImage3D`; `glTexParameterf` if missing; the anisotropy constant `0x84FE` used only when `GL_EXT_texture_filter_anisotropic`/`GL_ARB_texture_filter_anisotropic` is present — add `glGetStringi` to query it, max 8×); `src/runner/Renderer.{hpp,cpp}` (per-Terrain array pairs built incrementally ≤ 4 layers per frame and swapped when complete; the RGBA32F look; samplers `uSurfaceA`/`uSurfaceB` on `kUnitNormalMap`/`kUnitRoughnessMap` and `uNoise` on `kUnitMetalnessMap`; a small tileable value-noise texture made once; quality and LOD-level uniforms; the "Terrain" GPU pass), `src/runner/TerrainDraws.cpp` (hand the set to the renderer), shaders `resources/shaders/pipeline/terrain.vert`, new `terrain.geom`, `terrain.frag`; `Renderer::initialize` builds the terrain program with the geometry shader (extend `buildProgram`/`LinkProgram` for an optional geometry stage); remove `orient_triangle_materials` from `LodBuilder` (and its test, replaced by TX-R3).

Shader outline:
- `terrain.vert`: outputs Terrain-local position and normal (for triplanar, so textures stick to a moved Terrain), view-space position and normal, `ids` (uvec4 from `aColor * 255`), `weights` (`aTangent`).
- `terrain.geom`: per triangle emits 3 vertices with `flat` `uvec4 tIds[3]`, `vec4 tWeights[3]`, and smooth `vec3 bary`.
- `terrain.frag`: dither first (unchanged); merge the up-to-12 pairs into up to 4 materials by `Σ bary_i × w_i`; per material fetch look rows; triplanar weights `pow(abs(n), 4)` normalized, skip < 0.05 (Low: dominant axis only; Medium or LOD ≥ 2: top two); sample A and B per (material, projection) at `localPos / TextureScale` (+ noise offset when anti-tiling is on); height blend (off at Low: plain weights); whiteout normal reorientation per projection, then blend; multiply by Material color/roughness/metalness; write the G-buffer as today. No textures yet (no set) → flat colors from the look, as now.

- [ ] Tests: GPU-free parts in sandbox (the pair-merging rule as a C++ mirror function `merge_triangle_materials` with unit tests TX1–TX2); render checks in `tests/SceneRenderCheck.cpp` `--terrain-shots` (generated procedural textures written to the test's temp root — a stone pattern, a grass pattern, sand noise — so the check needs no downloads): TX-R1 a two-material border shows no staircase (sample a row across the border: luminance changes over ≥ 6 px, no step patterns), TX-R2 an untextured Material beside a textured one has no black pixels, TX-R3 three materials meeting inside one triangle produce a smooth blend (no flat-shaded triangle visible), TX-R4 a vertical cliff's texture is not stretched (texel density on the cliff within 25% of the floor's), TX-R5 changing TextureSize mid-run never draws a frame without terrain; existing terrain/LOD shots unchanged except materials now textured where the scene has textures.
- [ ] Commit "Draw terrain with height-blended triplanar textures".

### Task 7: Quality levels, far LOD shading, and anti-tiling

**Files:** `terrain.frag`, `Renderer.cpp` (uniforms), SceneRenderCheck.

- [ ] Tests: TX-R6 Low, Medium, High each render the textured scene with no missing-texture black and visibly fewer projections at Low (count projection usage via a debug uniform that outputs the number of active projections into a test-only target, or compare against reference crops); TX-R7 anti-tiling: a far hillside of one material has no strong periodic luminance peak at its repeat distance with anti-tiling on (FFT of a row, peak at the repeat frequency < 2× the median), and does have one with it off (proves the check works); TX-R8 LOD ≥ 2 nodes use ≤ 2 projections.
- [ ] Commit "Shade terrain by quality level, more cheaply far away, without visible tiling".

### Task 8: Live changes and the editor

**Files:** `TerrainWorld.cpp`/`TerrainTextures.cpp` (change detection for every Material property and texture reference), `IdeTerrainEditor.cpp` (memory figure refresh), README entries (Material, Terrain, Lighting).

- [ ] Tests: TL-T1 setting a Material's Color/TextureScale/BlendSharpness/HeightStrength changes the next look within one update and re-meshes nothing (mesh revision counts unchanged); TL-T2 assigning a NormalTexture rebuilds that layer only; TL-T3 the tab's header text includes "MB textures" and updates after a build completes (terrain-editor-tests).
- [ ] Full suite checkpoint. Commit "Update terrain textures live and show their memory in the Configure Terrain tab".

### Task 9: Budgets, screenshots, and the textured demo

- [ ] **GPU budgets** in SceneRenderCheck (`--terrain-budget`, Release, 1920×1080): a large textured island scene timed with the "Terrain" GPU pass over 60 frames after warm-up, at High, Medium, Low, and with a flat look; print ms; `Expect` High ≤ 2.0× flat and Low ≤ 1.2× flat. If the budget fails, profile (sample counts per pixel via the debug uniform), fix in Tasks 6–7's code with a regression check, re-measure.
- [ ] **Sample textures:** check freepbr.com's license terms (record them in the report). If they allow redistribution in a sample project, download 4 sets (a grass, a rock, a sand or dirt, a snow) into a scratch copy of `C:\Users\Andrew\Documents\Anarchy Engine Projects\Terrain Demo` (never the original), as Texture and Material assets; otherwise use the procedural test textures and say so. Never commit downloaded textures to the engine repo.
- [ ] **Screenshots** (`tex-*.png` in the scratchpad's screenshots folder, Read each): the textured Terrain Demo overview; a close-up of a height-blended grass/rock border; the cliff; a far hillside; the same view at Low / Medium / High side by side; the Configure Terrain tab showing the memory figure. Drive the Release studio over MCP per the user's notes.
- [ ] Commit any fixes; final whole-branch review.
