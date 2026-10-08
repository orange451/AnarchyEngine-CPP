# Terrain Textures Design

2026-10-07 · sub-project 3 of the terrain roadmap (`2026-10-06-terrain-roadmap-design.md`). Builds on terrain core (`2026-10-06-terrain-core-design.md`), the terrain surface (Surface Nets meshes, the terrain shader, the per-Terrain look table), and terrain LOD (`2026-10-06-terrain-lod-design.md`: LOD nodes re-shaded from full-resolution voxels).

## Goal

Terrain drawn with real, textured materials that blend naturally: grass gives way to rock with rock poking through between grass tufts and sand filling the cracks, cliffs and overhangs never stretch, and a hillside of one texture does not show its repeats. The user only assigns ordinary Materials: texture sizes, packing, and height maps are handled by the engine. It stays efficient, with a quality setting that keeps weak hardware smooth.

Look settings belong to **Material**, not TerrainMaterial, because a future precision geometry editor (in the style of TrenchBroom) will apply Materials directly to faces of raw geometry, and must see the same scale and blending.

## Decisions

### What users see

| Question | Decision |
| --- | --- |
| New Material properties | `HeightTexture: Texture?` (none); `TextureScale: number` (8, units per texture repeat when applied in world space, > 0); `BlendSharpness: number` (0.5, 0–1); `HeightStrength: number` (1, ≥ 0). Saved, undoable, range-checked like Material's other properties. Meshes ignore the last three for now. |
| New Terrain property | `TextureSize: Enum.TextureSize`, default Large. |
| New enum | `Enum.TextureSize { Small = 0, Medium = 1, Large = 2, Max = 3 }`: layer sizes 256², 512², 1024², 2048². Named for what it controls, so other texture systems can reuse it. |
| Global shading quality | `Lighting.TerrainQuality: Enum.EffectQuality` (existing enum: Low, Medium, High), default High, settable by scripts at runtime, as the other effect qualities are. |
| TerrainMaterial | Unchanged: Id, Name, Material. No per-TerrainMaterial overrides; "Rock" and "Rock (large)" are two Materials sharing texture files, which costs nothing extra. |
| Material Color, Roughness, Metalness | Still apply: Color multiplies the color texture; Roughness and Metalness multiply their maps, as on meshes. |
| A Material with no textures | Draws as its flat Color, roughness, and metalness through the same path (layer 0). Textured and untextured materials blend together. |
| Height maps | Never required. Each Material's height comes from the first that exists: HeightTexture; else height integrated from its Normal texture (low frequencies removed, so it has no drift); else the Diffuse texture's brightness; else flat (then blending is a plain soft blend). |
| Live changes | Changing a Material's TextureScale, BlendSharpness, HeightStrength, Color, Roughness, or Metalness updates the look at once (look table only). Changing a texture, or a Terrain's TextureSize, rebuilds that Terrain's arrays in the background; the old arrays keep drawing until the new ones are fully uploaded. Nothing is re-meshed. |
| Configure Terrain tab | The header shows texture memory beside the counter: "5 / 255 materials · 55 MB textures". |

### Texture arrays

