"""Reads a Terrain's .avox file into a top-surface heightfield and material map.

Format: docs/superpowers/specs/2026-10-06-terrain-core-design.md, "The .avox file".
Each dense chunk is one zstd frame of 32^3 distance residuals (3D Lorenzo
predictor, so three wrapping cumulative sums invert it) then 32^3 material Ids,
x fastest. Distances are int8, negative inside, +-127 = +-4 cells.

Usage: py -m tools.vegetation.terrain <file.avox> <out.npz>
"""
import struct
import sys

import numpy as np
import zstandard

CHUNK = 32
AIR = 127


def read_avox(path):
    data = open(path, "rb").read()
    magic, vmaj, vmin, voxel, csize, count, _crc, _r = struct.unpack_from("<4sHHfIIIQ", data, 0)
    assert magic == b"AVOX" and csize == CHUNK, (magic, csize)
    index = np.frombuffer(data, dtype=np.dtype([("x", "<i4"), ("y", "<i4"), ("z", "<i4"), ("form", "u1"),
                                                ("dist", "i1"), ("id", "u1"), ("r0", "u1"), ("off", "<u8"),
                                                ("size", "<u4"), ("r1", "<u4")]), count=count, offset=32)
    return data, voxel, index


def _decode(dctx, data, off, size):
    raw = np.frombuffer(dctx.decompress(data[off:off + size], max_output_size=65536), dtype=np.uint8)
    d = raw[:32768].reshape(CHUNK, CHUNK, CHUNK)              # [z][y][x]
    for axis in (2, 1, 0):
        d = np.cumsum(d, axis=axis, dtype=np.uint8)           # wraps mod 256: exact inverse
    return d.view(np.int8), raw[32768:].reshape(CHUNK, CHUNK, CHUNK)


def surface(path):
    """-> (height[z, x] in cells, material[z, x] Id, origin cell (x0, z0), voxel size)."""
    data, voxel, idx = read_avox(path)
    x0, z0 = int(idx["x"].min()), int(idx["z"].min())
    nx, nz = int(idx["x"].max()) - x0 + 1, int(idx["z"].max()) - z0 + 1
    H = np.full((nz * CHUNK, nx * CHUNK), np.nan, dtype=np.float32)
    M = np.zeros((nz * CHUNK, nx * CHUNK), dtype=np.uint8)
    dctx = zstandard.ZstdDecompressor()
    columns = {}
    for i, e in enumerate(idx):
        columns.setdefault((int(e["x"]), int(e["z"])), []).append(i)
    for (cx, cz), members in columns.items():
        members.sort(key=lambda i: -int(idx[i]["y"]))
        found = np.zeros((CHUNK, CHUNK), dtype=bool)
        hz = np.full((CHUNK, CHUNK), np.nan, dtype=np.float32)
        mz = np.zeros((CHUNK, CHUNK), dtype=np.uint8)
        above = np.full((CHUNK, CHUNK), AIR, dtype=np.int16)    # bottom layer of the chunk above, [z][x]
        above_mat = np.zeros((CHUNK, CHUNK), dtype=np.uint8)
        last_y = None
        for i in members:
            e = idx[i]
            cy = int(e["y"])
            if last_y is not None and cy != last_y - 1:          # a gap of air chunks in between
                above[:] = AIR
            if e["form"] == 1:
                D, Mt = _decode(dctx, data, int(e["off"]), int(e["size"]))
            else:
                D = np.full((CHUNK, CHUNK, CHUNK), int(e["dist"]), dtype=np.int8)
                Mt = np.full((CHUNK, CHUNK, CHUNK), int(e["id"]), dtype=np.uint8)
            solid = D <= 0                                       # [z][y][x]
            any_solid = solid.any(axis=1) & ~found
            if any_solid.any():
                top = CHUNK - 1 - np.argmax(solid[:, ::-1, :], axis=1)   # highest solid y, [z][x]
                zz, xx = np.nonzero(any_solid)
                ty = top[zz, xx]
                d0 = D[zz, ty, xx].astype(np.float32)
                d1 = np.where(ty < CHUNK - 1, D[zz, np.minimum(ty + 1, CHUNK - 1), xx], above[zz, xx]).astype(np.float32)
                t = np.clip(d0 / np.where(d1 - d0 == 0, 1, d1 - d0) * -1, 0, 1)   # zero crossing in (y, y+1)
                hz[zz, xx] = cy * CHUNK + ty + t
                mat = Mt[zz, ty, xx]
                mat_up = np.where(ty < CHUNK - 1, Mt[zz, np.minimum(ty + 1, CHUNK - 1), xx], above_mat[zz, xx])
                mz[zz, xx] = np.where(mat == 0, mat_up, mat)
                found |= any_solid
            above = D[:, 0, :].astype(np.int16)
            above_mat = Mt[:, 0, :]
            last_y = cy
            if found.all():
                break
        rz, rx = (cz - z0) * CHUNK, (cx - x0) * CHUNK
        H[rz:rz + CHUNK, rx:rx + CHUNK] = hz
        M[rz:rz + CHUNK, rx:rx + CHUNK] = mz
    return H, M, (x0 * CHUNK, z0 * CHUNK), voxel


if __name__ == "__main__":
    import time
    t = time.time()
    H, M, origin, voxel = surface(sys.argv[1])
    np.savez_compressed(sys.argv[2], height=H, material=M, origin=np.array(origin), voxel=voxel)
    print(f"{H.shape} origin {origin} voxel {voxel} height {np.nanmin(H):.1f}..{np.nanmax(H):.1f} "
          f"nan {np.isnan(H).mean():.3%} in {time.time() - t:.1f}s")
    ids, counts = np.unique(M, return_counts=True)
    print(dict(zip(ids.tolist(), (counts / M.size).round(4).tolist())))
