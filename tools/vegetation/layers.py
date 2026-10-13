"""Terrain simulation on a 4-unit grid: relief, hydrology, sun, soil, human use, obstacles.

Input: surface.npz from terrain.py (1-unit heightfield + material Ids) and the
project's Workspace folder (for placed objects). Output: layers.npz, every
layer a float32/bool [row=z, col=x] grid. World = cell - 2048 (Z1's Transform).

Materials are painted as dithered speckle, so surfaces are classified from
local material fractions, never from a single cell.

Usage: py -m tools.vegetation.layers <surface.npz> <Workspace.workspace dir> <layers.npz>
"""
import heapq
import json
import math
import os
import sys
import time

import numpy as np
from scipy import ndimage as ndi

G = 4                      # grid cell size, units
ORIGIN = -2048.0           # world coordinate of terrain cell 0 (x and z)
MAT = {"Grass": 1, "GrassField": 2, "DryGrass": 3, "Dirt": 4, "ForestFloor": 5, "TilledDirt": 6,
       "RockGrey": 7, "RockBrown": 8, "Riverbed": 9, "Gravel": 10, "Asphalt": 11}
SINK_DEPTH = 5.0           # deeper closed basins drain into themselves instead of flooding the map


def block(a, f=G):
    n = a.shape[0] // f
    return a[:n * f, :n * f].reshape(n, f, n, f).mean(axis=(1, 3))


def norm(a, lo=2, hi=98):
    p0, p1 = np.nanpercentile(a, [lo, hi])
    return np.clip((a - p0) / (p1 - p0 + 1e-9), 0, 1).astype(np.float32)


def lowfreq_noise(shape, scale_cells, seed):
    rng = np.random.default_rng(seed)
    out = np.zeros(shape, np.float32)
    for octave, amp in ((1, 1.0), (0.5, 0.5), (0.25, 0.25)):
        out += amp * ndi.gaussian_filter(rng.standard_normal(shape).astype(np.float32), scale_cells * octave, mode="wrap")
    return norm(out, 1, 99)


# ---------------------------------------------------------------- hydrology
def priority_flood(h, outlets):
    """Barnes priority-flood with epsilon. Returns (filled, receiver flat index or -1, pop order)."""
    n, m = h.shape
    filled = h.astype(np.float64).ravel().copy()
    recv = np.full(n * m, -1, np.int64)
    seen = np.zeros(n * m, bool)
    heap = []
    edge = np.zeros((n, m), bool)
    edge[0, :] = edge[-1, :] = edge[:, 0] = edge[:, -1] = True
    for idx in np.flatnonzero((edge | outlets).ravel()):
        heap.append((filled[idx], int(idx)))
        seen[idx] = True
    heapq.heapify(heap)
    order = []
    offs = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    pop, push = heapq.heappop, heapq.heappush
    while heap:
        z, c = pop(heap)
        order.append(c)
        r, q = divmod(c, m)
        for dr, dq in offs:
            rr, qq = r + dr, q + dq
            if 0 <= rr < n and 0 <= qq < m:
                k = rr * m + qq
                if not seen[k]:
                    seen[k] = True
                    if filled[k] <= z:
                        filled[k] = z + 1e-4
                    recv[k] = c
                    push(heap, (filled[k], k))
    return filled.reshape(n, m), recv, np.array(order, np.int64)


def accumulate(recv, order):
    acc = np.ones(recv.size, np.float64)
    for c in order[::-1]:
        r = recv[c]
        if r >= 0:
            acc[r] += acc[c]
    return acc


# ---------------------------------------------------------------- sun
def insolation(h, cell, samples=10, steps=48, step_cells=2):
    """Daily sun on a slope with terrain shadowing: sum over a sun path of max(cos, 0) * lit.
    North is -Z (rows decrease), so the noon sun stands in the +Z (south) sky."""
    gz, gx = np.gradient(h, cell)
    nx, ny, nz = -gx, np.ones_like(h), -gz
    inv = 1 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx * inv, ny * inv, nz * inv
    total = np.zeros_like(h)
    pad = steps * step_cells + 1
    hp = np.pad(h, pad, mode="edge")       # edge padding, not wraparound, so borders do not shadow
    n0, m0 = h.shape
    for t in np.linspace(0.08, 0.92, samples):
        az = math.pi * t                              # 0 = east, pi/2 = south, pi = west
        el = math.radians(8 + 50 * math.sin(math.pi * t))
        sx, sz = math.cos(az) * math.cos(el), math.sin(az) * math.cos(el)
        sy = math.sin(el)
        lam = np.clip(nx * sx + ny * sy + nz * sz, 0, None)
        lit = np.ones_like(h, bool)
        dx, dz = sx / math.hypot(sx, sz), sz / math.hypot(sx, sz)
        for k in range(1, steps + 1):
            ox, oz = int(round(dx * k * step_cells)), int(round(dz * k * step_cells))
            shifted = hp[pad + oz:pad + oz + n0, pad + ox:pad + ox + m0]
            rise = k * step_cells * cell * math.tan(el)
            lit &= ~(shifted - h > rise)
        total += lam * lit * sy ** 0.3
    return norm(total)


