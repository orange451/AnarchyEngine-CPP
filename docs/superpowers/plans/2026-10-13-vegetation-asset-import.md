# Vegetation Asset Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the single Roblox OBJ export `C:\Users\Andrew\Desktop\Trees\Trees.obj` into 132 vegetation Prefabs, 24 shared Materials and at most 48 Textures inside a copy of the H1Z1 Z1 project, with ground-level pivots and a manifest of measured sizes.

**Architecture:** Offline Python (streaming, no numpy) scans the OBJ once, classifies groups into assets, and writes one scaled, re-centred OBJ+MTL per asset plus `manifest.json`. A second Python script prepares the shared textures. A small Python MCP client then drives a running AnarchyStudio on the project copy: imports textures and OBJs, runs one Luau pass to build and assign the shared materials, verifies, screenshots a gallery, and saves.

**Tech Stack:** Python 3.12 (`py` launcher), pytest, Pillow (texture prep only), urllib (MCP over HTTP JSON-RPC), Luau in the studio console VM via the `run_lua` MCP tool, `build/Release/AnarchyStudio.exe`.

**Spec:** `docs/superpowers/specs/2026-10-13-vegetation-asset-import-design.md`

## Global Constraints

- Scale factor **0.23** on every vertex of every family.
- Work only in the copy `C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1 Vegetation`; never open or write `...\H1Z1 Z1`.
- Skip every `Strump*` group and the 78 `Log*` groups paired to trunks; keep the 27 ground logs.
- Per-asset MTLs carry **no `map_*` lines**; the importer must not copy textures.
- Final counts: 132 Prefabs (79 trees, 27 ground logs, 10 bushes, 6 berry bushes, 5 cards), 24 Materials under `Assets.Materials.Vegetation`, ≤ 48 Textures.
- Names: `<Species>-<Age>-VarNN`, `<Species>-Dead`, `Bush-Sycamore-VarNN`, `Bush-Oak-VarNN`, `BerryBush-VarNN`, `Card-VarNN`, `GroundLog-VarNN`. Species: Spruce, ScotsPine, Fir, Hawthorn, Apple, Cherry, Sycamore, Oak.
- Pivots: tree = trunk footprint centre XZ, trunk lowest vertex Y; bush/berry/card = bounds centre XZ, Y = −0.15 export; ground log = bounds centre XZ, lowest vertex Y. All in export space before scaling.
- Python tools live in `tools/vegetation/`; tests in `tools/vegetation/tests/`; run with `py -m pytest tools/vegetation/tests -q`.
- Scratch output goes to `C:\Users\Andrew\Desktop\Trees\split\` (beside the export, as the spec says).
- Nothing is placed on the map; the gallery GameObjects are deleted before the final save.

## Review Focus

1. **A leaf group with its XZ centre between two trunks of adjacent rows** should go to the nearer trunk, never to a bush, and never be dropped. Test in Task 2 (`test_crown_goes_to_nearest_trunk`).
2. **A tree with four crown groups where two are identical flipped copies** must still produce exactly one Model per leaf albedo on import (groups sharing a material merge). Test in Task 3 (`test_same_albedo_groups_share_material`).
3. **An OBJ face line with `v/vt/vn` indices, `v//vn`, or `v` only** must re-index without crashing or shifting texture coordinates. Test in Task 1 (`test_face_index_forms`).
4. **A `_spec` map that is flat in G but textured in R** must yield a Metalness file and no Roughness file, and the material gets scalar roughness. Test in Task 4 (`test_flat_green_textured_red`).
5. **Studio not yet answering on the port, or a tool call returning `isError`** must raise a clear Python error naming the tool and the message, never silently continue to the next import batch. Test in Task 5 (`test_tool_error_raises`, `test_wait_until_ready_times_out`).

---

## File structure

```
tools/vegetation/
  __init__.py            (empty)
  objscan.py             streaming OBJ reader → Group records (Task 1)
  catalog.py             group classification, species rows, naming (Task 2)
  split_trees.py         writes per-asset OBJ/MTL + manifest.json (Task 3)
  prepare_textures.py    shared textures, spec-map channel split (Task 4)
  mcp_client.py          JSON-RPC client + studio launcher (Task 5)
  import_vegetation.py   drives the editor import + fix pass + verify (Task 6)
  fix_materials.luau     the in-editor material pass (Task 6)
  gallery.luau           lines up assets for screenshots (Task 7)
  tests/
    conftest.py          synthetic OBJ/PNG fixtures
    test_objscan.py
    test_catalog.py
    test_split_trees.py
    test_prepare_textures.py
    test_mcp_client.py
```

---

### Task 1: Streaming OBJ group reader

**Files:**
- Create: `tools/vegetation/__init__.py`, `tools/vegetation/objscan.py`
- Test: `tools/vegetation/tests/conftest.py`, `tools/vegetation/tests/test_objscan.py`

**Interfaces:**
- Produces:
  ```python
  @dataclass
  class Group:
      name: str
      material: str | None
      v: list[tuple[float, float, float]]      # this group's own vertices
      vt: list[tuple[float, float]]
      vn: list[tuple[float, float, float]]
      faces: list[list[tuple[int, int | None, int | None]]]  # 0-based indices into v/vt/vn OF THIS GROUP
  def scan_groups(path: str) -> Iterator[Group]
  def bounds(g: Group) -> tuple[float, float, float, float, float, float]  # minx,miny,minz,maxx,maxy,maxz
  ```
  Each Group's indices are local (re-based) so a group can be written standalone. The reader keeps global running offsets for `v`, `vt`, `vn` to translate OBJ 1-based global indices (positive or negative) into local ones.

- [ ] **Step 1: Install test tooling (one-time)**

```bash
py -m pip install --user pytest pillow
```
Expected: both import cleanly: `py -c "import pytest, PIL; print('ok')"` → `ok`.

- [ ] **Step 2: Write the failing tests**

`tools/vegetation/tests/conftest.py`:
```python
import pytest, textwrap

TINY_OBJ = textwrap.dedent("""\
    # tiny
    mtllib tiny.mtl
    g Trunk1
    usemtl Trunk1Mtl
    v 0 0 0
    v 1 0 0
    v 0 2 0
    vt 0 0
    vt 1 0
    vt 0 1
    vn 0 0 1
    f 1/1/1 2/2/1 3/3/1
    g Leaves1
    usemtl Leaves1Mtl
    v 0 2 0
    v 1 2 0
    v 0 3 0
    v 1 3 0
    vt 0 0
    vt 1 0
    f 4/4 5/5 6/4
    f -1 -2 -3
    g Leaves2
    usemtl Leaves1Mtl
    v 5 0 0
    v 6 0 0
    v 5 1 0
    vn 0 1 0
    f 8//2 9//2 10//2
    """)

@pytest.fixture
def tiny_obj(tmp_path):
    p = tmp_path / "tiny.obj"
    p.write_text(TINY_OBJ)
    return str(p)
```

`tools/vegetation/tests/test_objscan.py`:
```python
from tools.vegetation.objscan import scan_groups, bounds

def test_groups_and_materials(tiny_obj):
    groups = list(scan_groups(tiny_obj))
    assert [g.name for g in groups] == ["Trunk1", "Leaves1", "Leaves2"]
    assert [g.material for g in groups] == ["Trunk1Mtl", "Leaves1Mtl", "Leaves1Mtl"]
    assert len(groups[0].v) == 3 and len(groups[1].v) == 4 and len(groups[2].v) == 3

def test_face_index_forms(tiny_obj):
    groups = list(scan_groups(tiny_obj))
    trunk, leaves1, leaves2 = groups
    assert trunk.faces == [[(0, 0, 0), (1, 1, 0), (2, 2, 0)]]
    # v/vt form, global index 4 -> local 0; vt global 4 -> local 0
    assert leaves1.faces[0] == [(0, 0, None), (1, 1, None), (2, 0, None)]
    # negative indices count back from the last vertex written so far
    assert leaves1.faces[1] == [(3, None, None), (2, None, None), (1, None, None)]
    # v//vn form; vn global 2 -> local 0 (this group's only normal)
    assert leaves2.faces[0] == [(0, None, 0), (1, None, 0), (2, None, 0)]

def test_bounds(tiny_obj):
    g = list(scan_groups(tiny_obj))[1]
    assert bounds(g) == (0.0, 2.0, 0.0, 1.0, 3.0, 0.0)
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `py -m pytest tools/vegetation/tests/test_objscan.py -q`
Expected: FAIL with `ModuleNotFoundError: No module named 'tools.vegetation'` (create the empty `tools/vegetation/__init__.py` and `tools/__init__.py`? No: use `rootdir` conftest — add `tools/vegetation/tests/conftest.py` line `import sys, pathlib; sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3]))` at the top so `tools.vegetation` imports from the repo root) then FAIL with `cannot import name 'scan_groups'`.

- [ ] **Step 4: Implement `objscan.py`**

```python
"""Streaming reader for a big OBJ: yields one Group per `g` line with its own
re-based vertex/uv/normal lists, so each group can be written standalone."""
from dataclasses import dataclass, field
from typing import Iterator

@dataclass
class Group:
    name: str
    material: str | None = None
    v: list = field(default_factory=list)
    vt: list = field(default_factory=list)
    vn: list = field(default_factory=list)
    faces: list = field(default_factory=list)

def _parse_index(token: str, total: int, start: int) -> int | None:
    """OBJ index (1-based, or negative from the end) -> local 0-based, or None."""
    if token == "":
        return None
    i = int(token)
    g = total + i if i < 0 else i - 1          # global 0-based
    return g - start

