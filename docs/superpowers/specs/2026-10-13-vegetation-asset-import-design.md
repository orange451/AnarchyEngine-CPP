# Vegetation asset import — design

Sub-project 1 of the H1Z1 Z1 procedural vegetation program. It turns the
single Roblox OBJ export at `C:\Users\Andrew\Desktop\Trees\Trees.obj` into
one Prefab per vegetation asset in a **copy** of the project, with textures
shared, materials corrected for the engine's PBR slots, pivots at ground
level, and every asset's real size measured for the forest plan
(sub-project 2).

Nothing in this sub-project places vegetation on the map.

## Working copy

All vegetation work happens in a copy of the place, never in the original:
`C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1 Vegetation`,
made by copying the whole `H1Z1 Z1` folder (src, resources, project.json,
.studio) before step 1 of the editor work; `project.json`'s `name` becomes
"H1Z1 Z1 Vegetation". The original folder is not opened or written by any
tool in this program. Later sub-projects open the same copy.

## The export

One 113 MB OBJ (`Trees.obj`, 1.07M vertices, no vertex sharing) plus
`Trees.mtl` (159 materials) and 174 PNGs. Every vertex is in Roblox world
space; the assets are laid out in rows. Groups:

| Prefix | Count | What it is |
|---|---|---|
| `Trunk` | 79 | one tree trunk each, in nine rows (one species per row) |
| `Leaves` | 269 | 248 tree crown groups + 21 free-standing bush groups |
| `Strump` | 79 | stump paired to each trunk — **chop gameplay, skipped** |
| `Log` | 105 | 78 chop logs paired to trunks — **skipped**; 27 ground logs at x 337..508 — kept |

Species rows, confirmed by the user on 2026-10-13 (z is the row's trunk
centre in export space; "heights" are trunk bounds in export units):

| Row z | Trunks | Count | Species | Leaf albedo | Bark albedos |
|---|---|---|---|---|---|
| -630 | Trunk1-7 | 7 | Spruce | Leaves22 | Trunk1 |
| -580 | Trunk57-68 | 12 | ScotsPine | Leaves22 | Trunk1, Trunk62 |
| -520 | Trunk69-79 | 11 | Fir | Leaves22 | Trunk1, Trunk62 |
| -440 | Trunk41-43 | 3 | Hawthorn | Leaves142 | Trunk41 |
| -370 | Trunk40 | 1 | Apple (lone) | Leaves1 | Trunk33 |
| -300 | Trunk17-26 | 10 | Cherry | Leaves68, Leaves78 | Trunk17, Trunk20 |
| -220 | Trunk8-16 | 9 | Sycamore | Leaves2, Leaves44 | Trunk8, Trunk10 |
| -150 | Trunk44-56 | 13 | Apple | Leaves1 | Trunk44, Trunk50 |
| -60 | Trunk27-39 | 13 | Oak | Leaves1, 2, 3 | Trunk27, Trunk33 |

Each row has exactly one trunk with no leaf groups and its own darker bark
texture (Trunk1, 62, 73, 43, 20, 10, 50, 33): that is the species' **Dead**
variant. Trunk40 joins the Apple row as one more variant.

Free-standing bush groups (all at x≈228..277, z≈0, bottoms at ground):

| Groups | Family | Leaf albedo | Sizes |
|---|---|---|---|
| Leaves12-16 | Bush-Sycamore | Leaves2 | 5, large to small |
| Leaves17-21 | Bush-Oak | Leaves3 | 5, large to small |
| Leaves7-11 | BerryBush | Leaves1 | 5, large to small |
| Leaves1 | BerryBush small | Leaves1 | 1 |
| Leaves2-6 | Card | Leaves2/3/1 | 5 single two-sided quads, 3-6 units tall |

Textures: 7 leaf albedos (RGBA, alpha is the cutout), 12 bark albedos, 5
ground-log albedos, matching `_nmap` normals for most, `_spec` maps that are
Roblox's packed roughness (G) / metalness (R) and are flat for every leaf,
and 106 `_emap` files that are constant and unused.

## Scale and pivots

**Scale factor 0.23** on every vertex, every family. Chosen so the five bush
sizes' leaf mass (ground to top, stem excluded) lands at 2.0–3.0 units, as
the user asked. Resulting sizes: old oak ≈ 17 units, old spruce ≈ 14,
young trees 4.6–7, cards 0.7–1.4, player is 2.5. Every distance quoted in
the original (stud-based) prompt is multiplied by 0.23 in later
sub-projects.

Pivot (OBJ origin after re-centring):

- **Tree:** XZ = centre of the trunk group's footprint; Y = the trunk's
  lowest vertex. The trunk base is the origin, so a GameObject's uniform
  Scale grows the tree about the ground.