# ---------------------------------------------------------------- obstacles from the place
def workspace_objects(ws_dir):
    """(x, z, radius) of every placed Brush/GameObject/PhysicsObject in the Workspace, in world units."""
    out = []
    for root, _, files in os.walk(ws_dir):
        for f in files:
            if not f.endswith(".json"):
                continue
            try:
                j = json.load(open(os.path.join(root, f)))
            except Exception:
                continue
            if j.get("class") not in ("Brush", "GameObject", "PhysicsObject") or "Transform" not in j:
                continue
            t = j["Transform"]
            r = 4.0
            if j.get("class") == "Brush" and "Faces" in j:
                pts = [p for face in json.loads(j["Faces"]) for p in face["p"]]
                r = max(math.hypot(pts[i], pts[i + 2]) for i in range(0, len(pts), 3))
            out.append((t[12], t[14], r))
    return out


def build(surface_path, ws_dir):
    t0 = time.time()
    z = np.load(surface_path)
    H1, M1 = z["height"], z["material"]
    n = H1.shape[0] // G
    h = block(H1).astype(np.float32)
    frac = {name: block((M1 == i).astype(np.float32)) for name, i in MAT.items()}
    L = {"height": h}
    for name, f in frac.items():
        L["frac_" + name] = f.astype(np.float32)
    print(f"grid {n}x{n} ({time.time() - t0:.1f}s)", flush=True)

    # relief
    gz, gx = np.gradient(h, G)
    L["slope"] = np.degrees(np.arctan(np.hypot(gx, gz))).astype(np.float32)
    L["northness"] = (gz / (np.hypot(gx, gz) + 1e-6)).astype(np.float32)    # +1 faces north (-Z)... see note
    L["tpi_fine"] = (h - ndi.gaussian_filter(h, 10 / G * 2)).astype(np.float32)    # ~20 unit scale
    L["tpi_broad"] = (h - ndi.gaussian_filter(h, 200 / G)).astype(np.float32)      # ~200 unit scale

    # surface classes from local fractions
    # Seen at 1-unit resolution: roads are solid RockBrown ~6 units wide, towns are
    # Asphalt + RockBrown pads, Dirt is natural trails and patches (trees may grow there).
    # Mountain rock is RockBrown too, told apart by slope.
    paved1 = np.isin(M1, [MAT["RockBrown"], MAT["Asphalt"]]).astype(np.float32)
    paved1 = ndi.uniform_filter(paved1, 3) > 0.6
    paved = block(paved1.astype(np.float32)) >= 0.3
    lab, nlab = ndi.label(paved, structure=np.ones((3, 3)))
    idx = np.arange(1, nlab + 1)
    sizes = ndi.sum(paved, lab, index=idx)
    steep_part = ndi.mean(L["slope"], lab, index=idx)
    asphalt_part = ndi.mean(frac["Asphalt"], lab, index=idx)
    is_rock = np.zeros(nlab + 1, bool)
    is_rock[1:] = steep_part > 18
    keep = np.zeros(nlab + 1, bool)
    keep[1:] = (sizes >= 12) & ~is_rock[1:]
    hard = keep[lab]
    # fat parts (survive a 5x5 opening) or asphalt-rich parts are lots/towns; thin parts are roads
    fat = ndi.binary_dilation(ndi.binary_opening(hard, structure=np.ones((5, 5))), iterations=1) & hard
    L["lots"] = fat | (hard & (ndi.uniform_filter(frac["Asphalt"], 3) > 0.3))
    L["road"] = hard & ~L["lots"]
    L["water"] = ndi.binary_opening(frac["Riverbed"] > 0.5, iterations=1)
    L["rock"] = (is_rock[lab] & paved) | (ndi.uniform_filter(frac["RockGrey"], 3) > 0.5)
    L["field"] = ndi.uniform_filter(frac["GrassField"], 9) > 0.62
    L["woodland"] = ndi.uniform_filter(frac["ForestFloor"], 9).astype(np.float32)
    soil = (1.0 * frac["Grass"] + 0.9 * frac["GrassField"] + 1.0 * frac["ForestFloor"] + 0.8 * frac["DryGrass"]
            + 0.5 * frac["Dirt"] + 0.6 * frac["TilledDirt"] + 0.3 * frac["Gravel"])
    L["soil"] = norm(ndi.uniform_filter(soil, 7))

    # placed objects (the test house etc.)
    obj = np.zeros((n, n), bool)
    for x, zz, r in workspace_objects(ws_dir):
        c, rr = (x - ORIGIN) / G, (zz - ORIGIN) / G
        rad = r / G + 1
        y0, y1 = int(max(0, rr - rad)), int(min(n, rr + rad + 1))
        x0, x1 = int(max(0, c - rad)), int(min(n, c + rad + 1))
        obj[y0:y1, x0:x1] = True
    L["objects"] = obj
    print(f"surfaces ({time.time() - t0:.1f}s)", flush=True)

    # distances, in units
    for name in ("road", "lots", "water", "objects", "field"):
        L["dist_" + name] = ndi.distance_transform_edt(~L[name], sampling=G).astype(np.float32)
    L["dist_human"] = np.minimum(L["dist_road"], np.minimum(L["dist_lots"] * 0.6, L["dist_objects"] * 0.6))
    rows, cols = np.mgrid[0:n, 0:n]
    L["dist_edge"] = (np.minimum(np.minimum(rows, cols), np.minimum(n - 1 - rows, n - 1 - cols)) * G).astype(np.float32)

    # hydrology: fill once, turn deep basins into sinks, fill again
    filled, _, _ = priority_flood(h, np.zeros((n, n), bool))
    depth = filled - h
    # Only the pit bottoms of deep depressions become sinks: marking every flooded cell
    # would turn this bowl-shaped map into one lake. Each pit drains its own valley.
    hs = ndi.gaussian_filter(h, 1.0)
    pits = (hs == ndi.minimum_filter(hs, size=9)) & (depth > SINK_DEPTH)
    sinks = ndi.binary_dilation(pits, iterations=1) | L["water"]
    print(f"fill 1: {int((depth > SINK_DEPTH).sum())} deep cells, {int(pits.sum())} deep pits "
          f"({time.time() - t0:.1f}s)", flush=True)
    filled, recv, order = priority_flood(h, sinks)
    acc = accumulate(recv, order).reshape(n, n)
    L["flow"] = acc.astype(np.float32)
    L["sinks"] = sinks
    tan_b = np.maximum(np.tan(np.radians(L["slope"])), 0.01)
    L["twi"] = np.log(acc * G / tan_b).astype(np.float32)
    L["stream"] = acc * G * G > 40_000          # > ~4 ha of catchment
    L["dist_stream"] = ndi.distance_transform_edt(~(L["stream"] | L["water"]), sampling=G).astype(np.float32)
    print(f"hydrology ({time.time() - t0:.1f}s)", flush=True)

    # sun on a half-resolution grid
    L["sun"] = ndi.zoom(insolation(block(h, 2), G * 2), 2, order=1)[:n, :n].astype(np.float32)
    print(f"sun ({time.time() - t0:.1f}s)", flush=True)

    # wetness and erosion proxies
    L["wet"] = norm(ndi.gaussian_filter(L["twi"], 1.5))
    steep = norm(L["slope"], 5, 99)
    L["erosion"] = np.clip(steep * norm(np.log1p(acc)), 0, 1).astype(np.float32)
    L["forbidden"] = (L["road"] | L["lots"] | L["water"] | L["rock"] | obj | (L["slope"] > 38)).astype(bool)
    return L


if __name__ == "__main__":
    L = build(sys.argv[1], sys.argv[2])
    np.savez_compressed(sys.argv[3], **L)
    usable = ~L["forbidden"]
    print(f"usable {usable.mean():.1%}  road {L['road'].mean():.2%}  lots {L['lots'].mean():.2%}  water "
          f"{L['water'].mean():.2%}  rock {L['rock'].mean():.2%}  field {L['field'].mean():.1%}  "
          f"sinks {L['sinks'].mean():.2%}  streams {L['stream'].mean():.2%}")