def scan_groups(path: str) -> Iterator[Group]:
    nv = nvt = nvn = 0                           # global counts so far
    cur: Group | None = None
    start = (0, 0, 0)                            # global index where cur's lists begin
    with open(path, "r", errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split(); cur.v.append((float(p[1]), float(p[2]), float(p[3]))); nv += 1
            elif line.startswith("vt "):
                p = line.split(); cur.vt.append((float(p[1]), float(p[2]))); nvt += 1
            elif line.startswith("vn "):
                p = line.split(); cur.vn.append((float(p[1]), float(p[2]), float(p[3]))); nvn += 1
            elif line.startswith("f "):
                face = []
                for tok in line.split()[1:]:
                    parts = (tok.split("/") + ["", ""])[:3]
                    face.append((_parse_index(parts[0], nv, start[0]),
                                 _parse_index(parts[1], nvt, start[1]),
                                 _parse_index(parts[2], nvn, start[2])))
                cur.faces.append(face)
            elif line.startswith("usemtl"):
                cur.material = line.split(maxsplit=1)[1].strip()
            elif line.startswith("g ") or line.startswith("o "):
                if cur is not None:
                    yield cur
                cur = Group(name=line[2:].strip())
                start = (nv, nvt, nvn)
    if cur is not None:
        yield cur

def bounds(g: Group):
    xs = [p[0] for p in g.v]; ys = [p[1] for p in g.v]; zs = [p[2] for p in g.v]
    return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))
