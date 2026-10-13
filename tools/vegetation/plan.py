"""Builds the vegetation plan from the terrain layers: structure first, trees second.

1. Forest potential (wetness, soil, sun, distance from people, existing woodland,
   low-frequency noise, a ragged frontier near the map edge), thresholded by
   quantile so forest covers a chosen share of usable land.
2. Placement categories: interior, fringe, hedgerows (Voronoi parcel lines, field
   edges, roadsides), riparian, village, lone trees, orchards.
3. Species by niche x patch noise, ages by history, crown-radius Poisson spacing,
   a hard budget, dead trees, then undergrowth from the same model.

Writes plan.bin (32-byte records), plan.json (assets, params, stats) and
canopy.npz (rasters for the review page).

Usage: py -m tools.vegetation.plan <plan_dir> <manifest.json> [config.json]
"""
import json
import math
import os
import struct
import sys
import time
from collections import Counter, defaultdict

import numpy as np
from scipy import ndimage as ndi

from .layers import G, ORIGIN, lowfreq_noise, norm

DEFAULTS = {
    "seed": 13,
    "tree_budget": 100_000,
    "forest_share": 0.42,          # of usable land
    "bushes": 30_000, "saplings": 15_000, "berry_bushes": 3_000, "logs": 5_000,
    "dead_share": 0.04,
    "cherry_share": 0.012, "apple_share": 0.04, "hawthorn_share": 0.10,
    "orchards": 14,
    "region_bias": [],             # e.g. [{"x0":-2048,"x1":2048,"z0":800,"z1":2048,"factor":0.6}] (south = +z)
}
CATS = ["interior", "fringe", "hedgerow", "riparian", "village", "lone", "orchard"]
KINDS = ["tree", "dead", "bush", "sapling", "berry", "log"]
REC = struct.Struct("<7fHBB")      # x y z yaw tiltX tiltZ scale | asset | category | kind


