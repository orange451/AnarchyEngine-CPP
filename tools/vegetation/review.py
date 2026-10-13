"""Builds the interactive review page for a vegetation plan.

Reads plan_dir/{plan.bin, plan.json, layers.npz, canopy.npz}, computes line-of-sight
cover, and writes one self-contained HTML page (review_template.html filled with
base64 data): heat layers as WebP images, every tree and undergrowth instance as
packed typed arrays, and small inspection grids.

Usage: py -m tools.vegetation.review <plan_dir> <out.html>
"""
import base64
import io
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

from .layers import G, ORIGIN

HERE = os.path.dirname(os.path.abspath(__file__))
SPECIES = ["Oak", "Sycamore", "ScotsPine", "Spruce", "Fir", "Hawthorn", "Apple", "Cherry"]
AGES = ["Old", "Adult", "Young", "Dead"]
UNDER = ["Bush-Oak", "Bush-Sycamore", "Sapling", "Card", "BerryBush", "GroundLog"]
REC = struct.Struct("<7fHBB")


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def webp(rgba) -> str:
    buf = io.BytesIO()
    Image.fromarray(rgba).save(buf, "WEBP", quality=82, method=6)
    return "data:image/webp;base64," + b64(buf.getvalue())


def ramp(v, stops, alpha=None):
    """v in 0..1 -> RGBA via linear color stops [(t, (r,g,b)), ...]."""
    v = np.clip(np.nan_to_num(v), 0, 1)
    ts = np.array([s[0] for s in stops])
    cols = np.array([s[1] for s in stops], np.float32)
    out = np.zeros(v.shape + (4,), np.uint8)
    for c in range(3):
        out[..., c] = np.interp(v, ts, cols[:, c]).astype(np.uint8)
    out[..., 3] = 255 if alpha is None else (np.clip(alpha, 0, 1) * 255).astype(np.uint8)
    return out


def los_cover(canopy, step_units=8, reach=100, dirs=16):
    """Share of 16 sight lines from each cell that a crown blocks within `reach` units."""
    blocked_any = canopy > 0.5
    n = blocked_any.shape[0]
    steps = int(reach / step_units)
    pad = int(math.ceil(reach / G)) + 1
    bp = np.pad(blocked_any, pad)
    total = np.zeros((n, n), np.float32)
    for a in range(dirs):
        ang = 2 * math.pi * a / dirs
        hit = np.zeros((n, n), bool)
        for k in range(1, steps + 1):
            d = k * step_units / G
            oi, oj = int(round(math.sin(ang) * d)), int(round(math.cos(ang) * d))
            hit |= bp[pad + oi:pad + oi + n, pad + oj:pad + oj + n]
        total += hit
    return total / dirs


def hotspots(trees, cats, n_cells):
    """Four close-up windows: densest interior, most tree lines, settlement edge, riparian."""
    def best(mask, size=420):
        if not mask.any():
            return None
        xs, zs = trees["x"][mask], trees["z"][mask]
        H, xe, ze = np.histogram2d(xs, zs, bins=40, range=[[ORIGIN, -ORIGIN], [ORIGIN, -ORIGIN]])
        H = ndi.uniform_filter(H, 3)
        i, j = np.unravel_index(np.argmax(H), H.shape)
        cx, cz = (xe[i] + xe[i + 1]) / 2, (ze[j] + ze[j + 1]) / 2
        return {"x": float(cx), "z": float(cz), "size": size}
    spots = [("Closed forest", best(cats == 0)), ("Tree lines", best(cats == 2, 520)),
             ("Village & orchards", best((cats == 4) | (cats == 6), 520)), ("Riparian strip", best(cats == 3))]
    return [{"name": nm, **s} for nm, s in spots if s]