```
Note: a vertex line before any `g` would crash on `cur.v`; the export starts with `g Leaves1`, and the test file too. Guard anyway: if `cur is None` on a data line, create `Group(name="default")`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `py -m pytest tools/vegetation/tests/test_objscan.py -q`
Expected: `3 passed`

- [ ] **Step 6: Smoke the real export**

Run: `py -c "import sys; sys.path.insert(0,'.'); from tools.vegetation.objscan import scan_groups; import collections; c=collections.Counter(g.name.rstrip('0123456789') for g in scan_groups(r'C:\Users\Andrew\Desktop\Trees\Trees.obj')); print(c)"`
Expected: `Counter({'Leaves': 269, 'Log': 105, 'Trunk': 79, 'Strump': 79})` (takes about a minute).

- [ ] **Step 7: Commit**

```bash
git add tools/vegetation/__init__.py tools/vegetation/objscan.py tools/vegetation/tests/conftest.py tools/vegetation/tests/test_objscan.py
git commit -m "Vegetation: streaming OBJ group reader"
```

---

### Task 2: Catalog — classify groups into assets and name them

**Files:**
- Create: `tools/vegetation/catalog.py`
- Test: `tools/vegetation/tests/test_catalog.py`

**Interfaces:**
- Consumes: `Group`, `bounds` from Task 1.
- Produces:
  ```python
  GROUND_Y = -0.15
  SPECIES_ROWS = {  # row z (export) -> species; row = round(trunk_cz / 10) * 10
      -630: "Spruce", -580: "ScotsPine", -520: "Fir", -440: "Hawthorn",
      -370: "Apple", -300: "Cherry", -220: "Sycamore", -150: "Apple", -60: "Oak"}
  BUSH_FAMILY_BY_ALBEDO = {"Leaves2_diff.png": "Bush-Sycamore", "Leaves3_diff.png": "Bush-Oak", "Leaves1_diff.png": "BerryBush"}

  @dataclass
  class Asset:
      name: str            # final prefab name
      family: str          # "Tree" | "Bush" | "BerryBush" | "Card" | "GroundLog"
      species: str | None
      age: str | None      # "Old" | "Adult" | "Young" | "Dead" | None
      trunk: str | None    # group name of the trunk (trees)
      groups: list[str]    # every group name in this asset (trunk first for trees)
      pivot: tuple[float, float, float]   # export space

  def read_mtl_albedos(mtl_path: str) -> dict[str, str]      # material name -> map_Kd file name
  def build_catalog(groups: list[GroupInfo], albedo_of: dict[str, str]) -> list[Asset]
  ```
  where `GroupInfo` is a light record `(name, material, bounds)` so the catalog never needs vertex lists:
  ```python
  @dataclass
  class GroupInfo:
      name: str
      material: str | None
      bounds: tuple[float, float, float, float, float, float]
      faces: int
  def summarize(g: Group) -> GroupInfo
  ```

Classification rules (from the spec):
- trunk = name starts with `Trunk`; trunk centre = XZ centre of bounds; trunk radius = max(width, depth)/2.
- `Leaves*`: **bush** if XZ distance to every trunk centre > 1.5 × that trunk's radius AND `miny - GROUND_Y <= 4`; else **crown** of the nearest trunk.
- `Strump*`: skipped. `Log*`: skipped if within 1.5 radii of any trunk centre (chop log), else ground log.
- Rows: trunks grouped by `round(cz/10)*10`; species from `SPECIES_ROWS` (KeyError → `ValueError("unknown row z")`).
- Ages: in a row, leafy trunks sorted by height descending; `n>=6`: thirds (ceil for Old, remainder split Adult then Young, i.e. `old = ceil(n/3)`, `young = n//3`, `adult = n - old - young`); `3 <= n < 6`: Old and Young halves (`old = ceil(n/2)`); Hawthorn (n==3) all Old; n==1 Old. Leafless trunk → `Dead`. `VarNN` numbers from 01 by height within class.
- Both Apple rows (-370 and -150) are one species list for naming (row -370's tree joins by height).
- Bushes: by albedo family, sorted by height descending → `Var01..`. `BerryBush` gets 6 (Leaves1 is the smallest). Cards = Leaves groups with exactly 8 faces → `Card-Var01..05` sorted by height descending. Ground logs → `GroundLog-Var01..27` sorted by bounds max-dimension descending.
- Pivot per family as in Global Constraints.

- [ ] **Step 1: Write the failing tests**

`tools/vegetation/tests/test_catalog.py`:
```python
from tools.vegetation.catalog import GroupInfo, build_catalog, read_mtl_albedos, GROUND_Y

def gi(name, mat, minx, miny, minz, maxx, maxy, maxz, faces=100):
    return GroupInfo(name, mat, (minx, miny, minz, maxx, maxy, maxz), faces)

ALBEDO = {"T1": "Trunk1_diff.png", "T2": "Trunk62_diff.png", "L22": "Leaves22_diff.png",
          "L2": "Leaves2_diff.png", "L3": "Leaves3_diff.png", "L1": "Leaves1_diff.png", "LG": "Log1_diff.png"}

def spruce_row():
    # four trunks in row z=-630 at x=-90,-60,-30,0; heights 60,50,40 + a leafless dead one
    return [gi("Trunk1", "T2", -98, -2, -642, -82, 58, -626),      # dead (no crown)
            gi("Trunk2", "T1", -68, -2, -642, -52, 48, -626),
            gi("Trunk3", "T1", -38, -2, -642, -22, 38, -626),
            gi("Trunk4", "T1", -8, -2, -642, 8, 28, -626),
            gi("Leaves10", "L22", -75, 20, -650, -45, 50, -618),
            gi("Leaves11", "L22", -75, 20, -650, -45, 50, -618),
            gi("Leaves12", "L22", -45, 15, -648, -15, 40, -620),
            gi("Leaves13", "L22", -15, 10, -646, 15, 30, -622)]

def test_tree_ages_and_dead():
    cat = build_catalog(spruce_row(), ALBEDO)
    by = {a.name: a for a in cat}
    assert set(by) == {"Spruce-Dead", "Spruce-Old-Var01", "Spruce-Young-Var01", "Spruce-Young-Var02"}
    assert by["Spruce-Dead"].groups == ["Trunk1"]
    assert by["Spruce-Old-Var01"].groups == ["Trunk2", "Leaves10", "Leaves11"]
    assert by["Spruce-Old-Var01"].pivot == (-60.0, -2.0, -634.0)

def test_crown_goes_to_nearest_trunk():
    rows = spruce_row()
    # a crown centred between Trunk3 (x=-30) and Trunk4 (x=0), slightly nearer Trunk4, bottom high up
    rows.append(gi("Leaves99", "L22", -28, 12, -646, 2, 40, -622))
    cat = build_catalog(rows, ALBEDO)
    owner = [a for a in cat if "Leaves99" in a.groups]
    assert len(owner) == 1 and owner[0].trunk == "Trunk4"

def test_bushes_cards_and_logs():
    rows = spruce_row() + [
        gi("Leaves20", "L2", 268, -2.3, -9, 284, 13.7, 9),    # Bush-Sycamore big
        gi("Leaves21", "L2", 270, -2.6, -6, 282, 6.8, 4),     # Bush-Sycamore small
        gi("Leaves22", "L3", 266, -2.1, -9, 283, 12.5, 9),    # Bush-Oak
        gi("Leaves23", "L1", 268, -2.3, -10, 284, 13.1, 8),   # BerryBush
        gi("Leaves24", "L1", 222, -1.0, -4.5, 232.5, 6.1, 4.3),  # BerryBush small
        gi("Leaves25", "L2", 273, -0.15, -2, 278, 6.05, 2, faces=8),  # Card
        gi("Log1", "LG", 340, 0, -190, 346, 2.2, -186),       # ground log
        gi("Log2", "T1", -68, -2, -700, -52, 44, -690),       # chop log near Trunk2? no: far in z -> ground
        gi("Log3", "T1", -66, 0, -640, -54, 45, -628),        # chop log inside Trunk2 footprint -> skipped
        gi("Strump1", "T1", -66, -2, -640, -54, 3, -628)]
    cat = build_catalog(rows, ALBEDO)
    names = sorted(a.name for a in cat)
    assert "Bush-Sycamore-Var01" in names and "Bush-Sycamore-Var02" in names
    assert "Bush-Oak-Var01" in names
    assert "BerryBush-Var01" in names and "BerryBush-Var02" in names
    assert "Card-Var01" in names
    assert "GroundLog-Var01" in names and "GroundLog-Var02" in names
    assert not any("Log3" in a.groups or "Strump1" in a.groups for a in cat)
    bush = next(a for a in cat if a.name == "Bush-Sycamore-Var01")
    assert bush.pivot == (276.0, GROUND_Y, 0.0)
    card = next(a for a in cat if a.name == "Card-Var01")
    assert card.family == "Card"

def test_read_mtl_albedos(tmp_path):
    p = tmp_path / "t.mtl"
    p.write_text("newmtl A\nmap_Kd Leaves1_diff.png\nnewmtl B\nKs 0 0 0\n")
    assert read_mtl_albedos(str(p)) == {"A": "Leaves1_diff.png"}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `py -m pytest tools/vegetation/tests/test_catalog.py -q`
Expected: FAIL, `cannot import name 'GroupInfo'`.

- [ ] **Step 3: Implement `catalog.py`**

```python
"""Classifies the export's groups into vegetation assets and names them."""
from dataclasses import dataclass
import math
from .objscan import Group, bounds as group_bounds

GROUND_Y = -0.15
SPECIES_ROWS = {-630: "Spruce", -580: "ScotsPine", -520: "Fir", -440: "Hawthorn",
                -370: "Apple", -300: "Cherry", -220: "Sycamore", -150: "Apple", -60: "Oak"}
BUSH_FAMILY_BY_ALBEDO = {"Leaves2_diff.png": "Bush-Sycamore", "Leaves3_diff.png": "Bush-Oak",
                         "Leaves1_diff.png": "BerryBush"}
NEAR = 1.5          # trunk radii: inside this a Leaves/Log group belongs to the trunk
BUSH_BOTTOM = 4.0   # a free-standing Leaves group sits within this of the ground

@dataclass
class GroupInfo:
    name: str
    material: str | None
    bounds: tuple
    faces: int

@dataclass
class Asset:
    name: str
    family: str
    species: str | None
    age: str | None
    trunk: str | None
    groups: list
    pivot: tuple

def summarize(g: Group) -> GroupInfo:
    return GroupInfo(g.name, g.material, group_bounds(g), len(g.faces))

def read_mtl_albedos(mtl_path: str) -> dict:
    out, cur = {}, None
    for line in open(mtl_path, errors="ignore"):
        if line.startswith("newmtl"):
            cur = line.split(maxsplit=1)[1].strip()
        elif line.startswith("map_Kd") and cur:
            out[cur] = line.split(maxsplit=1)[1].strip()
    return out

def _cx(b): return (b[0] + b[3]) / 2
def _cz(b): return (b[2] + b[5]) / 2
def _h(b): return b[4] - b[1]
def _radius(b): return max(b[3] - b[0], b[5] - b[2]) / 2

def _nearest_trunk(trunks, b):
    best, bd = None, math.inf
    for t in trunks:
        d = math.hypot(_cx(t.bounds) - _cx(b), _cz(t.bounds) - _cz(b))
        if d < bd:
            best, bd = t, d
    return best, bd

def _ages(n: int, species: str) -> list:
    if species == "Hawthorn" or n <= 1:
        return ["Old"] * n
    if n < 6:
        old = math.ceil(n / 2)
        return ["Old"] * old + ["Young"] * (n - old)
    old = math.ceil(n / 3); young = n // 3
    return ["Old"] * old + ["Adult"] * (n - old - young) + ["Young"] * young

def build_catalog(groups: list, albedo_of: dict) -> list:
    trunks = [g for g in groups if g.name.startswith("Trunk")]
    crowns = {t.name: [] for t in trunks}
    bushes, cards, logs = [], [], []
    for g in groups:
        if g.name.startswith("Leaves"):
            t, d = _nearest_trunk(trunks, g.bounds)
            free = (t is None or d > NEAR * _radius(t.bounds)) and (g.bounds[1] - GROUND_Y) <= BUSH_BOTTOM
            if free:
                (cards if g.faces == 8 else bushes).append(g)
            else:
                crowns[t.name].append(g)
        elif g.name.startswith("Log"):
            t, d = _nearest_trunk(trunks, g.bounds)
            if t is None or d > NEAR * _radius(t.bounds):
                logs.append(g)
        # Strump*: skipped
    assets = []
    # --- trees, by species (both Apple rows merge) ---
    by_species = {}
    for t in trunks:
        row = round(_cz(t.bounds) / 10) * 10
        if row not in SPECIES_ROWS:
            raise ValueError(f"unknown row z={row} for {t.name}")
        by_species.setdefault(SPECIES_ROWS[row], []).append(t)
    for species, ts in by_species.items():
        leafy = sorted([t for t in ts if crowns[t.name]], key=lambda t: -_h(t.bounds))
        ages = _ages(len(leafy), species)
        counter = {}
        for t, age in zip(leafy, ages):
            counter[age] = counter.get(age, 0) + 1
            cg = sorted(crowns[t.name], key=lambda g: int(g.name[6:]))
            assets.append(Asset(f"{species}-{age}-Var{counter[age]:02d}", "Tree", species, age, t.name,
                                [t.name] + [g.name for g in cg],
                                (_cx(t.bounds), t.bounds[1], _cz(t.bounds))))
        dead = [t for t in ts if not crowns[t.name]]
        for i, t in enumerate(dead):
            suffix = "" if len(dead) == 1 else f"-Var{i + 1:02d}"
            assets.append(Asset(f"{species}-Dead{suffix}", "Tree", species, "Dead", t.name, [t.name],
                                (_cx(t.bounds), t.bounds[1], _cz(t.bounds))))
    # --- bushes ---
    fam_groups = {}
    for g in bushes:
        fam = BUSH_FAMILY_BY_ALBEDO[albedo_of[g.material]]
        fam_groups.setdefault(fam, []).append(g)
    for fam, gs in fam_groups.items():
        family = "BerryBush" if fam == "BerryBush" else "Bush"
        for i, g in enumerate(sorted(gs, key=lambda g: -_h(g.bounds))):
            assets.append(Asset(f"{fam}-Var{i + 1:02d}", family, None, None, None, [g.name],
                                (_cx(g.bounds), GROUND_Y, _cz(g.bounds))))
    for i, g in enumerate(sorted(cards, key=lambda g: -_h(g.bounds))):
        assets.append(Asset(f"Card-Var{i + 1:02d}", "Card", None, None, None, [g.name],
                            (_cx(g.bounds), GROUND_Y, _cz(g.bounds))))
    for i, g in enumerate(sorted(logs, key=lambda g: -max(g.bounds[3] - g.bounds[0], g.bounds[5] - g.bounds[2]))):
        assets.append(Asset(f"GroundLog-Var{i + 1:02d}", "GroundLog", None, None, None, [g.name],
                            (_cx(g.bounds), g.bounds[1], _cz(g.bounds))))
    return assets
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `py -m pytest tools/vegetation/tests/test_catalog.py -q`
Expected: `4 passed`. If `test_bushes_cards_and_logs` fails on `Log2`: it sits 60 units from Trunk2 in z, so it must be a ground log (`GroundLog-Var02` by size); fix the rule, not the test.

- [ ] **Step 5: Run the catalog on the real export and check the counts**

Add `tools/vegetation/catalog.py` `__main__`:
```python
if __name__ == "__main__":
    import sys, collections
    from .objscan import scan_groups
    obj, mtl = sys.argv[1], sys.argv[2]
    infos = [summarize(g) for g in scan_groups(obj)]
    cat = build_catalog(infos, read_mtl_albedos(mtl))
    print(collections.Counter(a.family for a in cat))
    print(collections.Counter((a.species, a.age) for a in cat if a.family == "Tree"))
    for a in cat: print(a.name, a.groups)
```
Run: `py -m tools.vegetation.catalog "C:\Users\Andrew\Desktop\Trees\Trees.obj" "C:\Users\Andrew\Desktop\Trees\Trees.mtl" > "C:\Users\Andrew\Desktop\Trees\catalog.txt"` then `head -3 "C:\Users\Andrew\Desktop\Trees\catalog.txt"`.
Expected first line: `Counter({'Tree': 79, 'GroundLog': 27, 'Bush': 10, 'BerryBush': 6, 'Card': 5})`. Expected dead trees: 8 (`-Dead` with no suffix). If counts differ, print the misfits and fix the rule.

- [ ] **Step 6: Commit**

```bash
git add tools/vegetation/catalog.py tools/vegetation/tests/test_catalog.py
git commit -m "Vegetation: classify export groups into named assets"
```

---

### Task 3: Splitter — per-asset OBJ/MTL and the manifest

**Files:**
- Create: `tools/vegetation/split_trees.py`
- Test: `tools/vegetation/tests/test_split_trees.py`

**Interfaces:**
- Consumes: `scan_groups`, `Group` (Task 1); `summarize`, `build_catalog`, `read_mtl_albedos`, `Asset` (Task 2).
- Produces:
  ```python
  SCALE = 0.23
  def material_key(albedo_file: str) -> str
      # "Leaves3_diff.png" -> "Leaf_Leaves3"; "Trunk27_diff.png" -> "Bark_Trunk27"; "Log9_diff.png" -> "Log_Log9"
  def write_asset(asset: Asset, groups: dict[str, Group], albedo_of: dict, out_dir: str) -> dict
      # writes <out_dir>/<name>.obj and <name>.mtl; returns the manifest entry
  def split(obj_path: str, mtl_path: str, out_dir: str) -> list[dict]   # writes manifest.json too
  ```
  Manifest entry:
  ```json
  {"name": "Oak-Old-Var01", "family": "Tree", "species": "Oak", "age": "Old",
   "groups": ["Trunk34", "Leaves118", ...], "pivotExport": [x, y, z],
   "materials": ["Bark_Trunk27", "Leaf_Leaves3"],
   "height": 17.4, "crownRadius": 9.8, "crownBase": 3.1, "trunkRadius": 0.9,
   "bounds": [minx, miny, minz, maxx, maxy, maxz]}
  ```
  All numbers in engine units (already scaled). `crownRadius`: trees = max over crown vertices of XZ distance from the pivot axis; bushes/cards/logs = max over all vertices. `crownBase`: trees = min Y over crown vertices (0 if no crown); others = 0. `trunkRadius`: trees = max XZ radius of trunk vertices below 1.0 engine unit above the pivot; others = 0.

Writing: vertices `(x - px, y - py, z - pz) * SCALE`; faces re-indexed into the asset's single running `v/vt/vn` lists; a `usemtl <material_key>` before each group's faces, with groups ordered trunk first. MTL: one `newmtl` per distinct key, with only `Kd 1 1 1`, `d 1`, `illum 2` (no maps).

Streaming: `split()` must not hold all 532 groups' vertices at once. Do two passes: pass 1 `summarize` every group (bounds only) and build the catalog, then compute `needed = {group -> asset}`; pass 2 re-scan and collect only the groups of the asset currently being assembled — since groups of one asset are not contiguous in the file, instead collect all needed groups' vertex data in a dict and write assets as soon as every group of an asset has arrived (`remaining[asset] -= 1`), then drop them. Peak memory stays small.

- [ ] **Step 1: Write the failing tests**

`tools/vegetation/tests/test_split_trees.py`:
```python
import json, os
from tools.vegetation.objscan import scan_groups, Group
from tools.vegetation.catalog import Asset
from tools.vegetation.split_trees import SCALE, material_key, write_asset, split

def test_material_key():
    assert material_key("Leaves3_diff.png") == "Leaf_Leaves3"
    assert material_key("Trunk27_diff.png") == "Bark_Trunk27"
    assert material_key("Log9_diff.png") == "Log_Log9"

def test_write_tree_pivot_scale_and_reindex(tmp_path):
    trunk = Group("Trunk1", "T1", v=[(10, 5, 0), (12, 5, 0), (11, 25, 0)], vt=[(0, 0), (1, 0), (0, 1)],
                  faces=[[(0, 0, None), (1, 1, None), (2, 2, None)]])
    leaves = Group("Leaves1", "L3", v=[(5, 20, -5), (17, 20, 5), (11, 30, 0)], vt=[(0, 0), (1, 0), (0, 1)],
                   faces=[[(0, 0, None), (1, 1, None), (2, 2, None)]])
    flipped = Group("Leaves2", "L3", v=list(leaves.v), vt=list(leaves.vt),
                    faces=[[(2, 2, None), (1, 1, None), (0, 0, None)]])
    asset = Asset("Oak-Old-Var01", "Tree", "Oak", "Old", "Trunk1", ["Trunk1", "Leaves1", "Leaves2"], (11.0, 5.0, 0.0))
    entry = write_asset(asset, {"Trunk1": trunk, "Leaves1": leaves, "Leaves2": flipped},
                        {"T1": "Trunk27_diff.png", "L3": "Leaves3_diff.png"}, str(tmp_path))
    text = (tmp_path / "Oak-Old-Var01.obj").read_text()
    assert "mtllib Oak-Old-Var01.mtl" in text
    assert text.count("usemtl Bark_Trunk27") == 1 and text.count("usemtl Leaf_Leaves3") == 2
    # the trunk base vertex lands on the origin, scaled
    assert "v 0.0000 0.0000 0.0000" in text or "v -0.2300 0.0000 0.0000" in text
    # faces of the second group are re-indexed after the first group's 3 vertices
    assert "f 4/4 5/5 6/6" in text and "f 9/9 8/8 7/7" in text
    mtl = (tmp_path / "Oak-Old-Var01.mtl").read_text()
    assert "map_" not in mtl and mtl.count("newmtl") == 2
    assert entry["materials"] == ["Bark_Trunk27", "Leaf_Leaves3"]
    assert abs(entry["height"] - 25 * SCALE) < 1e-6
    assert abs(entry["crownRadius"] - (6 ** 2 + 5 ** 2) ** 0.5 * SCALE) < 1e-6
    assert abs(entry["crownBase"] - 15 * SCALE) < 1e-6
    assert abs(entry["bounds"][1]) < 1e-9

def test_same_albedo_groups_share_material(tmp_path):
    a = Group("Leaves1", "LA", v=[(0, 0, 0), (1, 0, 0), (0, 1, 0)], faces=[[(0, None, None), (1, None, None), (2, None, None)]])
    b = Group("Leaves2", "LB", v=[(0, 0, 1), (1, 0, 1), (0, 1, 1)], faces=[[(0, None, None), (1, None, None), (2, None, None)]])
    asset = Asset("Bush-Oak-Var01", "Bush", None, None, None, ["Leaves1", "Leaves2"], (0.5, -0.15, 0.5))
    entry = write_asset(asset, {"Leaves1": a, "Leaves2": b}, {"LA": "Leaves3_diff.png", "LB": "Leaves3_diff.png"}, str(tmp_path))
    assert entry["materials"] == ["Leaf_Leaves3"]
    assert (tmp_path / "Bush-Oak-Var01.mtl").read_text().count("newmtl") == 1

def test_split_end_to_end(tiny_obj, tmp_path):
    # tiny_obj has Trunk1 at x=0..1,z=0 and Leaves1 above it, Leaves2 far away at ground -> a bush
    mtl = tmp_path / "tiny.mtl"
    mtl.write_text("newmtl Trunk1Mtl\nmap_Kd Trunk27_diff.png\nnewmtl Leaves1Mtl\nmap_Kd Leaves3_diff.png\n")
    out = tmp_path / "split"
    entries = split(tiny_obj, str(mtl), str(out))
    # Trunk1's z-row is 0 -> not a known species row: the tiny fixture must be shifted, see Step 3
    names = sorted(e["name"] for e in entries)
    assert names == ["Bush-Oak-Var01", "Oak-Old-Var01"]
    assert json.load(open(out / "manifest.json")) == entries
    assert {p.name for p in out.iterdir()} >= {"Oak-Old-Var01.obj", "Oak-Old-Var01.mtl", "Bush-Oak-Var01.obj", "manifest.json"}
```
The last test needs the tiny fixture's trunk in a species row: in `conftest.py` change `Trunk1`'s and `Leaves1`'s z from `0` to `-60` (Oak row) — i.e. every `v x y 0` line of those two groups becomes `v x y -60`. Keep `Leaves2` at z=0. Re-run Task 1's tests afterwards (`test_bounds` then expects `(0.0, 2.0, -60.0, 1.0, 3.0, -60.0)`; update it).

- [ ] **Step 2: Run tests to verify they fail**

Run: `py -m pytest tools/vegetation/tests/test_split_trees.py -q`
Expected: FAIL, `cannot import name 'SCALE'`.

- [ ] **Step 3: Implement `split_trees.py`**

```python
"""Writes one scaled, re-centred OBJ+MTL per vegetation asset and manifest.json."""
import json, math, os, sys
from .objscan import scan_groups, Group
from .catalog import Asset, summarize, build_catalog, read_mtl_albedos

SCALE = 0.23

def material_key(albedo_file: str) -> str:
    stem = albedo_file.split("_diff")[0]
    if stem.startswith("Leaves"): return "Leaf_" + stem
    if stem.startswith("Trunk"): return "Bark_" + stem
    if stem.startswith("Log"): return "Log_" + stem
    raise ValueError(f"unknown albedo {albedo_file}")

def write_asset(asset: Asset, groups: dict, albedo_of: dict, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    px, py, pz = asset.pivot
    keys = []
    lines = [f"mtllib {asset.name}.mtl"]
    nv = nvt = nvn = 0
    lo = [math.inf] * 3; hi = [-math.inf] * 3
    crown_r = 0.0; crown_base = math.inf; trunk_r = 0.0
    for gname in asset.groups:
        g = groups[gname]
        key = material_key(albedo_of[g.material])
        if key not in keys: keys.append(key)
        lines.append(f"g {gname}")
        lines.append(f"usemtl {key}")
        is_trunk = asset.family == "Tree" and gname == asset.trunk
        for (x, y, z) in g.v:
            X, Y, Z = (x - px) * SCALE, (y - py) * SCALE, (z - pz) * SCALE
            lines.append(f"v {X:.4f} {Y:.4f} {Z:.4f}")
            for i, c in enumerate((X, Y, Z)):
                lo[i] = min(lo[i], c); hi[i] = max(hi[i], c)
            r = math.hypot(X, Z)
            if is_trunk:
                if Y <= 1.0: trunk_r = max(trunk_r, r)
            else:
                crown_r = max(crown_r, r); crown_base = min(crown_base, Y)
        for (u, v) in g.vt: lines.append(f"vt {u:.5f} {v:.5f}")
        for (a, b, c) in g.vn: lines.append(f"vn {a:.4f} {b:.4f} {c:.4f}")
        for face in g.faces:
            toks = []
            for (vi, ti, ni) in face:
                s = str(vi + 1 + nv)
                if ti is not None or ni is not None:
                    s += "/" + ("" if ti is None else str(ti + 1 + nvt))
                if ni is not None:
                    s += "/" + str(ni + 1 + nvn)
                toks.append(s)
            lines.append("f " + " ".join(toks))
        nv += len(g.v); nvt += len(g.vt); nvn += len(g.vn)
    with open(os.path.join(out_dir, asset.name + ".obj"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out_dir, asset.name + ".mtl"), "w", newline="\n") as f:
        for k in keys:
            f.write(f"newmtl {k}\nKd 1 1 1\nd 1\nillum 2\n")
    if asset.family != "Tree":
        crown_base = 0.0
    if crown_base is math.inf:
        crown_base = 0.0
    return {"name": asset.name, "family": asset.family, "species": asset.species, "age": asset.age,
            "groups": list(asset.groups), "pivotExport": [px, py, pz], "materials": keys,
            "height": hi[1] - min(lo[1], 0.0), "crownRadius": crown_r, "crownBase": crown_base,
            "trunkRadius": trunk_r, "bounds": lo + hi}

def split(obj_path: str, mtl_path: str, out_dir: str) -> list:
    albedo_of = read_mtl_albedos(mtl_path)
    infos = [summarize(g) for g in scan_groups(obj_path)]
    catalog = build_catalog(infos, albedo_of)
    owner = {}
    for a in catalog:
        for g in a.groups: owner[g] = a
    remaining = {a.name: len(a.groups) for a in catalog}
    held: dict = {}
    entries = {}
    for g in scan_groups(obj_path):
        a = owner.get(g.name)
        if a is None: continue
        held[g.name] = g
        remaining[a.name] -= 1
        if remaining[a.name] == 0:
            entries[a.name] = write_asset(a, held, albedo_of, out_dir)
            for name in a.groups: held.pop(name, None)
    ordered = [entries[a.name] for a in catalog]
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(ordered, f, indent=1)
    return ordered

if __name__ == "__main__":
    entries = split(sys.argv[1], sys.argv[2], sys.argv[3])
    print(len(entries), "assets written")
```
`height` for trees: `hi[1]` (the pivot is the base, so min Y is 0); for bushes the formula gives top above ground (stem below 0 ignored), which is the "leaf mass" height the user asked about.

- [ ] **Step 4: Run tests to verify they pass**

Run: `py -m pytest tools/vegetation/tests -q`
Expected: all pass (Task 1 + 2 + 3 tests).

- [ ] **Step 5: Split the real export and check it**

Run:
```bash
py -m tools.vegetation.split_trees "C:\Users\Andrew\Desktop\Trees\Trees.obj" "C:\Users\Andrew\Desktop\Trees\Trees.mtl" "C:\Users\Andrew\Desktop\Trees\split"
```
Expected: `132 assets written` (2–3 minutes). Then check:
```bash
py -c "import json;m=json.load(open(r'C:\Users\Andrew\Desktop\Trees\split\manifest.json'));import collections;print(collections.Counter(e['family'] for e in m));print(sorted((round(e['height'],2),e['name']) for e in m if e['family']=='Bush'));print(max(abs(e['bounds'][1]) for e in m if e['family'] in ('Tree','GroundLog')));print([ (e['name'],round(e['height'],1),round(e['crownRadius'],1)) for e in m if e['name'].startswith('Oak-Old')])"
```
Expected: families `Tree 79, GroundLog 27, Bush 10, BerryBush 6, Card 5`; bush heights between 1.9 and 3.1; the tree/log min-Y line prints `0.0` (≤ 0.05); Oak-Old heights ≈ 15–17.5 with crown radii ≈ 8–10. Record the actual numbers in the commit message.

- [ ] **Step 6: Commit**

```bash
git add tools/vegetation/split_trees.py tools/vegetation/tests/test_split_trees.py tools/vegetation/tests/conftest.py tools/vegetation/tests/test_objscan.py
git commit -m "Vegetation: split the export into per-asset OBJs with a size manifest"
```

---

### Task 4: Shared texture preparation

**Files:**
- Create: `tools/vegetation/prepare_textures.py`
- Test: `tools/vegetation/tests/test_prepare_textures.py`

**Interfaces:**
- Consumes: `manifest.json` (Task 3) to learn which material keys are used.
- Produces:
  ```python
  FLAT_TOLERANCE = 2   # a channel whose max-min <= this is flat
  def channel_is_flat(img, channel: int) -> bool           # PIL image, 0=R,1=G,2=B
  def prepare(src_dir: str, manifest_path: str, out_dir: str) -> dict
      # returns {"Leaf_Leaves3": {"Color": "Leaf_Leaves3_Color.png", "Normal": ..., "Roughness": None, "Metalness": None}, ...}
      # and writes <out_dir>/textures.json with that dict
  ```
  Rules: for each key `Leaf_X`/`Bark_X`/`Log_X` in any manifest entry's `materials`: copy `X_diff.png` → `<Key>_Color.png`; if `X_nmap.png` exists copy → `<Key>_Normal.png`; for Bark/Log keys with `X_spec.png`: if G not flat write `<Key>_Roughness.png` (grayscale "L" from G), if R not flat write `<Key>_Metalness.png` (from R). Leaf keys never get Roughness/Metalness files. Any `_emap` is ignored.

- [ ] **Step 1: Write the failing tests**

`tools/vegetation/tests/test_prepare_textures.py`:
```python
import json
from PIL import Image
from tools.vegetation.prepare_textures import channel_is_flat, prepare

def png(path, pixels):  # pixels: function (x,y)->(r,g,b,a) over 4x4
    im = Image.new("RGBA", (4, 4))
    for y in range(4):
        for x in range(4):
            im.putpixel((x, y), pixels(x, y))
    im.save(path)

def test_channel_is_flat(tmp_path):
    p = tmp_path / "a.png"; png(p, lambda x, y: (x * 40, 128, 0, 255))
    im = Image.open(p)
    assert channel_is_flat(im, 1) and not channel_is_flat(im, 0)

def test_flat_green_textured_red(tmp_path):
    src = tmp_path / "src"; src.mkdir(); out = tmp_path / "out"
    png(src / "Trunk9_diff.png", lambda x, y: (100, 80, 60, 255))
    png(src / "Trunk9_nmap.png", lambda x, y: (128, 128, 255, 255))
    png(src / "Trunk9_spec.png", lambda x, y: (x * 60, 128, 0, 255))   # R textured, G flat
    png(src / "Leaves9_diff.png", lambda x, y: (0, 120, 0, 255 if x else 0))
    png(src / "Leaves9_spec.png", lambda x, y: (0, 128, 0, 255))
    m = tmp_path / "manifest.json"
    m.write_text(json.dumps([{"name": "A", "materials": ["Bark_Trunk9", "Leaf_Leaves9"]}]))
    result = prepare(str(src), str(m), str(out))
    assert result["Bark_Trunk9"] == {"Color": "Bark_Trunk9_Color.png", "Normal": "Bark_Trunk9_Normal.png",
                                     "Roughness": None, "Metalness": "Bark_Trunk9_Metalness.png"}
    assert result["Leaf_Leaves9"] == {"Color": "Leaf_Leaves9_Color.png", "Normal": None, "Roughness": None, "Metalness": None}
    assert Image.open(out / "Bark_Trunk9_Metalness.png").mode == "L"
    assert Image.open(out / "Leaf_Leaves9_Color.png").mode == "RGBA"
    assert json.load(open(out / "textures.json")) == result
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `py -m pytest tools/vegetation/tests/test_prepare_textures.py -q`
Expected: FAIL, `No module named 'tools.vegetation.prepare_textures'`.

- [ ] **Step 3: Implement `prepare_textures.py`**

```python
"""Copies the export's shared textures under engine names and splits Roblox's
packed spec maps (R = metalness, G = roughness) into the engine's one-map-per-slot files."""
import json, os, shutil, sys
from PIL import Image

FLAT_TOLERANCE = 2

def channel_is_flat(img, channel: int) -> bool:
    band = img.convert("RGBA").getchannel(channel)
    lo, hi = band.getextrema()
    return hi - lo <= FLAT_TOLERANCE

def prepare(src_dir: str, manifest_path: str, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    keys = []
    for entry in json.load(open(manifest_path)):
        for k in entry["materials"]:
            if k not in keys: keys.append(k)
    result = {}
    for key in keys:
        kind, stem = key.split("_", 1)
        files = {"Color": None, "Normal": None, "Roughness": None, "Metalness": None}
        shutil.copyfile(os.path.join(src_dir, f"{stem}_diff.png"), os.path.join(out_dir, f"{key}_Color.png"))
        files["Color"] = f"{key}_Color.png"
        nmap = os.path.join(src_dir, f"{stem}_nmap.png")
        if os.path.exists(nmap):
            shutil.copyfile(nmap, os.path.join(out_dir, f"{key}_Normal.png")); files["Normal"] = f"{key}_Normal.png"
        spec = os.path.join(src_dir, f"{stem}_spec.png")
        if kind in ("Bark", "Log") and os.path.exists(spec):
            img = Image.open(spec).convert("RGBA")
            if not channel_is_flat(img, 1):
                img.getchannel(1).save(os.path.join(out_dir, f"{key}_Roughness.png")); files["Roughness"] = f"{key}_Roughness.png"
            if not channel_is_flat(img, 0):
                img.getchannel(0).save(os.path.join(out_dir, f"{key}_Metalness.png")); files["Metalness"] = f"{key}_Metalness.png"
        result[key] = files
    with open(os.path.join(out_dir, "textures.json"), "w") as f:
        json.dump(result, f, indent=1)
    return result

if __name__ == "__main__":
    r = prepare(sys.argv[1], sys.argv[2], sys.argv[3])
    n = sum(1 for v in r.values() for f in v.values() if f)
    print(len(r), "materials,", n, "texture files")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `py -m pytest tools/vegetation/tests/test_prepare_textures.py -q`
Expected: `2 passed`.

- [ ] **Step 5: Prepare the real textures**

Run: `py -m tools.vegetation.prepare_textures "C:\Users\Andrew\Desktop\Trees" "C:\Users\Andrew\Desktop\Trees\split\manifest.json" "C:\Users\Andrew\Desktop\Trees\split\textures"`
Expected: `24 materials, N texture files` with N ≤ 48. List which bark/log keys got Roughness/Metalness files (`py -c "import json;print({k:[s for s,f in v.items() if f] for k,v in json.load(open(r'C:\Users\Andrew\Desktop\Trees\split\textures\textures.json')).items()})"`) and record it in the commit message.

- [ ] **Step 6: Commit**

```bash
git add tools/vegetation/prepare_textures.py tools/vegetation/tests/test_prepare_textures.py
git commit -m "Vegetation: prepare shared textures and split packed spec maps"
```

---

### Task 5: MCP client and studio launcher

**Files:**
- Create: `tools/vegetation/mcp_client.py`
- Test: `tools/vegetation/tests/test_mcp_client.py`

**Interfaces:**
- Produces:
  ```python
  class McpError(RuntimeError): ...
  class Studio:
      def __init__(self, port: int, token: str, base: str = "http://127.0.0.1"): ...
      def initialize(self) -> dict                      # JSON-RPC "initialize", returns result
      def call(self, tool: str, **arguments) -> dict    # tools/call; returns structuredContent if present else {"text": ...}
      def lua(self, source: str) -> list[str]           # run_lua; returns output lines; raises McpError if error=true
      def wait_until_ready(self, timeout_s: float = 120) -> None   # polls initialize
  def launch_studio(exe: str, project_dir: str, port: int, token: str) -> subprocess.Popen
      # env ANARCHY_MCP_PORT / ANARCHY_MCP_TOKEN set; argv[1] = project_dir
  def copy_project(src_dir: str, dst_dir: str, new_name: str) -> None
      # shutil.copytree (dirs_exist_ok=False), then rewrites project.json "name"
  ```
  Wire format (from `src/ide/McpServer.cpp`): POST `/mcp`, headers `Authorization: Bearer <token>`, `Content-Type: application/json`, `Accept: application/json, text/event-stream`; body `{"jsonrpc":"2.0","id":N,"method":...,"params":...}`; reply JSON `{"result": {...}}` or `{"error": {...}}`. `tools/call` params `{"name": tool, "arguments": {...}}`; result has `content[0].text`, optional `structuredContent`, and `isError`. `run_lua` arguments `{"source": ...}`, its result `{"output": [...], "error": bool}`.

- [ ] **Step 1: Write the failing tests**

`tools/vegetation/tests/test_mcp_client.py`:
```python
import json, threading, pytest
from http.server import BaseHTTPRequestHandler, HTTPServer
from tools.vegetation.mcp_client import Studio, McpError, copy_project

class Fake(BaseHTTPRequestHandler):
    calls = []
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Fake.calls.append((self.headers.get("Authorization"), body))
        if self.headers.get("Authorization") != "Bearer tok":
            self.send_response(401); self.end_headers(); return
        if body["method"] == "initialize":
            result = {"protocolVersion": "2025-03-26", "serverInfo": {"name": "fake"}}
        else:
            args = body["params"]["arguments"]
            if body["params"]["name"] == "run_lua":
                bad = "error(" in args["source"]
                sc = {"output": ["boom"] if bad else ["hi"], "error": bad}
                result = {"content": [{"type": "text", "text": json.dumps(sc)}], "structuredContent": sc, "isError": False}
            elif body["params"]["name"] == "explode":
                result = {"content": [{"type": "text", "text": "it broke"}], "isError": True}
            else:
                result = {"content": [{"type": "text", "text": "plain"}], "isError": False}
        out = json.dumps({"jsonrpc": "2.0", "id": body["id"], "result": result}).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass

@pytest.fixture
def server():
    srv = HTTPServer(("127.0.0.1", 0), Fake)
    t = threading.Thread(target=srv.serve_forever, daemon=True); t.start()
    yield srv.server_port
    srv.shutdown()

def test_initialize_and_lua(server):
    s = Studio(server, "tok"); s.initialize()
    assert s.lua("print('hi')") == ["hi"]
    assert Fake.calls[0][0] == "Bearer tok"

def test_tool_error_raises(server):
    s = Studio(server, "tok")
    with pytest.raises(McpError, match="explode.*it broke"):
        s.call("explode")
    with pytest.raises(McpError, match="run_lua.*boom"):
        s.lua("error('x')")

def test_wait_until_ready_times_out():
    s = Studio(1, "tok")   # nothing listens on port 1
    with pytest.raises(McpError, match="not answering"):
        s.wait_until_ready(timeout_s=0.5)

def test_copy_project(tmp_path):
    src = tmp_path / "A"; (src / "src").mkdir(parents=True)
    (src / "project.json").write_text(json.dumps({"name": "A", "format": 1}))
    (src / "src" / "x.json").write_text("{}")
    copy_project(str(src), str(tmp_path / "B"), "B")
    assert json.load(open(tmp_path / "B" / "project.json"))["name"] == "B"
    assert (tmp_path / "B" / "src" / "x.json").exists()
    with pytest.raises(FileExistsError):
        copy_project(str(src), str(tmp_path / "B"), "B")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `py -m pytest tools/vegetation/tests/test_mcp_client.py -q`
Expected: FAIL, `No module named 'tools.vegetation.mcp_client'`.

- [ ] **Step 3: Implement `mcp_client.py`**

```python
"""Minimal JSON-RPC client for AnarchyStudio's MCP server, plus launch/copy helpers."""
import json, os, shutil, subprocess, time, urllib.request, urllib.error

class McpError(RuntimeError):
    pass

class Studio:
    def __init__(self, port: int, token: str, base: str = "http://127.0.0.1"):
        self.url = f"{base}:{port}/mcp"; self.token = token; self._id = 0

    def _rpc(self, method: str, params: dict, timeout: float = 600) -> dict:
        self._id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._id, "method": method, "params": params}).encode()
        req = urllib.request.Request(self.url, data=body, method="POST", headers={
            "Authorization": f"Bearer {self.token}", "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            reply = json.loads(r.read())
        if "error" in reply:
            raise McpError(f"{method}: {reply['error']}")
        return reply["result"]

    def initialize(self) -> dict:
        return self._rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                        "clientInfo": {"name": "vegetation-import", "version": "1"}})

    def wait_until_ready(self, timeout_s: float = 120) -> None:
        deadline = time.time() + timeout_s
        while True:
            try:
                self.initialize(); return
            except (urllib.error.URLError, ConnectionError, OSError, McpError):
                if time.time() > deadline:
                    raise McpError(f"studio not answering on {self.url} after {timeout_s}s")
                time.sleep(1)

    def call(self, tool: str, **arguments) -> dict:
        result = self._rpc("tools/call", {"name": tool, "arguments": arguments})
        text = "".join(c.get("text", "") for c in result.get("content", []))
        if result.get("isError"):
            raise McpError(f"{tool}: {text}")
        return result.get("structuredContent") or {"text": text}

    def lua(self, source: str) -> list:
        r = self.call("run_lua", source=source)
        if r.get("error"):
            raise McpError("run_lua failed: " + "\n".join(r.get("output", [])))
        return list(r.get("output", []))

