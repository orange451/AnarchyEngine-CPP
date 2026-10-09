# Terrain: parked work

2026-10-08. What is left on terrain after LOD (1d) and textured materials (3) were merged to main. Nothing here blocks use; each item is something seen or measured, not a guess.

## Textured materials (sub-project 3)

| Item | Notes |
| --- | --- |
| Faint discoloration at very shallow view angles | The main band (normal maps ignoring the surface slope) is fixed in 47b0c62. What remains is faint. An unfinished edit that weights each projection's normal by `abs(n)` instead of the sharpened projection weights is saved as `grazing-normals-wip.patch` in the session scratchpad. Also worth trying: a small minimum on N·V in the lighting path. |
| Distance detail ramp and Toksvig roughness | Committed as WIP in 490860c and not verified on screen. It replaced the per-LOD-level shading step with a smooth ramp over distance. Check that it looks right, or drop it. |
| Task 9 not finished | The GPU budget check is started (WIP, ae54006) but not run. There is no textured demo with real textures (freepbr license not checked yet) and no final screenshots. |
| No final whole-branch review | Tasks 5–8 had no per-task review either (speed rules). |
| Tests | PropertiesTest not run after the new Material rows. HeightDerive has no non-square tests. No concurrent `build_layer` test. The render checks TX-R* are pixel heuristics and assume no MSAA. Anisotropy and layer-clamp paths are only exercised indirectly. |
| Small inefficiencies | Rapid changes across layers can rebuild a layer twice. `resolve_sources` runs per layer per tick. The rough/metal resize computes 4 channels. |
| Emissive and other maps | Need a third texture array (Surface C); see the textures spec. |

## LOD (1d)

| Item | Notes |
| --- | --- |
| First build of a 4,096-chunk island is 8.3 s, against a 5 s budget | 7.9 s of it is full-detail Surface Nets meshing. Raise the budget or speed up the mesher. TL1 (1.5 s) also fails on this machine. |
| Wrong-material triangles | About 0.1% of pixels in the ridge-24 render view. |
| Starburst shading and faint thin dark lines | On flat ground at LOD levels 2–3. |
| Shimmer at the 4000-unit draw distance | Reversed or log depth is the fix; future work. |
| Seam edges | 4 open seam edges where 4 children meet on a 45° plane (skirts hide them). The seam stitch tolerance could fold onto an unrelated nearby edge (untested). |
| Tests | Nothing tests "an edit's chunks show together" any more (LT13's hold hook). The 64-thread "lod edit ... held" check and LT15 are flaky under load. The render check never shows levels 4–5. |
| Performance | Every node-list change republishes the whole list (about 8.4 ms per edit on a 4 km tree). In-view uploads have no per-frame cap. `discard` in terrain.frag may turn off early depth for terrain. A chunk whose build keeps throwing is retried every physics step. |

## Physics scaling (from the lag fix, f202bca)

| Item | Notes |
| --- | --- |
| Hundreds of rigid bodies | Collider interest is incremental now, but each substep still runs 27 lookups per occupied chunk (`build_colliders_now`) even when nothing moved. Plan: remember each body's last chunk and check the ground only when it changes or is not built; add a 500-body stress benchmark with a terrain budget (about 0.5 ms a step, Release). |
| Fallen parts | Bodies that roll off the island fall forever through empty chunks. Consider `Workspace.FallenPartsDestroyHeight` (Roblox's default is -500). |

## Earlier terrain notes

- Terrain.png and some effect icons are missing from the Explorer.
- Dug pit walls keep the surface material.
- CSS is duplicated in the Configure Terrain tab.
- An unexplained freeze on the first launch of one test build; not seen since.

## Next

The sculpt tools prototype (Add, Grow, Smooth, Paint), then joint planning of the real tools (sub-project 2).