- **Bush, berry bush, card:** XZ = centre of the group's bounds; Y = the
  export ground plane, −0.15 (the cards' bottom edge). The stem below that
  stays in the mesh, under the surface.
- **Ground log:** XZ = bounds centre; Y = lowest vertex.

The engine's `Prefab.OriginOffset` reports the bounds middle relative to
this origin; the plan uses the sidecar below, not that property.

## Splitting (offline Python, `tools/vegetation/split_trees.py`)

Single pass over the OBJ, streaming (never holding all vertices).

1. Parse groups with their vertex/uv/normal lines, faces and `usemtl`.
2. Classify each `Leaves` group: **bush** if its XZ centre lies more than
   1.5 trunk radii from every trunk centre and its bottom is within 4 units
   of the export ground; otherwise **crown**, assigned to the nearest trunk
   in XZ. (Checked by hand on 2026-10-13: this yields the 21 bush groups and
   248 crown groups listed above, none ambiguous.)
3. Skip `Strump*` and the 78 `Log*` groups whose XZ centre lies within 1.5
   radii of a trunk; the remaining 27 `Log*` groups are ground logs.
4. Name assets. Trees: sort each row's leafy trunks by trunk height; split
   into thirds → `Old`, `Adult`, `Young` (rows with fewer than 6 leafy
   trees get `Old` and `Young` only; Hawthorn's three are all `Old`);
   number `Var01..` by height within the class. Name
   `<Species>-<Age>-VarNN`; the leafless trunk is `<Species>-Dead`.
   Bushes: `Bush-Sycamore-Var01..05`, `Bush-Oak-Var01..05`,
   `BerryBush-Var01..06` (06 = Leaves1), `Card-Var01..05`,
   `GroundLog-Var01..27`.
5. Write one OBJ + MTL per asset to `C:\Users\Andrew\Desktop\Trees\split\`.
   Faces are re-indexed per file; vertices scaled by 0.23 and translated to
   the pivot. The MTL declares one material per *shared texture pair* the
   asset uses (e.g. `Bark_Trunk27`, `Leaf_Leaves3`) with **no `map_*`
   lines**, so the editor's importer makes no texture copies. Two groups of
   the same asset that use the same leaf albedo share one material and
   therefore merge into one mesh on import; a tree thus imports as a bark
   Model plus one Model per leaf albedo (1–3).
6. Write `split/manifest.json`: per asset — name, family, species, age,
   source groups, pivot in export space, and measured sizes in engine units:
   `height`, `crownRadius` (XZ radius of the crown groups about the trunk
   axis; for bushes the whole group), `crownBase` (lowest crown Y above the
   pivot), `trunkRadius`, and the 6-number bounds. This is the input to the
   forest plan.

## Textures and materials

Prepared offline by `tools/vegetation/prepare_textures.py` into
`split/textures/`:

- Copy the 7 leaf `_diff` (RGBA) and the 12 bark + 5 log `_diff` albedos and
  their `_nmap` normals, renamed `Leaf_<n>_Color.png`, `Leaf_<n>_Normal.png`,
  `Bark_<n>_Color.png`, `Bark_<n>_Normal.png`, `Log_<n>_*.png`.
- For each bark/log `_spec` whose G channel is not flat (within ±2/255),
  write `..._Roughness.png` from G; for a non-flat R channel write
  `..._Metalness.png`. Leaf specs are flat: no files. `_emap`: dropped.
- The engine reads each map's red channel, so the split files are
  grayscale.

In the editor (Studio open on the project, driven over MCP):

1. `import_assets` the prepared textures → Textures in `Assets.Textures`
   (moved into a `Vegetation` Folder by the Lua pass).
2. `import_assets` the split OBJs, 64 per call → one Prefab per asset in
   `Assets.Prefabs`, with Models, plus per-file Folders of Meshes and
   Materials.
3. A Lua pass (`tools/vegetation/fix_materials.luau`, run through
   `run_lua` inside one ChangeHistory recording):
   - creates 24 shared Materials under `Assets.Materials.Vegetation`:
     `Leaf_<n>` (DiffuseTexture, NormalTexture, Roughness 0.85, Metalness 0)
     and `Bark_<n>` / `Log_<n>` (DiffuseTexture, NormalTexture,
     RoughnessTexture/MetalnessTexture when they exist, else Roughness 0.9);
   - for every imported Model, maps its Material's name (`Leaf_Leaves3`
     etc.) to the shared one and reassigns `Model.Material`;
   - deletes the per-asset Material folders the importer made;
   - moves the asset Prefabs into `Assets.Prefabs.Vegetation.<Family>`
     Folders (Trees, Bushes, BerryBushes, Cards, GroundLogs).
4. Save the place.

Result: 132 Prefabs (79 trees, 27 ground logs, 10 bushes, 6 berry bushes,
5 cards), 24 Materials, ≤ 48 Textures. Nothing else in the project changes.

## Leaf rendering facts the design relies on

- Cutout is fixed in the shaders: diffuse alpha < 0.25 is discarded. The
  leaf albedos carry alpha. No material flag needed.
- Back faces are always culled. The export already includes the flipped
  copy of each leaf mesh (that is why crown groups come in identically
  bounded pairs), so leaves show from both sides. Verified on one tree in
  the editor before the full import; if a family lacks the flipped copy,
  the splitter duplicates its faces with reversed winding.
- Known engine bug D7 (cut-out surfaces cast solid shadows) is out of
  scope here; noted for the engine sub-project.

## Verification

- **Numeric, offline:** every split OBJ's bounds have min Y within 0.05 of
  0 (trees, logs) or the stem below 0 and leaf mass starting within 0.5 of 0
  (bushes); manifest `height` matches bounds; the five bushes' leaf heights
  are 2.0–3.0.
- **Numeric, in the editor:** every Prefab's `GetBoundingBox()` matches
  the manifest within 1%; Texture count ≤ 48; no Material outside
  `Assets.Materials.Vegetation` references a vegetation texture.
- **Visual:** a Lua script lines up, on flat ground next to the player
  prefab, each species' Old/Adult/Young/Dead and each bush family's sizes;
  one MCP screenshot per row for the user. Then those GameObjects are
  deleted.
- The gallery scene is not kept; the only durable change is the assets.

## Out of scope

Placement, the forest plan, tags, undergrowth scatter, stumps/chop logs,
mesh LOD, the shadow bug.