def launch_studio(exe: str, project_dir: str, port: int, token: str) -> subprocess.Popen:
    env = dict(os.environ, ANARCHY_MCP_PORT=str(port), ANARCHY_MCP_TOKEN=token)
    return subprocess.Popen([exe, project_dir], env=env, cwd=os.path.dirname(exe))

def copy_project(src_dir: str, dst_dir: str, new_name: str) -> None:
    if os.path.exists(dst_dir):
        raise FileExistsError(dst_dir)
    shutil.copytree(src_dir, dst_dir)
    pj = os.path.join(dst_dir, "project.json")
    data = json.load(open(pj)); data["name"] = new_name
    with open(pj, "w") as f:
        json.dump(data, f, indent=2)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `py -m pytest tools/vegetation/tests/test_mcp_client.py -q`
Expected: `4 passed`.

- [ ] **Step 5: Live smoke against the real studio (manual, no project write)**

Run (PowerShell or bash, from repo root):
```bash
py -c "import sys; sys.path.insert(0,'.'); from tools.vegetation.mcp_client import *; p=launch_studio(r'build\Release\AnarchyStudio.exe', r'C:\Users\Andrew\Documents\Anarchy Engine Projects\Bounce', 8765, 'veg'); s=Studio(8765,'veg'); s.wait_until_ready(); print(s.lua('print(workspace.Name, #game.Assets.Prefabs:GetChildren())')); print(s.call('get_studio_info')); p.terminate()"
```
Expected: prints `['Workspace N']`-style output and a studio-info dict, then the studio closes. (Bounce is a small sample project; nothing is saved.)

