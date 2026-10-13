"""Writes one scaled, re-centred OBJ+MTL per vegetation asset and manifest.json.

Usage: py -m tools.vegetation.split_trees <Trees.obj> <Trees.mtl> <out_dir>
"""
import json
import math
import os
import sys

from .catalog import Asset, build_catalog, read_mtl_albedos, summarize
from .objscan import scan_groups

SCALE = 0.23
TRUNK_BASE_BAND = 1.0   # engine units above the pivot that count as the trunk's base


def material_key(albedo_file: str) -> str:
    stem = albedo_file.split("_diff")[0]
    for prefix, kind in (("Leaves", "Leaf"), ("Trunk", "Bark"), ("Log", "Log")):
        if stem.startswith(prefix):
            return f"{kind}_{stem}"
    raise ValueError(f"unknown albedo {albedo_file}")


def _face_token(vi, ti, ni, nv, nvt, nvn) -> str:
    s = str(vi + 1 + nv)
    if ti is not None or ni is not None:
        s += "/" + ("" if ti is None else str(ti + 1 + nvt))
    if ni is not None:
        s += "/" + str(ni + 1 + nvn)
    return s


def write_asset(asset: Asset, groups: dict, albedo_of: dict, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    px, py, pz = asset.pivot
    keys = []
    lines = [f"mtllib {asset.name}.mtl"]
    nv = nvt = nvn = 0
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    crown_r, crown_base, trunk_r = 0.0, math.inf, 0.0
    for gname in asset.groups:
        g = groups[gname]
        key = material_key(albedo_of[g.material])
        if key not in keys:
            keys.append(key)
        lines.append(f"g {gname}")
        lines.append(f"usemtl {key}")
        is_trunk = asset.family == "Tree" and gname == asset.trunk
        for (x, y, z) in g.v:
            X, Y, Z = (x - px) * SCALE, (y - py) * SCALE, (z - pz) * SCALE
            lines.append(f"v {X:.4f} {Y:.4f} {Z:.4f}")
            for i, c in enumerate((X, Y, Z)):
                lo[i] = min(lo[i], c)
                hi[i] = max(hi[i], c)
            r = math.hypot(X, Z)
            if is_trunk:
                if Y <= TRUNK_BASE_BAND:
                    trunk_r = max(trunk_r, r)
            else:
                crown_r = max(crown_r, r)
                crown_base = min(crown_base, Y)
        lines.extend(f"vt {u:.5f} {v:.5f}" for (u, v) in g.vt)
        lines.extend(f"vn {a:.4f} {b:.4f} {c:.4f}" for (a, b, c) in g.vn)
        for face in g.faces:
            lines.append("f " + " ".join(_face_token(vi, ti, ni, nv, nvt, nvn) for (vi, ti, ni) in face))
        nv += len(g.v)
        nvt += len(g.vt)
        nvn += len(g.vn)
    with open(os.path.join(out_dir, asset.name + ".obj"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out_dir, asset.name + ".mtl"), "w", newline="\n") as f:
        # A distinct Ns per material: the importer's Assimp RemoveRedundantMaterials step
        # ignores names, so otherwise-identical materials would merge bark and leaves into
        # one mesh. The import's material pass replaces these materials, so Ns is unused.
        for i, k in enumerate(keys):
            f.write(f"newmtl {k}\nKd 1 1 1\nNs {10 + i}\nd 1\nillum 2\n")
    if asset.family != "Tree" or crown_base == math.inf:
        crown_base = 0.0
    return {"name": asset.name, "family": asset.family, "species": asset.species, "age": asset.age,
            "groups": list(asset.groups), "pivotExport": [px, py, pz], "materials": keys,
            # The pivot is the ground, so height is the top above it; a bush's stem below stays out.
            "height": hi[1], "crownRadius": crown_r, "crownBase": crown_base,
            "trunkRadius": trunk_r, "bounds": lo + hi}


def split(obj_path: str, mtl_path: str, out_dir: str) -> list:
    albedo_of = read_mtl_albedos(mtl_path)
    catalog = build_catalog([summarize(g) for g in scan_groups(obj_path)], albedo_of)
    owner = {name: a for a in catalog for name in a.groups}
    remaining = {a.name: len(a.groups) for a in catalog}
    held, entries = {}, {}
    for g in scan_groups(obj_path):
        a = owner.get(g.name)
        if a is None:
            continue
        held[g.name] = g
        remaining[a.name] -= 1
        if remaining[a.name] == 0:
            entries[a.name] = write_asset(a, held, albedo_of, out_dir)
            for name in a.groups:
                held.pop(name, None)
    ordered = [entries[a.name] for a in catalog]
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(ordered, f, indent=1)
    return ordered


if __name__ == "__main__":
    print(len(split(sys.argv[1], sys.argv[2], sys.argv[3])), "assets written")
