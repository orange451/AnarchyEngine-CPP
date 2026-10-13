"""Prepares the export's shared textures for import.

Each material key (Leaf_/Bark_/Log_<albedo stem>) gets its albedo, and the
normal and spec maps its first MTL material names: they do not always share
the albedo's stem (several dead trunks borrow a log's maps). Every output file
is named after its SOURCE texture with a Veg_ prefix, so a map that several
materials share is written, and later imported, once. Roblox packs roughness
in the spec map's G channel and metalness in R; the engine reads one map per
slot from its red channel, so each non-flat channel becomes its own grayscale
file. Leaf spec maps are flat and leaves get scalar roughness instead. The
export's _emap files are constant and unused.

Usage: py -m tools.vegetation.prepare_textures <src_dir> <Trees.mtl> <manifest.json> <out_dir>
"""
import json
import os
import shutil
import sys

from PIL import Image

FLAT_TOLERANCE = 2
PREFIX = "Veg_"


def channel_is_flat(img, channel: int) -> bool:
    lo, hi = img.convert("RGBA").getchannel(channel).getextrema()
    return hi - lo <= FLAT_TOLERANCE


def read_mtl_maps(mtl_path: str) -> dict:
    """albedo file -> {"normal": file|None, "spec": file|None}, from the first material naming it."""
    materials, cur = [], None
    with open(mtl_path, errors="ignore") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "newmtl":
                cur = {}
                materials.append(cur)
            elif cur is not None and parts[0] in ("map_Kd", "map_Bump", "map_Ns") and len(parts) > 1:
                cur[parts[0]] = parts[1]
    out = {}
    for m in materials:
        albedo = m.get("map_Kd")
        if albedo and albedo not in out:
            out[albedo] = {"normal": m.get("map_Bump"), "spec": m.get("map_Ns")}
    return out


def _stem(file_name: str) -> str:
    return file_name.rsplit("_", 1)[0]   # "Log9_nmap.png" -> "Log9"


def prepare(src_dir: str, mtl_path: str, manifest_path: str, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    maps = read_mtl_maps(mtl_path)
    keys = []
    with open(manifest_path) as f:
        for entry in json.load(f):
            for k in entry["materials"]:
                if k not in keys:
                    keys.append(k)
    written = {}   # output name -> True, so a shared map is written once

    def copy(src_name: str, out_name: str) -> str:
        if out_name not in written:
            shutil.copyfile(os.path.join(src_dir, src_name), os.path.join(out_dir, out_name))
            written[out_name] = True
        return out_name

    def channel(spec_name: str, ch: int, out_name: str) -> str | None:
        if out_name in written:
            return out_name
        img = Image.open(os.path.join(src_dir, spec_name)).convert("RGBA")
        if channel_is_flat(img, ch):
            return None
        img.getchannel(ch).save(os.path.join(out_dir, out_name))
        written[out_name] = True
        return out_name

    result = {}
    for key in keys:
        kind, albedo_stem = key.split("_", 1)
        albedo = f"{albedo_stem}_diff.png"
        files = {"Color": copy(albedo, f"{PREFIX}{albedo_stem}_Color.png"),
                 "Normal": None, "Roughness": None, "Metalness": None}
        found = maps.get(albedo, {})
        normal = found.get("normal")
        if normal and os.path.exists(os.path.join(src_dir, normal)):
            files["Normal"] = copy(normal, f"{PREFIX}{_stem(normal)}_Normal.png")
        spec = found.get("spec")
        if kind in ("Bark", "Log") and spec and os.path.exists(os.path.join(src_dir, spec)):
            files["Roughness"] = channel(spec, 1, f"{PREFIX}{_stem(spec)}_Roughness.png")
            files["Metalness"] = channel(spec, 0, f"{PREFIX}{_stem(spec)}_Metalness.png")
        result[key] = files
    with open(os.path.join(out_dir, "textures.json"), "w") as f:
        json.dump(result, f, indent=1)
    return result


if __name__ == "__main__":
    r = prepare(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
    unique = {f for v in r.values() for f in v.values() if f}
    print(len(r), "materials,", len(unique), "texture files")