- [ ] **Step 6: Commit**

```bash
git add tools/vegetation/mcp_client.py tools/vegetation/tests/test_mcp_client.py
git commit -m "Vegetation: MCP client and studio launcher"
```

---

### Task 6: Editor import, shared materials, verification, save

**Files:**
- Create: `tools/vegetation/fix_materials.luau`, `tools/vegetation/import_vegetation.py`
- Test: manual run with numeric checks (the editor is the system under test); unit test only for the batching helper.

**Interfaces:**
- Consumes: `Studio`, `launch_studio`, `copy_project` (Task 5); `split/manifest.json` (Task 3); `split/textures/textures.json` (Task 4).
- Produces: the project copy with assets; `split/import_report.json` with the verification numbers.
  ```python
  def batches(items: list, n: int = 64) -> list[list]
  def main(argv) -> int   # args: --project-src --project-dst --exe --split-dir [--port 8765] [--token veg]
  ```

Flow of `main`:
1. `copy_project(src, dst, "H1Z1 Z1 Vegetation")` (skip with a message if dst exists and `--reuse` given).
2. `launch_studio(exe, dst, port, token)`; `wait_until_ready`.
3. `s.call("import_assets", files=[...all files in split/textures except textures.json...])` → expect every row has no `error`; collect texture names.
4. For each batch of 64 OBJ paths (sorted by name): `s.call("import_assets", files=batch)`; every row must have `prefab` and no `error`; collect `left_out` notes; any note mentioning "texture" is a failure (means a map slipped through).
5. Read `fix_materials.luau`, substitute the `__TEXTURES_JSON__` token with the `textures.json` content as a Luau table literal (write a tiny Python converter: dict → `{["Key"]={Color="...",Normal=...}}`, `None` → `nil` is not storable in a table; emit only non-null fields), and `__MANIFEST_MATERIALS__` with `{["Oak-Old-Var01"]={"Bark_Trunk27","Leaf_Leaves3"},...}`. Run it with `s.lua(...)`. It prints one line per step and ends with `FIX_DONE <nMaterials> <nModelsAssigned> <nDeletedMaterials>`.
6. Verification Lua (also via `s.lua`): for every prefab under `Assets.Prefabs.Vegetation`, print `BOUNDS <name> <sx> <sy> <sz>`; print `TEXTURES <count>`; print `MATERIALS <count under Vegetation>`; print `STRAY <n>` = number of Materials outside `Assets.Materials.Vegetation` whose DiffuseTexture is one of the vegetation textures. Python compares each `BOUNDS` with the manifest's `bounds` size within 1% and writes `import_report.json`.
7. `s.call("save_place")`. Leave the studio open (print the port) for Task 7.