def build(plan_dir, out_html):
    meta = json.load(open(os.path.join(plan_dir, "plan.json")))
    L = np.load(os.path.join(plan_dir, "layers.npz"))
    C = np.load(os.path.join(plan_dir, "canopy.npz"))
    manifest_by = meta["assets"]
    raw = open(os.path.join(plan_dir, "plan.bin"), "rb").read()
    rec = np.frombuffer(raw, dtype=np.dtype([("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("yaw", "<f4"), ("tx", "<f4"),
                                             ("tz", "<f4"), ("s", "<f4"), ("a", "<u2"), ("c", "u1"), ("k", "u1")]))
    split_dir = os.path.join(os.path.dirname(plan_dir), "split")
    manifest = {e["name"]: e for e in json.load(open(os.path.join(split_dir, "manifest.json")))}
    names = np.array(manifest_by)[rec["a"]]
    radius = np.array([manifest[nm]["crownRadius"] for nm in manifest_by], np.float32)[rec["a"]] * rec["s"]

    is_tree = rec["k"] <= 1
    t = rec[is_tree]
    tn = names[is_tree]
    sp = np.array([SPECIES.index(manifest[nm]["species"]) for nm in tn], np.uint8)
    age = np.array([AGES.index(manifest[nm]["age"]) for nm in tn], np.uint8)
    # dead trees draw their trunk footprint, not a crown
    tr = np.where(age == 3, np.array([manifest[nm]["trunkRadius"] for nm in tn]) * t["s"] * 2.5, radius[is_tree])
    pack_t = np.zeros(len(t), dtype=[("x", "<u2"), ("z", "<u2"), ("r", "u1"), ("m", "u1"), ("c", "u1"), ("pad", "u1")])
    pack_t["x"] = np.clip((t["x"] - ORIGIN) * 15, 0, 65535)
    pack_t["z"] = np.clip((t["z"] - ORIGIN) * 15, 0, 65535)
    pack_t["r"] = np.clip(tr * 16, 1, 255)
    pack_t["m"] = sp | (age << 3)
    pack_t["c"] = t["c"]

    u = rec[~is_tree]
    un = names[~is_tree]

    def under_kind(nm, k):
        if k == 3:
            return 3 if nm.startswith("Card") else 2
        return {"Bush-Oak": 0, "Bush-Sycamore": 1, "BerryBush": 4, "GroundLog": 5}[nm.rsplit("-Var", 1)[0]]
    pack_u = np.zeros(len(u), dtype=[("x", "<u2"), ("z", "<u2"), ("r", "u1"), ("m", "u1")])
    pack_u["x"] = np.clip((u["x"] - ORIGIN) * 15, 0, 65535)
    pack_u["z"] = np.clip((u["z"] - ORIGIN) * 15, 0, 65535)
    pack_u["r"] = np.clip(radius[~is_tree] * 16, 1, 255)
    pack_u["m"] = [under_kind(nm, k) for nm, k in zip(un, u["k"])]

    # ---------------------------------------------------------- rasters
    h = L["height"]
    gz, gx = np.gradient(h, G)
    shade = np.clip(0.62 + 0.55 * ((-gx * 0.7 - gz * 0.7) / np.sqrt(1 + gx * gx + gz * gz)), 0, 1)
    elev = (h - h.min()) / (np.ptp(h) + 1e-9)
    base = ramp(elev, [(0, (196, 199, 170)), (0.35, (178, 186, 150)), (0.7, (160, 158, 132)), (1, (210, 206, 196))])
    base[..., :3] = (base[..., :3] * shade[..., None]).astype(np.uint8)
    paved = L["road"] | L["lots"]
    base[paved] = (84, 78, 74, 255)
    base[L["water"]] = (88, 140, 178, 255)
    base[L["rock"]] = (150, 142, 136, 255)

    canopy, stand, cat, F = C["canopy"], C["stand"], C["cat"].astype(np.int16), C["F"]
    cover = los_cover(canopy)
    usable = ~L["forbidden"]
    layers = {
        "density": webp(ramp(stand, [(0, (240, 228, 170)), (0.3, (150, 190, 90)), (0.7, (40, 110, 60)), (1, (12, 52, 32))],
                             alpha=np.clip(stand * 3, 0, 0.92))),
        "cover": webp(ramp(cover, [(0, (242, 160, 54)), (0.5, (240, 214, 120)), (1, (24, 84, 74))], alpha=usable * 0.85)),
        "sun": webp(ramp(L["sun"], [(0, (40, 46, 96)), (0.5, (170, 120, 110)), (1, (250, 214, 100))], alpha=0.8)),
        "wet": webp(ramp(L["wet"], [(0, (214, 188, 140)), (0.5, (120, 170, 160)), (1, (28, 76, 150))], alpha=0.8)),
        "potential": webp(ramp((F - np.percentile(F, 2)) / (np.percentile(F, 98) - np.percentile(F, 2)),
                               [(0, (120, 60, 110)), (0.5, (236, 230, 210)), (1, (30, 100, 60))], alpha=usable * 0.8)),
        "category": webp(np.where((cat >= 0)[..., None], np.array([(29, 77, 44, 210), (106, 168, 79, 200),
                                                                    (224, 165, 38, 230), (58, 143, 214, 210),
                                                                    (196, 92, 60, 200), (0, 0, 0, 0), (214, 92, 168, 220)],
                                                                   np.uint8)[np.clip(cat, 0, 6)], 0).astype(np.uint8)),
    }

    def grid(a, lo, hi):
        return b64(np.clip((a[::2, ::2] - lo) / (hi - lo) * 255, 0, 255).astype(np.uint8).tobytes())

    inspect = {"n": h.shape[0] // 2, "height": grid(h, 0, 200), "sun": grid(L["sun"], 0, 1), "wet": grid(L["wet"], 0, 1),
               "cover": grid(cover, 0, 1), "stand": grid(stand, 0, 1), "slope": grid(L["slope"], 0, 60),
               "cat": b64(np.where(cat[::2, ::2] < 0, 255, cat[::2, ::2]).astype(np.uint8).tobytes())}

    open_cover = cover[usable & (cat == 5)]
    data = {
        "origin": ORIGIN, "size": -2 * ORIGIN + 32, "grid": G,
        "base": webp(base), "layers": layers, "inspect": inspect,
        "trees": b64(pack_t.tobytes()), "under": b64(pack_u.tobytes()),
        "stats": meta["stats"], "params": meta["params"], "spots": hotspots(t, t["c"], h.shape[0]),
        "openExposure": float((open_cover < 0.25).mean()) if open_cover.size else 0,
        "usableArea": float(usable.sum() * G * G),
    }
    html = open(os.path.join(HERE, "review_template.html"), encoding="utf-8").read()
    html = html.replace("/*__DATA__*/null", json.dumps(data, separators=(",", ":")))
    with open(out_html, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print(f"{out_html}: {os.path.getsize(out_html) / 1e6:.1f} MB, {len(t)} trees, {len(u)} undergrowth")


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
