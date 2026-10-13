"""Classifies the export's groups into vegetation assets and names them.

Rules come from docs/superpowers/specs/2026-10-13-vegetation-asset-import-design.md:
trunks sit in species rows; a Leaves group is a crown of its nearest trunk
unless it stands free on the ground (a bush or a card); stumps and the logs
paired to trunks are chop gameplay and are skipped; other logs are ground logs.
"""
import math
from dataclasses import dataclass

from .objscan import Group, bounds as group_bounds

GROUND_Y = -0.15
SPECIES_ROWS = {-630: "Spruce", -580: "ScotsPine", -520: "Fir", -440: "Hawthorn",
                -370: "Apple", -300: "Cherry", -220: "Sycamore", -150: "Apple", -60: "Oak"}
BUSH_FAMILY_BY_ALBEDO = {"Leaves2_diff.png": "Bush-Sycamore", "Leaves3_diff.png": "Bush-Oak",
                         "Leaves1_diff.png": "BerryBush"}
NEAR = 1.5          # trunk radii: inside this a Leaves/Log group belongs to the trunk
BUSH_BOTTOM = 4.0   # a free-standing Leaves group sits within this of the ground
CARD_FACES = 8      # a card is one two-sided quad: 8 triangles in the export


@dataclass
class GroupInfo:
    name: str
    material: str | None
    bounds: tuple
    faces: int


@dataclass
class Asset:
    name: str
    family: str          # "Tree" | "Bush" | "BerryBush" | "Card" | "GroundLog"
    species: str | None
    age: str | None      # "Old" | "Adult" | "Young" | "Dead" | None
    trunk: str | None
    groups: list
    pivot: tuple         # export space


def summarize(g: Group) -> GroupInfo:
    return GroupInfo(g.name, g.material, group_bounds(g), len(g.faces))


def read_mtl_albedos(mtl_path: str) -> dict:
    out, cur = {}, None
    with open(mtl_path, errors="ignore") as f:
        for line in f:
            if line.startswith("newmtl"):
                cur = line.split(maxsplit=1)[1].strip()
            elif line.startswith("map_Kd") and cur:
                out[cur] = line.split(maxsplit=1)[1].strip()
    return out


def _cx(b): return (b[0] + b[3]) / 2
def _cz(b): return (b[2] + b[5]) / 2
def _height(b): return b[4] - b[1]
def _radius(b): return max(b[3] - b[0], b[5] - b[2]) / 2
def _span(b): return max(b[3] - b[0], b[5] - b[2])


def _nearest_trunk(trunks, b):
    best, best_d = None, math.inf
    for t in trunks:
        d = math.hypot(_cx(t.bounds) - _cx(b), _cz(t.bounds) - _cz(b))
        if d < best_d:
            best, best_d = t, d
    return best, best_d


def _near_a_trunk(trunks, b) -> bool:
    t, d = _nearest_trunk(trunks, b)
    return t is not None and d <= NEAR * _radius(t.bounds)


def _ages(n: int, species: str) -> list:
    if species == "Hawthorn" or n <= 1:
        return ["Old"] * n
    if n < 6:
        old = math.ceil(n / 2)
        return ["Old"] * old + ["Young"] * (n - old)
    old, young = math.ceil(n / 3), n // 3
    return ["Old"] * old + ["Adult"] * (n - old - young) + ["Young"] * young


def _trunk_pivot(t):
    return (_cx(t.bounds), t.bounds[1], _cz(t.bounds))


def build_catalog(groups: list, albedo_of: dict) -> list:
    trunks = [g for g in groups if g.name.startswith("Trunk")]
    crowns = {t.name: [] for t in trunks}
    bushes, cards, logs = [], [], []
    for g in groups:
        if g.name.startswith("Leaves"):
            on_ground = (g.bounds[1] - GROUND_Y) <= BUSH_BOTTOM
            if not _near_a_trunk(trunks, g.bounds) and on_ground:
                (cards if g.faces == CARD_FACES else bushes).append(g)
            else:
                crowns[_nearest_trunk(trunks, g.bounds)[0].name].append(g)
        elif g.name.startswith("Log") and not _near_a_trunk(trunks, g.bounds):
            logs.append(g)
        # Strump* and chop logs: skipped

    assets = []
    by_species = {}
    for t in trunks:
        row = round(_cz(t.bounds) / 10) * 10
        if row not in SPECIES_ROWS:
            raise ValueError(f"unknown row z={row} for {t.name}")
        by_species.setdefault(SPECIES_ROWS[row], []).append(t)
    for species, ts in by_species.items():
        leafy = sorted((t for t in ts if crowns[t.name]), key=lambda t: -_height(t.bounds))
        counter = {}
        for t, age in zip(leafy, _ages(len(leafy), species)):
            counter[age] = counter.get(age, 0) + 1
            crown = sorted(crowns[t.name], key=lambda g: int(g.name[len("Leaves"):]))
            assets.append(Asset(f"{species}-{age}-Var{counter[age]:02d}", "Tree", species, age, t.name,
                                [t.name] + [g.name for g in crown], _trunk_pivot(t)))
        dead = [t for t in ts if not crowns[t.name]]
        for i, t in enumerate(dead):
            suffix = "" if len(dead) == 1 else f"-Var{i + 1:02d}"
            assets.append(Asset(f"{species}-Dead{suffix}", "Tree", species, "Dead", t.name, [t.name],
                                _trunk_pivot(t)))

    families = {}
    for g in bushes:
        families.setdefault(BUSH_FAMILY_BY_ALBEDO[albedo_of[g.material]], []).append(g)
    for fam, gs in families.items():
        family = "BerryBush" if fam == "BerryBush" else "Bush"
        for i, g in enumerate(sorted(gs, key=lambda g: -_height(g.bounds))):
            assets.append(Asset(f"{fam}-Var{i + 1:02d}", family, None, None, None, [g.name],
                                (_cx(g.bounds), GROUND_Y, _cz(g.bounds))))
    for i, g in enumerate(sorted(cards, key=lambda g: -_height(g.bounds))):
        assets.append(Asset(f"Card-Var{i + 1:02d}", "Card", None, None, None, [g.name],
                            (_cx(g.bounds), GROUND_Y, _cz(g.bounds))))
    for i, g in enumerate(sorted(logs, key=lambda g: -_span(g.bounds))):
        assets.append(Asset(f"GroundLog-Var{i + 1:02d}", "GroundLog", None, None, None, [g.name],
                            (_cx(g.bounds), g.bounds[1], _cz(g.bounds))))
    return assets


if __name__ == "__main__":
    import collections
    import sys

    from .objscan import scan_groups

    infos = [summarize(g) for g in scan_groups(sys.argv[1])]
    cat = build_catalog(infos, read_mtl_albedos(sys.argv[2]))
    print(collections.Counter(a.family for a in cat))
    print(collections.Counter((a.species, a.age) for a in cat if a.family == "Tree"))
    for a in cat:
        print(a.name, a.groups)