`fix_materials.luau` (the token substitution happens in Python):
```lua
local TEX = __TEXTURES_JSON__          -- {["Bark_Trunk27"]={Color="Bark_Trunk27_Color.png", Normal=..., Roughness=..., Metalness=...}, ...}
local ASSET_MATS = __MANIFEST_MATERIALS__   -- {["Oak-Old-Var01"]={"Bark_Trunk27","Leaf_Leaves3"}, ...}
local FAMILY = __MANIFEST_FAMILIES__        -- {["Oak-Old-Var01"]="Tree", ...}
local assets = game:GetService("Assets")
local history = game:GetService("ChangeHistoryService")
local rec = history:TryBeginRecording("Vegetation materials")

local function folder(parent, name)
	local f = parent:FindFirstChild(name)
	if f == nil then f = Instance.new("Folder", parent); f.Name = name end
	return f
end

-- 1. index imported textures by file stem (import names a Texture after the file's stem)
local texByName = {}
for _, t in ipairs(assets.Textures:GetDescendants()) do
	if t.ClassName == "Texture" then texByName[t.Name] = t end
end
local function tex(file)
	if file == nil then return nil end
	local stem = file:gsub("%.png$", "")
	local t = texByName[stem]
	if t == nil then error("texture not imported: " .. stem) end
	return t
end
local texFolder = folder(assets.Textures, "Vegetation")
for _, t in pairs(texByName) do
	if t.Name:match("^Leaf_") or t.Name:match("^Bark_") or t.Name:match("^Log_") then t.Parent = texFolder end
end

-- 2. shared materials
local matFolder = folder(assets.Materials, "Vegetation")
local shared = {}
local nMat = 0
for key, files in pairs(TEX) do
	local m = Instance.new("Material", matFolder)
	m.Name = key
	m.DiffuseTexture = tex(files.Color)
	m.NormalTexture = tex(files.Normal)
	m.Metalness = 0
	if key:match("^Leaf_") then
		m.Roughness = 0.85
	else
		m.Roughness = 0.9
		if files.Roughness then m.RoughnessTexture = tex(files.Roughness) end
		if files.Metalness then m.MetalnessTexture = tex(files.Metalness) end
	end
	shared[key] = m
	nMat += 1
end

-- 3. reassign every imported Model, move prefabs into family folders, delete per-asset material folders
local prefabRoot = folder(assets.Prefabs, "Vegetation")
local familyFolder = {}
for _, fam in ipairs({"Trees", "Bushes", "BerryBushes", "Cards", "GroundLogs"}) do familyFolder[fam] = folder(prefabRoot, fam) end
local famName = {Tree = "Trees", Bush = "Bushes", BerryBush = "BerryBushes", Card = "Cards", GroundLog = "GroundLogs"}
local nAssigned, nDeleted = 0, 0
for name, _ in pairs(ASSET_MATS) do
	local prefab = assets.Prefabs:FindFirstChild(name)
	if prefab == nil or prefab.ClassName ~= "Prefab" then error("prefab missing: " .. name) end
	for _, model in ipairs(prefab:GetChildren()) do
		local old = model.Material
		local key = old and old.Name or nil
		if shared[key] == nil then error(name .. ": model material " .. tostring(key) .. " has no shared material") end
		model.Material = shared[key]
		nAssigned += 1
	end
	prefab.Parent = familyFolder[famName[FAMILY[name]]]
	local oldFolder = assets.Materials:FindFirstChild(name)
	if oldFolder then oldFolder:Destroy(); nDeleted += 1 end
	local meshFolder = assets.Meshes:FindFirstChild(name)
	if meshFolder then meshFolder.Parent = folder(assets.Meshes, "Vegetation") end
end
if rec then history:FinishRecording(rec, Enum.FinishRecordingOperation.Commit) end
print("FIX_DONE", nMat, nAssigned, nDeleted)
```
`Enum.FinishRecordingOperation.Commit` and `.Cancel` are the members (`src/engine_core/ScriptBindings.cpp:1173`, `LuaApi.cpp:1789-1792`). Containment allows Folders under every asset category (`src/engine_core/Containment.cpp:121-126`), so the `Instance.new("Folder", parent)` calls are valid.