| Question | Decision |
| --- | --- |
| Arrays | Two `GL_TEXTURE_2D_ARRAY`s per Terrain, at its TextureSize. **Surface A:** color RGB (sRGB) + height A (linear). **Surface B:** tangent-space normal X, Y (RG; Z reconstructed) + roughness (B) + metalness (A). Full mip chains. |
| Layers | Layer 0 is the untextured layer (white color, height 0.5, flat normal, roughness 1, metalness 1, to be multiplied by the Material's values). Each distinct Material a Terrain's TerrainMaterials use gets one layer; unused Materials get none. GL 3.3 guarantees at least 256 layers. |
| Look table | The per-Terrain look texture grows to carry, per material Id: the layer index, TextureScale, BlendSharpness, HeightStrength, Color, Roughness, Metalness, Reflectivity. Id 0 and unassigned Ids use layer 0 with Material defaults. |
| Building a layer | On a background thread, never the simulation or render thread: load the Material's texture files; resize to the layer size with a good filter (never inventing detail: smaller sources are filtered up smoothly); derive height if needed; pack into A and B; compute every mip on the CPU. |
| Uploading | The render thread uploads finished layer bytes, a few layers per frame, into a new array pair, and switches to it when complete. |
| Caching | Built layers are cached by (texture files, their stamps, layer size). Only changed layers rebuild. Two Terrains using the same Material at the same size share the CPU-side layer; GPU arrays stay per Terrain. |
| Memory | Layers × size² × 4 bytes × 2 arrays × 4/3 (mips). About 11 MB per Material at Large. |
| More maps later | Emissive, ambient occlusion, or subsurface maps need a third array (Surface C): A and B are full. It would be read only for Materials that have those maps. |
| Compression | Not now. GPU compression (BC7) would cut memory about 4× but needs a texture compressor in the build and slower array builds. A later step; nothing here blocks it. |

### Blend weights in the mesh

| Question | Decision |
| --- | --- |
| Per vertex | Up to 4 material Ids (vertex color bytes) with weights (tangent channel), summing to 1. From the 8 voxel corners of the vertex's cell: each corner within one VoxelSize of the surface counts once for its Id; counts are normalized; the top 4 are kept and renormalized. A vertex deep inside one material has one Id at weight 1. |
| LOD nodes | Re-shading from full-resolution voxels uses the same rule at each vertex's position. |
| Border triangles | A triangle whose three vertices carry different material sets gets three vertices of its own, each carrying the triangle's merged set (top 4 Ids by summed weight) and that corner's weights for it; triangles inside one material keep shared vertices. Done at mesh-build time on the workers, for chunks and LOD nodes. No geometry shader, so the terrain shader runs on Metal, WebGL/WebGPU, and OpenGL ES 3.0 too. About 10–20% more render vertices. |
| Physics | Unchanged: each collision triangle keeps one material Id, for raycasts and friction. |
| Effect | Weights vary smoothly across a border; with the height blend this replaces today's sawtooth edges. |

### The shader

| Question | Decision |
| --- | --- |
| Materials | Up to 4 per pixel from the interpolated vertex weights; those under 0.01 are skipped. |
| Projections | Triplanar weights from the surface normal, sharpened by a fixed shader constant (power 4, not a setting: lower ghosts doubled texture onto slopes and keeps extra projections active, higher shows a seam on rounded surfaces) so most surfaces use one or two projections; a projection under 5% is skipped. Coordinates: world position ÷ the Material's TextureScale. |
| Height blend | Each material's score is `weight + height × HeightStrength`; scores within a band of `(1 − BlendSharpness) × 0.5` of the highest blend smoothly, the rest drop out. |
| Normals | Each projection's normal is reoriented onto its axis ("whiteout" blend), then projections and materials are blended by their final weights. |
| Output | The same G-buffer as today: lighting, shadows, ambient occlusion, reflections, and bloom apply unchanged. The LOD dither stays first. |
| Anti-tiling | A low-frequency value noise (one read of a small tiling noise texture, at about 1/40 of the texture scale) offsets each material's coordinates and gently varies its brightness, so repeats do not line up across a hillside. |
| Far LOD nodes (level ≥ 2) | Two projections at most, and a mip bias of +1. |
| Arrays not ready | Flat colors from the look table, as terrain draws today. |

**Quality levels** (`Lighting.TerrainQuality`):

| | High (default) | Medium | Low |
| --- | --- | --- | --- |
| Projections | up to 3 | up to 2 | 1 (dominant axis) |
| Height blend | on | on | off (soft blend by weight) |
| Normal maps | all nodes | levels 0–1 only | off on levels ≥ 2 |
| Anti-tiling | on | on | off |
| Typical texture reads per pixel | 4–8 | 3–5 | 2–3 |

### Out of scope

GPU texture compression, a third array for emissive and other maps, per-material custom shaders, virtual texturing for far distances, parallax or displacement, Material texture settings on meshes (they keep ignoring TextureScale and blending).

## Architecture

| Unit | What it does | Depends on |
| --- | --- | --- |
| `terrain/BlendWeights` | The per-vertex Ids-and-weights rule, shared by Surface Nets and LOD re-shading | `VoxelChunk` |
| `terrain/HeightDerive` | Height from a normal map (Poisson integration, high-pass) or from brightness. Pure | nothing |
| `terrain/LayerBuilder` | One Material's layer at a size: load, resize, height, pack, mips. Pure apart from file reads | stb_image, `HeightDerive` |
| `TerrainTextures` (sim side, beside `TerrainWorld`) | Per Terrain: which Materials need layers, the layer cache, background builds, the published array set (immutable, with a revision) | `LayerBuilder`, a worker thread |
| Renderer | Per-Terrain array pairs, incremental upload, the bigger look table, the new shader, quality uniforms | the published sets |
| Material, Terrain, Lighting | The new properties and enums | engine registry |
| Configure Terrain tab | The memory figure | `TerrainTextures` |

## Testing

- **Blend weights:** a vertex on a two-material border carries both Ids with weights summing to 1; deep inside one material, one Id at weight 1; never more than 4 Ids.
- **Height derivation:** height integrated from the normal map of a known height field correlates ≥ 0.9 with it and has no drift; the brightness and flat fallbacks work.
- **Layer building:** any source size resizes to the layer size; mips are complete; packing round-trips each channel within 1/255; changing one Material's texture rebuilds only its layer.
- **Memory figure:** matches the formula above.
- **Properties and enums:** saved, undoable, range-checked; `TextureSize` and `TerrainQuality` round-trip.
- **Render checks** (with screenshots): no single-material staircase across a two-material border; no stretching on a vertical cliff; no strong periodic luminance peak at the texture's repeat on a far hillside; Low, Medium, and High all render correctly with no missing-texture black; changing TextureSize swaps arrays without a frame of missing terrain.
- **GPU-time budgets** (the Terrain Demo island, 1920 × 1080, the existing GPU timer): the terrain geometry pass at High costs at most 2× flat-color terrain; at Low at most 1.2×.
- **Screenshots for the user:** the Terrain Demo re-textured with sample textures from freepbr.com (license checked first; textures kept in the sample project, not the engine repo): grass, rock, sand, snow; a close-up of a height-blended border; a cliff; a far hillside; the three quality levels side by side.