class Hash:
    """Spatial hash of accepted discs for Poisson-disk acceptance."""

    def __init__(self, cell=10.0):
        self.cell, self.b = cell, defaultdict(list)

    def ok(self, x, z, r, k, rmax):
        reach = (r + rmax) * k
        c = self.cell
        i0, i1 = int((x - reach) // c), int((x + reach) // c)
        j0, j1 = int((z - reach) // c), int((z + reach) // c)
        for i in range(i0, i1 + 1):
            for j in range(j0, j1 + 1):
                for (px, pz, pr) in self.b.get((i, j), ()):
                    d = (r + pr) * k
                    if (px - x) ** 2 + (pz - z) ** 2 < d * d:
                        return False
        return True

    def add(self, x, z, r):
        self.b[(int(x // self.cell), int(z // self.cell))].append((x, z, r))

    def near(self, x, z, radius):
        c = self.cell
        for i in range(int((x - radius) // c), int((x + radius) // c) + 1):
            for j in range(int((z - radius) // c), int((z + radius) // c) + 1):
                yield from self.b.get((i, j), ())


def main(plan_dir, manifest_path, config_path=None):
    t0 = time.time()
    P = dict(DEFAULTS)
    if config_path:
        P.update(json.load(open(config_path)))
    rng = np.random.default_rng(P["seed"])
    L = dict(np.load(os.path.join(plan_dir, "layers.npz")))
    H1 = np.load(os.path.join(plan_dir, "surface.npz"))["height"]
    manifest = json.load(open(manifest_path))
    n = L["height"].shape[0]

    def log(msg):
        print(f"[{time.time() - t0:5.1f}s] {msg}", flush=True)

    # ------------------------------------------------------------ assets
    assets = [e["name"] for e in manifest]
    A = {e["name"]: e for e in manifest}
    aid = {name: i for i, name in enumerate(assets)}
    by_sa = defaultdict(list)
    for e in manifest:
        if e["family"] == "Tree":
            by_sa[(e["species"], e["age"])].append(e["name"])
    species = sorted({e["species"] for e in manifest if e["family"] == "Tree"})
    fam = defaultdict(list)
    for e in manifest:
        fam[e["name"].rsplit("-Var", 1)[0]].append(e["name"])

    def variant(sp, age):
        for a in (age, "Adult", "Old", "Young"):
            if by_sa.get((sp, a)):
                return by_sa[(sp, a)][rng.integers(len(by_sa[(sp, a)]))]
        raise KeyError(sp)

    # ------------------------------------------------------------ helpers
    def cell_of(x, z):
        return (min(n - 1, max(0, int((z - ORIGIN) / G))), min(n - 1, max(0, int((x - ORIGIN) / G))))

    def ground(x, z):
        cx, cz = x - ORIGIN, z - ORIGIN
        i, j = int(cz), int(cx)
        i = min(max(i, 0), H1.shape[0] - 2)
        j = min(max(j, 0), H1.shape[1] - 2)
        fx, fz = cx - j, cz - i
        h00, h01, h10, h11 = H1[i, j], H1[i, j + 1], H1[i + 1, j], H1[i + 1, j + 1]
        return float(h00 * (1 - fx) * (1 - fz) + h01 * fx * (1 - fz) + h10 * (1 - fx) * fz + h11 * fx * fz)

    def sample_cells(mask, count):
        idx = np.flatnonzero(mask.ravel())
        if idx.size == 0 or count <= 0:
            return np.empty((0, 2))
        pick = idx[rng.integers(idx.size, size=int(count))]
        r, c = np.divmod(pick, n)
        x = ORIGIN + (c + rng.random(pick.size)) * G
        z = ORIGIN + (r + rng.random(pick.size)) * G
        return np.stack([x, z], 1)

    # ------------------------------------------------------------ 1. forest potential
    paved = L["road"] | L["lots"]
    dist_paved = np.minimum(L["dist_road"], L["dist_lots"])
    usable = ~L["forbidden"] & (dist_paved > 6) & (L["dist_objects"] > 8) & (L["dist_water"] > 3)
    noise_big = lowfreq_noise((n, n), 300 / G, P["seed"] + 1)
    noise_mid = lowfreq_noise((n, n), 90 / G, P["seed"] + 2)
    away = norm(np.minimum(L["dist_human"], 500))
    sun_ok = 1 - np.abs(L["sun"] - 0.55) * 1.6
    F = (0.22 * L["wet"] + 0.12 * L["soil"] + 0.08 * sun_ok + 0.22 * away
         + 0.30 * norm(L["woodland"]) + 0.42 * noise_big + 0.15 * noise_mid
         - 0.30 * ndi.uniform_filter(L["frac_GrassField"], 15) - 0.25 * L["erosion"])
    # ragged frontier: the mountain ring must not be one solid wall
    edge_w = np.clip(1 - L["dist_edge"] / 420, 0, 1)
    F += edge_w * (lowfreq_noise((n, n), 60 / G, P["seed"] + 3) - 0.55) * 0.9
    rows, cols = np.mgrid[0:n, 0:n]
    wx, wz = ORIGIN + (cols + 0.5) * G, ORIGIN + (rows + 0.5) * G
    for b in P["region_bias"]:
        inside = (wx >= b["x0"]) & (wx < b["x1"]) & (wz >= b["z0"]) & (wz < b["z1"])
        F = np.where(inside, F * b["factor"] + (b["factor"] - 1) * 0.3, F)
    F = ndi.gaussian_filter(F, 40 / G)            # coherent forests, not salt-and-pepper
    q = np.quantile(F[usable], 1 - P["forest_share"])
    forest = usable & (F >= q)
    forest = ndi.binary_closing(ndi.binary_opening(forest, iterations=1), iterations=2) & usable
    lab, nl = ndi.label(forest)
    sizes = ndi.sum(forest, lab, index=np.arange(1, nl + 1))
    forest &= np.concatenate([[False], sizes >= 120])[lab]
    depth = ndi.distance_transform_edt(forest) * G           # units inside the forest
    open_land = usable & ~forest
    log(f"forest {forest.sum() / usable.sum():.1%} of usable land, {nl} patches")

    # ------------------------------------------------------------ 2. categories
    cat = np.full((n, n), -1, np.int8)
    cat[forest & (depth > 16)] = 0
    cat[forest & (depth <= 16)] = 1
    # Voronoi parcels over the whole map; their borders become tree lines in open land
    seeds = np.zeros((n, n), bool)
    pts = sample_cells(np.ones((n, n), bool), int(n * n * G * G / (230 ** 2)))
    for x, z in pts:
        seeds[cell_of(x, z)] = True
    _, (ir, ic) = ndi.distance_transform_edt(~seeds, return_indices=True)
    parcel = ir * n + ic
    border = np.zeros((n, n), bool)
    border[:, 1:] |= parcel[:, 1:] != parcel[:, :-1]
    border[1:, :] |= parcel[1:, :] != parcel[:-1, :]
    # keep about 60% of parcel borders, chosen per border (pair of parcels), so fields stay open
    pair = np.minimum(parcel, np.roll(parcel, 1, 1)) * 7919 + np.maximum(parcel, np.roll(parcel, 1, 1))
    keep_line = (np.sin(pair * 12.9898) * 43758.5453 % 1) < 0.6
    lines = border & keep_line
    field_edge = L["field"] & ~ndi.binary_erosion(L["field"], iterations=1)
    field_edge &= noise_mid > 0.35
    roadside = (L["dist_road"] >= 8) & (L["dist_road"] <= 13) & (lowfreq_noise((n, n), 50 / G, P["seed"] + 4) > 0.5)
    hedge = ndi.binary_dilation(lines | field_edge, iterations=1) | roadside
    hedge &= open_land & (L["dist_edge"] > 120)
    cat[hedge] = 2
    riparian = open_land & ~hedge & ((L["dist_stream"] <= 14) | ((L["dist_water"] > 3) & (L["dist_water"] <= 22)))
    riparian &= noise_mid > 0.3
    cat[riparian] = 3
    village = open_land & ~hedge & (L["dist_lots"] > 10) & (L["dist_lots"] < 70) & (noise_mid > 0.45)
    cat[village] = 4
    cat[open_land & (cat < 0)] = 5
    log("categories: " + ", ".join(f"{CATS[i]} {(cat == i).sum() * G * G / 1e6:.2f}M" for i in range(6)))

    # ------------------------------------------------------------ 3. species niches
    elev = norm(L["height"])
    north = np.clip(-L["northness"], 0, 1)        # gz < 0 slopes face north (-Z)
    ridge = norm(L["tpi_broad"])
    valley = 1 - ridge
    wet, soil, sun = L["wet"], L["soil"], L["sun"]
    near_road = np.clip(1 - L["dist_road"] / 60, 0, 1)
    near_town = np.clip(1 - L["dist_lots"] / 250, 0, 1)
    edge = np.clip(1 - depth / 40, 0, 1) * forest + open_land * 0.6
    niche = {
        "Spruce": 0.6 * elev + 0.5 * north + 0.3 * wet + 0.2 * (1 - soil),
        "Fir": 0.7 * elev + 0.4 * north + 0.2 * wet + 0.1 * valley,
        "ScotsPine": 0.6 * (1 - wet) + 0.4 * (1 - soil) + 0.4 * ridge + 0.3 * sun,
        "Oak": 0.6 * wet * valley + 0.5 * soil + 0.3 * (1 - elev) + 0.2 * sun,
        "Sycamore": 0.6 * wet + 0.4 * soil + 0.4 * np.clip(1 - L["dist_stream"] / 60, 0, 1),
        "Hawthorn": 0.5 * edge + 0.4 * near_road + 0.3 * sun,
        "Apple": 0.9 * near_town + 0.2 * sun,
        "Cherry": 1.0 * near_town * sun,
    }
    patch = {sp: lowfreq_noise((n, n), 160 / G, P["seed"] + 20 + i) for i, sp in enumerate(species)}
    weight = {sp: 1.0 for sp in species}
    cat_mult = {2: {"Hawthorn": 3.0, "Oak": 1.4}, 3: {"Sycamore": 2.2, "Oak": 1.4},
                4: {"Apple": 3.0, "Cherry": 2.0, "Oak": 1.4}, 5: {"Oak": 1.5, "ScotsPine": 1.2}}

    def species_at(rc_list, cats):
        sp_scores = np.stack([np.clip(niche[sp][rc_list[:, 0], rc_list[:, 1]], 0, None) ** 2
                              * (0.25 + patch[sp][rc_list[:, 0], rc_list[:, 1]]) ** 2 * weight[sp]
                              for sp in species], 1)
        for c, mult in cat_mult.items():
            m = cats == c
            for sp, f in mult.items():
                sp_scores[m, species.index(sp)] *= f
        sp_scores = sp_scores ** 2.0 + 1e-9
        p = sp_scores / sp_scores.sum(1, keepdims=True)
        u = rng.random(len(p))[:, None]
        return np.argmax(np.cumsum(p, 1) > u, 1)

    # ------------------------------------------------------------ 4. tree candidates
    density = {0: 1 / 14, 1: 1 / 20, 2: 1 / 10, 3: 1 / 40, 4: 1 / 900, 5: 1 / 9000}   # per sq unit
    spacing = {0: 0.62, 1: 0.72, 2: 0.50, 3: 0.80, 4: 1.3, 5: 2.5, 6: 1.0}
    cand = []
    for c, dens in density.items():
        m = cat == c
        pts = sample_cells(m, m.sum() * G * G * dens)
        cand.append((pts, np.full(len(pts), c)))
    pts = np.concatenate([p for p, _ in cand])
    ccat = np.concatenate([c for _, c in cand])
    rc = np.array([cell_of(x, z) for x, z in pts]) if len(pts) else np.empty((0, 2), int)
    log(f"{len(pts)} tree candidates")

    # species share caps: two passes adjusting the weights of capped species
    caps = {"Cherry": P["cherry_share"], "Apple": P["apple_share"], "Hawthorn": P["hawthorn_share"]}
    for _ in range(3):
        sidx = species_at(rc, ccat)
        share = np.bincount(sidx, minlength=len(species)) / len(sidx)
        for sp, cap in caps.items():
            s = share[species.index(sp)]
            if s > cap:
                weight[sp] *= (cap / s) ** 0.7
    sidx = species_at(rc, ccat)

    # ages: old where spared historically, young where land was reclaimed
    core = np.clip(depth[rc[:, 0], rc[:, 1]] / 80, 0, 1)
    p_old = np.select([ccat == 0, ccat == 1, ccat == 2, ccat == 3, ccat >= 4],
                      [0.12 + 0.30 * core, 0.10, 0.25, 0.45, 0.85])
    p_young = np.select([ccat == 0, ccat == 1, ccat == 2, ccat == 3, ccat >= 4],
                        [0.20 + 0.15 * (1 - core), 0.40, 0.25 + 0.35 * near_road[rc[:, 0], rc[:, 1]], 0.15, 0.03])
    u = rng.random(len(rc))
    ages = np.where(u < p_old, "Old", np.where(u > 1 - p_young, "Young", "Adult"))

    # priority: deliberate categories first, then old trees before young
    age_rank = {"Old": 0, "Adult": 1, "Young": 2}
    cat_rank = np.array([4, 5, 1, 3, 2, 0, 0])
    order = np.lexsort((rng.random(len(rc)), [age_rank[a] for a in ages], cat_rank[ccat]))

    trees = []        # dict rows
    hsh = Hash()
    rmax = max(e["crownRadius"] for e in manifest if e["family"] == "Tree") * 1.12

    def try_tree(x, z, name, c, k, scale):
        e = A[name]
        r = e["crownRadius"] * scale
        i, j = cell_of(x, z)
        if L["forbidden"][i, j] or dist_paved[i, j] < r + 1.5 or L["dist_objects"][i, j] < r + 2:
            return False
        if not hsh.ok(x, z, max(r, 1.0), k, rmax):
            return False
        hsh.add(x, z, max(r, 1.0))
        trees.append({"x": x, "z": z, "asset": name, "cat": c, "scale": scale, "r": r})
        return True

    # orchards first: rows beside towns, about 15% of the trees missing
    orch_sites = 0
    cand_sites = sample_cells(open_land & (L["dist_lots"] > 20) & (L["dist_lots"] < 160) & (L["slope"] < 9), 400)
    for x0, z0 in cand_sites:
        if orch_sites >= P["orchards"]:
            break
        ang = rng.uniform(0, math.pi)
        ca, sa = math.cos(ang), math.sin(ang)
        rows_n, cols_n = rng.integers(4, 8), rng.integers(6, 11)
        spots = [(x0 + (cc * 9) * ca - (rr * 10) * sa, z0 + (cc * 9) * sa + (rr * 10) * ca)
                 for rr in range(rows_n) for cc in range(cols_n)]
        if not all(open_land[cell_of(x, z)] for x, z in spots):
            continue
        orch_sites += 1
        for x, z in spots:
            if rng.random() < 0.15:
                continue
            try_tree(x, z, variant("Apple", "Adult" if rng.random() < 0.7 else "Old"), 6, 1.0, rng.uniform(0.92, 1.05))
    log(f"{orch_sites} orchards, {len(trees)} orchard trees")

    for o in order:
        x, z = pts[o]
        c = int(ccat[o])
        sp = species[sidx[o]]
        name = variant(sp, ages[o])
        scale = rng.uniform(0.9, 1.1) if c < 4 else rng.uniform(1.0, 1.12)
        try_tree(x, z, name, c, spacing[c], scale)
    log(f"{len(trees)} trees accepted before the budget")

    # ------------------------------------------------------------ 5. budget, then dead trees
    budget = P["tree_budget"]
    if len(trees) > budget:
        # thin closed canopy first, small/young first; keep tree lines, lone and village trees
        weight_rm = np.array([{0: 1.0, 1: 0.6, 3: 0.4}.get(t["cat"], 0.0) for t in trees])
        weight_rm *= np.array([1.6 if A[t["asset"]]["age"] == "Young" else 1.0 for t in trees])
        weight_rm *= 1.0 / (0.5 + np.array([t["r"] for t in trees]) / 6)
        need = len(trees) - budget
        p = weight_rm / weight_rm.sum()
        drop = set(rng.choice(len(trees), size=need, replace=False, p=p).tolist())
        trees = [t for i, t in enumerate(trees) if i not in drop]
        hsh = Hash()
        for t in trees:
            hsh.add(t["x"], t["z"], max(t["r"], 1.0))
    log(f"{len(trees)} trees after the budget")

    # suppressed trees die: centre under a bigger neighbour's crown, mostly in interiors
    n_dead = int(len(trees) * P["dead_share"])
    supp = np.zeros(len(trees))
    for i, t in enumerate(trees):
        if t["cat"] == 6:
            continue
        for (px, pz, pr) in hsh.near(t["x"], t["z"], t["r"] + rmax):
            if pr > t["r"] * 1.15 and (px - t["x"]) ** 2 + (pz - t["z"]) ** 2 < pr * pr:
                supp[i] += 1
        supp[i] = supp[i] * {0: 1.0, 1: 0.6}.get(t["cat"], 0.15) + rng.random() * 0.6
    for i in np.argsort(-supp)[:n_dead]:
        t = trees[i]
        sp = A[t["asset"]]["species"]
        t["asset"] = f"{sp}-Dead"
        t["kind"] = 1
    log(f"{n_dead} dead trees")

    # ------------------------------------------------------------ 6. canopy rasters
    canopy = np.zeros((n, n), np.float32)
    for t in trees:
        if t.get("kind") == 1:
            continue
        i, j = cell_of(t["x"], t["z"])
        rr = int(math.ceil(t["r"] / G))
        i0, i1, j0, j1 = max(0, i - rr), min(n, i + rr + 1), max(0, j - rr), min(n, j + rr + 1)
        yy, xx = np.mgrid[i0:i1, j0:j1]
        d2 = ((yy + 0.5) * G + ORIGIN - t["z"]) ** 2 + ((xx + 0.5) * G + ORIGIN - t["x"]) ** 2
        canopy[i0:i1, j0:j1] += (d2 <= t["r"] ** 2)
    canopy = np.minimum(canopy, 3)
    stand = ndi.uniform_filter((canopy > 0).astype(np.float32), 2 * int(35 / G) + 1)   # ~35 unit radius

    # ------------------------------------------------------------ 7. undergrowth
    under = []
    uhash = Hash(5.0)
    trunk_hash = Hash(10.0)
    for t in trees:
        trunk_hash.add(t["x"], t["z"], A[t["asset"]]["trunkRadius"] * t["scale"])

    def free_spot(x, z, r, k=1.0):
        i, j = cell_of(x, z)
        if L["forbidden"][i, j] or dist_paved[i, j] < r + 1 or L["dist_objects"][i, j] < r + 2:
            return False
        for (px, pz, pr) in trunk_hash.near(x, z, r + 3):
            if (px - x) ** 2 + (pz - z) ** 2 < (pr + r * 0.6 + 0.6) ** 2:
                return False
        return uhash.ok(x, z, r, k, 3.5)

    def nearest_species(x, z, radius=30):
        best, bd = None, 1e18
        for t in trees_near(x, z, radius):
            d = (t["x"] - x) ** 2 + (t["z"] - z) ** 2
            if d < bd:
                best, bd = t, d
        return A[best["asset"]]["species"] if best else None

    tree_grid = defaultdict(list)
    for t in trees:
        tree_grid[(int(t["x"] // 30), int(t["z"] // 30))].append(t)

    def trees_near(x, z, radius):
        for a in range(int((x - radius) // 30), int((x + radius) // 30) + 1):
            for b in range(int((z - radius) // 30), int((z + radius) // 30) + 1):
                yield from tree_grid.get((a, b), ())

    def place(kind, name, x, z, scale, c):
        r = max(A[name]["crownRadius"] * scale, 0.6)
        if not free_spot(x, z, r):
            return False
        uhash.add(x, z, r)
        under.append({"x": x, "z": z, "asset": name, "cat": c, "scale": scale, "r": r, "kind": kind})
        return True

    # bushes: heavy on fringes, tree lines and light woods; sparse under closed canopy; thickets
    thicket = lowfreq_noise((n, n), 40 / G, P["seed"] + 30) > 0.78
    bush_w = np.select([cat == 1, cat == 2, cat == 3, cat == 0], [1.0, 0.8, 0.6, 0.12 + 0.6 * (stand < 0.55)], 0.0)
    bush_w = bush_w + 0.5 * (open_land & (ndi.distance_transform_edt(~forest) * G < 20)) + 1.5 * (thicket & forest)
    bush_w = bush_w * usable
    pts_b = sample_cells(bush_w > 0, P["bushes"] * 10)
    keep_p = bush_w[tuple(np.array([cell_of(x, z) for x, z in pts_b]).T)] / bush_w.max()
    placed = 0
    for (x, z), kp in zip(pts_b, keep_p):
        if placed >= P["bushes"]:
            break
        if rng.random() > kp:
            continue
        sp = nearest_species(x, z)
        fam_name = "Bush-Sycamore" if sp in ("Sycamore", "Spruce", "Fir", "ScotsPine") else "Bush-Oak"
        names = fam[fam_name]
        if place(2, names[rng.integers(len(names))], x, z, rng.uniform(0.85, 1.15), int(cat[cell_of(x, z)])):
            placed += 1
    log(f"{placed} bushes")

    # saplings: edges, canopy gaps, reclaimed ground near forest; young of the local species or cards
    gap = forest & (stand < 0.35)
    near_forest = open_land & (ndi.distance_transform_edt(~forest) * G < 40)
    sap_w = 1.0 * (cat == 1) + 1.2 * gap + 0.7 * near_forest + 0.4 * (cat == 2)
    pts_s = sample_cells(sap_w > 0, P["saplings"] * 4)
    kp_s = sap_w[tuple(np.array([cell_of(x, z) for x, z in pts_s]).T)] / max(sap_w.max(), 1e-6)
    placed = 0
    for (x, z), kp in zip(pts_s, kp_s):
        if placed >= P["saplings"]:
            break
        if rng.random() > kp:
            continue
        c = int(cat[cell_of(x, z)])
        if rng.random() < 0.3:
            names = fam["Card"]
            ok = place(3, names[rng.integers(len(names))], x, z, rng.uniform(1.0, 1.6), c)
        else:
            sp = nearest_species(x, z) or "Oak"
            name = variant(sp, "Young")
            target_h = rng.uniform(2.0, 3.5)
            ok = place(3, name, x, z, target_h / A[name]["height"], c)
        placed += ok
    log(f"{placed} saplings")

    # berry patches: small clusters at sunny edges and clearings
    berry_w = ((cat == 1) | gap | near_forest) & (L["sun"] > 0.5)
    centres = sample_cells(berry_w, P["berry_bushes"])
    placed = 0
    for cx, cz in centres:
        if placed >= P["berry_bushes"]:
            break
        for _ in range(rng.integers(3, 9)):
            x, z = cx + rng.normal(0, 3), cz + rng.normal(0, 3)
            names = fam["BerryBush"]
            if place(4, names[rng.integers(len(names))], x, z, rng.uniform(0.85, 1.15), int(cat[cell_of(x, z)])):
                placed += 1
    log(f"{placed} berry bushes")

    # fallen logs: dense forest, matched loosely by being near the trees that fell
    log_w = (cat == 0) & (stand > 0.5)
    pts_l = sample_cells(log_w, P["logs"] * 2)
    placed = 0
    for x, z in pts_l:
        if placed >= P["logs"]:
            break
        names = fam["GroundLog"]
        placed += place(5, names[rng.integers(len(names))], x, z, rng.uniform(0.8, 1.1), 0)
    log(f"{placed} fallen logs")

    # ------------------------------------------------------------ 8. transforms + write
    recs = []
    gz, gx = np.gradient(H1[::2, ::2], 2.0)

    def slope_at(x, z):
        i = min(max(int((z - ORIGIN) / 2), 0), gz.shape[0] - 1)
        j = min(max(int((x - ORIGIN) / 2), 0), gz.shape[1] - 1)
        return float(gx[i, j]), float(gz[i, j])

    for t in trees + under:
        kind = t.get("kind", 0)
        e = A[t["asset"]]
        sx, sz = slope_at(t["x"], t["z"])
        yaw = rng.uniform(0, 2 * math.pi)
        if kind == 5:            # logs lie along the ground: tilt to the terrain normal
            tx, tz = math.atan(sz), -math.atan(sx)
        else:
            tx, tz = math.radians(rng.uniform(-2.5, 2.5)), math.radians(rng.uniform(-2.5, 2.5))
        sink = math.hypot(sx, sz) * max(e["trunkRadius"], 0.3) * t["scale"] + 0.05
        y = ground(t["x"], t["z"]) - sink
        recs.append(REC.pack(t["x"], y, t["z"], yaw, tx, tz, t["scale"], aid[t["asset"]], CATS.index(CATS[t["cat"]]) if t["cat"] >= 0 else 255, kind))
    with open(os.path.join(plan_dir, "plan.bin"), "wb") as f:
        for r in recs:
            f.write(r)

    stats = {
        "trees": len(trees), "undergrowth": len(under),
        "by_kind": dict(Counter(KINDS[t.get("kind", 0)] for t in trees + under)),
        "by_species": dict(Counter(A[t["asset"]]["species"] for t in trees)),
        "by_age": dict(Counter(A[t["asset"]]["age"] for t in trees)),
        "by_category": dict(Counter(CATS[t["cat"]] for t in trees)),
        "by_family": dict(Counter(A[t["asset"]]["name"].rsplit("-Var", 1)[0] for t in under)),
        "forest_share_of_usable": float(forest.sum() / usable.sum()),
        "orchards": orch_sites,
    }
    json.dump({"assets": assets, "record": "<7fHBB x y z yaw tiltX tiltZ scale asset category kind",
               "categories": CATS, "kinds": KINDS, "params": P, "stats": stats},
              open(os.path.join(plan_dir, "plan.json"), "w"), indent=1)
    np.savez_compressed(os.path.join(plan_dir, "canopy.npz"), canopy=canopy, stand=stand, cat=cat,
                        forest=forest, F=F.astype(np.float32))
    log(json.dumps(stats))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