Verification Lua (`VERIFY_LUA` string constant in Python):
```lua
local assets = game:GetService("Assets")
local root = assets.Prefabs:FindFirstChild("Vegetation")
for _, p in ipairs(root:GetDescendants()) do
	if p.ClassName == "Prefab" then
		local s = p:GetBoundingBox()
		print("BOUNDS", p.Name, s.X, s.Y, s.Z, #p:GetChildren())
	end
end
local nTex = 0
for _, t in ipairs(assets.Textures:GetDescendants()) do if t.ClassName == "Texture" then nTex += 1 end end
print("TEXTURES", nTex)
local veg = assets.Materials:FindFirstChild("Vegetation")
print("MATERIALS", #veg:GetChildren())
local vegTex = {}
for _, t in ipairs(assets.Textures.Vegetation:GetChildren()) do vegTex[t] = true end
local stray = 0
for _, m in ipairs(assets.Materials:GetDescendants()) do
	if m.ClassName == "Material" and m.Parent ~= veg and m.DiffuseTexture and vegTex[m.DiffuseTexture] then stray += 1 end
end
print("STRAY", stray)
```

- [ ] **Step 1: Write the failing unit test for batching and the Lua table conversion**

Append to `tools/vegetation/tests/test_mcp_client.py`:
```python
from tools.vegetation.import_vegetation import batches, lua_table

def test_batches():
    assert batches(list(range(130)), 64) == [list(range(64)), list(range(64, 128)), [128, 129]]

def test_lua_table():
    assert lua_table({"A": {"Color": "a.png", "Normal": None}, "B": ["x", "y"]}) == \
        '{["A"]={["Color"]="a.png"},["B"]={"x","y"}}'
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -m pytest tools/vegetation/tests/test_mcp_client.py -q -k "batches or lua_table"`
Expected: FAIL, `No module named 'tools.vegetation.import_vegetation'`.

- [ ] **Step 3: Implement `import_vegetation.py`**

```python
"""Drives AnarchyStudio over MCP: copies the project, imports the split assets and
textures, runs fix_materials.luau, verifies, saves. Usage:
  py -m tools.vegetation.import_vegetation --project-src "...\\H1Z1 Z1" --project-dst "...\\H1Z1 Z1 Vegetation" ^
     --exe build\\Release\\AnarchyStudio.exe --split-dir "C:\\Users\\Andrew\\Desktop\\Trees\\split" [--reuse]"""
import argparse, glob, json, os, sys
from .mcp_client import Studio, McpError, launch_studio, copy_project

HERE = os.path.dirname(os.path.abspath(__file__))

def batches(items, n=64):
    return [items[i:i + n] for i in range(0, len(items), n)]

def lua_table(value) -> str:
    if isinstance(value, dict):
        parts = [f'["{k}"]={lua_table(v)}' for k, v in value.items() if v is not None]
        return "{" + ",".join(parts) + "}"
    if isinstance(value, list):
        return "{" + ",".join(lua_table(v) for v in value) + "}"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'

VERIFY_LUA = r'''
local assets = game:GetService("Assets")
local root = assets.Prefabs:FindFirstChild("Vegetation")
for _, p in ipairs(root:GetDescendants()) do
	if p.ClassName == "Prefab" then
		local s = p:GetBoundingBox()
		print("BOUNDS", p.Name, s.X, s.Y, s.Z, #p:GetChildren())
	end
end
local nTex = 0
for _, t in ipairs(assets.Textures:GetDescendants()) do if t.ClassName == "Texture" then nTex += 1 end end
print("TEXTURES", nTex)
local veg = assets.Materials:FindFirstChild("Vegetation")
print("MATERIALS", #veg:GetChildren())
local vegTex = {}
for _, t in ipairs(assets.Textures.Vegetation:GetChildren()) do vegTex[t] = true end
local stray = 0
for _, m in ipairs(assets.Materials:GetDescendants()) do
	if m.ClassName == "Material" and m.Parent ~= veg and m.DiffuseTexture and vegTex[m.DiffuseTexture] then stray += 1 end
end
print("STRAY", stray)
'''

def import_files(s: Studio, files):
    r = s.call("import_assets", files=files)
    rows = r.get("files", [])
    bad = [row for row in rows if row.get("error")]
    if bad or r.get("failed"):
        raise McpError("import_assets failed: " + json.dumps(bad)[:2000])
    notes = [n for row in rows for n in row.get("left_out", [])]
    if any("texture" in n.lower() for n in notes):
        raise McpError("importer looked for textures: " + "; ".join(notes)[:2000])
    return rows

def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--project-src", required=True); ap.add_argument("--project-dst", required=True)
    ap.add_argument("--exe", required=True); ap.add_argument("--split-dir", required=True)
    ap.add_argument("--port", type=int, default=8765); ap.add_argument("--token", default="veg")
    ap.add_argument("--reuse", action="store_true")
    a = ap.parse_args(argv)
    if not (a.reuse and os.path.exists(a.project_dst)):
        copy_project(a.project_src, a.project_dst, "H1Z1 Z1 Vegetation")
    proc = launch_studio(os.path.abspath(a.exe), a.project_dst, a.port, a.token)
    s = Studio(a.port, a.token); s.wait_until_ready()
    manifest = json.load(open(os.path.join(a.split_dir, "manifest.json")))
    textures = json.load(open(os.path.join(a.split_dir, "textures", "textures.json")))
    tex_files = sorted(glob.glob(os.path.join(a.split_dir, "textures", "*.png")))
    import_files(s, tex_files)
    print("textures imported:", len(tex_files))
    objs = sorted(glob.glob(os.path.join(a.split_dir, "*.obj")))
    for b in batches(objs, 64):
        rows = import_files(s, b)
        print("prefabs imported:", len(rows))
    lua = open(os.path.join(HERE, "fix_materials.luau")).read()
    lua = lua.replace("__TEXTURES_JSON__", lua_table(textures))
    lua = lua.replace("__MANIFEST_MATERIALS__", lua_table({e["name"]: e["materials"] for e in manifest}))
    lua = lua.replace("__MANIFEST_FAMILIES__", lua_table({e["name"]: e["family"] for e in manifest}))
    out = s.lua(lua)
    done = [l for l in out if l.startswith("FIX_DONE")]
    if not done:
        raise McpError("fix pass did not finish:\n" + "\n".join(out[-20:]))
    print(done[0])
    lines = s.lua(VERIFY_LUA)
    report = {"fix": done[0], "bounds_mismatch": [], "textures": None, "materials": None, "stray": None}
    size_of = {e["name"]: [e["bounds"][i + 3] - e["bounds"][i] for i in range(3)] for e in manifest}
    for l in lines:
        p = l.split()
        if p[0] == "BOUNDS":
            name, got = p[1], [float(x) for x in p[2:5]]
            exp = size_of[name]
            if any(abs(g - e) > 0.01 * max(e, 0.05) + 0.02 for g, e in zip(got, exp)):
                report["bounds_mismatch"].append({"name": name, "got": got, "expected": exp})
        elif p[0] == "TEXTURES": report["textures"] = int(p[1])
        elif p[0] == "MATERIALS": report["materials"] = int(p[1])
        elif p[0] == "STRAY": report["stray"] = int(p[1])
    report["prefabs"] = sum(1 for l in lines if l.startswith("BOUNDS"))
    json.dump(report, open(os.path.join(a.split_dir, "import_report.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in report.items() if k != "bounds_mismatch"}), "mismatches:", len(report["bounds_mismatch"]))
    ok = (report["prefabs"] == len(manifest) and report["materials"] == len(textures) and report["textures"] <= 48
          and report["stray"] == 0 and not report["bounds_mismatch"])
    if ok:
        print(s.call("save_place")); print("saved; studio left running on port", a.port)
    else:
        print("NOT SAVED: verification failed, see import_report.json")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
```
- [ ] **Step 4: Run unit tests**

Run: `py -m pytest tools/vegetation/tests -q`
Expected: all pass.

- [ ] **Step 5: Dry-run the Lua on a scratch project first**

Before touching the H1Z1 copy, run the whole flow against a throwaway copy of `Bounce` with only three split assets (copy `Oak-Old-Var01`, `Bush-Oak-Var01`, `GroundLog-Var01` OBJ/MTL plus a trimmed `manifest.json` and the full `textures` folder into `C:\Users\Andrew\Desktop\Trees\split_mini\`):
```bash
py -m tools.vegetation.import_vegetation --project-src "C:\Users\Andrew\Documents\Anarchy Engine Projects\Bounce" --project-dst "C:\Users\Andrew\Documents\Anarchy Engine Projects\_veg_dryrun" --exe build\Release\AnarchyStudio.exe --split-dir "C:\Users\Andrew\Desktop\Trees\split_mini"
```
Expected: `FIX_DONE 24 4 3`, report `prefabs 3, materials 24, stray 0, mismatches 0`, then `saved`. Fix any Luau API mismatch here (enum names, property names) before the real run. Delete `_veg_dryrun` afterwards and close that studio (`taskkill /IM AnarchyStudio.exe /F` only if no other studio is open; otherwise use the Popen handle).

- [ ] **Step 6: Run for real**

```bash
py -m tools.vegetation.import_vegetation --project-src "C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1" --project-dst "C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1 Vegetation" --exe build\Release\AnarchyStudio.exe --split-dir "C:\Users\Andrew\Desktop\Trees\split"
```
Expected: `textures imported: ≤48`, three `prefabs imported` lines (64, 64, 4), `FIX_DONE 24 <n> 132`, report with `prefabs 132, materials 24, textures ≤ 48, stray 0, mismatches 0`, `saved`. Confirm on disk: `ls "C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1 Vegetation\src\Assets.assets\Prefabs.prefabs\Vegetation*"` shows the five family folders, and the original project's `src` tree is byte-identical to before (`git -C "...\H1Z1 Z1" status` if it is a git repo, else compare `find src -type f | wc -l` before and after: 107).

- [ ] **Step 7: Commit**

```bash
git add tools/vegetation/import_vegetation.py tools/vegetation/fix_materials.luau tools/vegetation/tests/test_mcp_client.py
git commit -m "Vegetation: import split assets and build shared materials in the editor"
```
Include the report numbers in the commit body.

---

### Task 7: Gallery screenshots and final check

**Files:**
- Create: `tools/vegetation/gallery.luau`, `tools/vegetation/gallery.py`

**Interfaces:**
- Consumes: `Studio` (Task 5), `manifest.json`, the running studio from Task 6 (or relaunch with `launch_studio` on the copy).
- Produces: `C:\Users\Andrew\Desktop\Trees\split\gallery\<row>.png` for each species row and each bush family; the place saved with the gallery removed.

`gallery.luau` takes `__ROW__` (a Luau list of prefab names) and `__ORIGIN__` (`{x, y, z}`): it creates a `Folder` named `VegetationGallery` in Workspace, raycasts down at each slot to find ground, and places one `GameObject` per name 1.2 × its manifest crown diameter apart along +X (pass `__SPACING__` as a list of per-name spacings computed in Python from `crownRadius`), then moves `workspace.CurrentCamera.Transform` to look at the row from 1.5 × row length away and 0.35 × row length up, and prints `GALLERY_READY <rowLengthUnits>`.

```lua
local NAMES = __ROW__
local SPACING = __SPACING__
local origin = Vector3.new(__ORIGIN__)
local assets = game:GetService("Assets")
local root = assets.Prefabs.Vegetation
local function findPrefab(name)
	for _, p in ipairs(root:GetDescendants()) do
		if p.ClassName == "Prefab" and p.Name == name then return p end
	end
	error("no prefab " .. name)
end
local old = workspace:FindFirstChild("VegetationGallery")
if old then old:Destroy() end
local gallery = Instance.new("Folder", workspace); gallery.Name = "VegetationGallery"
local x = 0
local params = RaycastParams.new()
params.FilterType = Enum.RaycastFilterType.Include
params.FilterDescendantsInstances = {workspace:FindFirstChild("Z1")}
for i, name in ipairs(NAMES) do
	x += SPACING[i] / 2
	local hit = workspace:Raycast(origin + Vector3.new(x, 200, 0), Vector3.new(0, -400, 0), params)
	local y = hit and hit.Position.Y or origin.Y
	local go = Instance.new("GameObject", gallery)
	go.Name = name
	go.Prefab = findPrefab(name)
	go.Transform = Matrix4.new(origin.X + x, y, origin.Z)
	x += SPACING[i] / 2
end
local cam = workspace.CurrentCamera
local mid = origin + Vector3.new(x / 2, 0, 0)
local eye = mid + Vector3.new(0, 0.35 * x + 4, 1.5 * x + 10)
cam.Transform = Matrix4.lookAt(eye, mid + Vector3.new(0, 0.075 * x, 0))
print("GALLERY_READY", x)
```
API names verified on 2026-10-13: `Matrix4.new(x, y, z)` (position only), `Matrix4.lookAt(at, target, up?)` with up defaulting to +Y (`src/engine_core/LuaApi.cpp:890-915`), `Enum.RaycastFilterType.Include` (`src/engine_core/RaycastBindings.cpp:251,286`), `Instance.new(class)` then `.Parent =` as in `scripts/stress_place.lua:68-76` (the two-argument parent form also exists in `ScriptBindings.cpp`).

`gallery.py`:
```python
"""Places each species row / bush family beside the house, screenshots it, then removes the gallery and saves."""
import base64, json, os, sys
from .mcp_client import Studio
from .import_vegetation import lua_table
HERE = os.path.dirname(os.path.abspath(__file__))
ORIGIN = [1010.0, 42.0, -660.0]   # flat ground east of the House brushes at (990, 41.75, -694)

def rows(manifest):
    out = {}
    for e in manifest:
        key = e["species"] if e["family"] == "Tree" else e["name"].rsplit("-Var", 1)[0]
        out.setdefault(key, []).append(e)
    order = {"Old": 0, "Adult": 1, "Young": 2, "Dead": 3}
    for k, es in out.items():
        es.sort(key=lambda e: (order.get(e["age"], 9), e["name"]))
    return out

def main(split_dir, port=8765, token="veg"):
    s = Studio(port, token); s.initialize()
    manifest = json.load(open(os.path.join(split_dir, "manifest.json")))
    lua = open(os.path.join(HERE, "gallery.luau")).read()
    out_dir = os.path.join(split_dir, "gallery"); os.makedirs(out_dir, exist_ok=True)
    for key, es in rows(manifest).items():
        names = [e["name"] for e in es]
        spacing = [max(2.4 * e["crownRadius"], 2.0) for e in es]
        src = lua.replace("__ROW__", lua_table(names)).replace("__SPACING__", lua_table(spacing)) \
                 .replace("__ORIGIN__", ",".join(repr(v) for v in ORIGIN))
        print(key, s.lua(src)[-1])
        shot = s.call("screenshot", max_size=1600)
        png = base64.b64decode(shot["image_content"]["data"])   # McpTools.cpp Screenshot(): {width, height, image_content: {data, mimeType}}
        open(os.path.join(out_dir, f"{key}.png"), "wb").write(png)
    s.lua('local g = workspace:FindFirstChild("VegetationGallery"); if g then g:Destroy() end; print("CLEANED")')
    print(s.call("save_place"))

if __name__ == "__main__":
    main(sys.argv[1], *(int(sys.argv[2]),) if len(sys.argv) > 2 else ())
```
The `screenshot` result shape is verified: `{"width", "height", "image_content": {"data": <base64 PNG>, "mimeType": "image/png"}}` (`src/ide/McpTools.cpp:1611-1625`, member name `kImageMember = "image_content"` in `McpServer.hpp:105`).

- [ ] **Step 1: Run the gallery against the open studio**

Run: `py -m tools.vegetation.gallery "C:\Users\Andrew\Desktop\Trees\split"`
Expected: one `GALLERY_READY` line per row (8 species + Bush-Sycamore, Bush-Oak, BerryBush, Card, GroundLog = 13), 13 PNGs in `split\gallery\`, `CLEANED`, and a save result. Open two PNGs with `Start-Process` (Windows viewer) and check: trunks stand on the ground, leaves are visible from the camera side, nothing is white or black (textures loaded), dead trees have no leaves. If leaves show only from one side, that family lacks flipped copies: add a `--double-sided` option to `write_asset` that appends each face reversed, re-split that family, and re-run Task 6 with `--reuse` after deleting those prefabs (`delete_instance` on the family folder first).

- [ ] **Step 2: Final numeric check and close**

Run: `py -c "import json;r=json.load(open(r'C:\Users\Andrew\Desktop\Trees\split\import_report.json'));print(r['prefabs'],r['materials'],r['textures'],r['stray'],len(r['bounds_mismatch']))"`
Expected: `132 24 <=48 0 0`. Confirm `workspace` has no `VegetationGallery` folder on disk: `ls "C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1 Vegetation\src\Workspace.workspace" | grep -i gallery` prints nothing. Close the studio via the Popen handle or ask the user.

- [ ] **Step 3: Commit and hand the screenshots to the user**

```bash
git add tools/vegetation/gallery.luau tools/vegetation/gallery.py
git commit -m "Vegetation: gallery screenshots for the imported assets"
```
Send the 13 PNGs with `SendUserFile`, with the report numbers in the caption. Remind the user the assets are saved in the copy project only.
