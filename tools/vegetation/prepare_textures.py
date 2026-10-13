"""Prepares the export's shared textures for import.

Each material key (Leaf_/Bark_/Log_<albedo stem>) gets its albedo plus the
normal and spec maps the MTL pairs with that albedo; they do not always share
its stem (several dead trunks borrow a log's maps). Output files are named
after their SOURCE texture with a Veg_ prefix, so a shared map is written, and
later imported, once.

Roblox packs roughness in the spec map's G channel and metalness in R. One
albedo can appear with several spec maps (a flat 127 placeholder and an
authored one), so the most detailed is used. The engine reads one map per slot
from its red channel: a channel that varies becomes a grayscale texture; a
nearly flat one becomes the material's scalar value instead (same look, no
VRAM). In this export metalness is 0 everywhere, bark and log roughness is a
flat 0.5, most leaf roughness is a near-flat ~0.95, and the apple/berry atlas
(Leaves138_spec) carries a real roughness map. The _emap files are unused.

Usage: py -m tools.vegetation.prepare_textures <src_dir> <Trees.mtl> <manifest.json> <out_dir>
"""
import json
import os
import shutil
import sys

from PIL import Image, ImageStat

PREFIX = "Veg_"
NEAR_FLAT = 32          # a channel whose range is at most this is stored as a scalar
PLACEHOLDER = 127       # Roblox's flat default roughness


def read_mtl_maps(mtl_path: str) -> dict:
    """albedo file -> {"normal": first normal map, "specs": [every spec map, in MTL order]}."""
    out, cur = {}, None
    materials = []
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
    for m in materials:
        albedo = m.get("map_Kd")
        if not albedo:
            continue
        entry = out.setdefault(albedo, {"normal": None, "specs": []})
        if entry["normal"] is None and m.get("map_Bump"):
            entry["normal"] = m["map_Bump"]
        if m.get("map_Ns") and m["map_Ns"] not in entry["specs"]:
            entry["specs"].append(m["map_Ns"])
    return out


def _stem(file_name: str) -> str:
    return file_name.rsplit("_", 1)[0]   # "Log9_nmap.png" -> "Log9"


def _channel_stats(img, ch):
    band = img.getchannel(ch)
    lo, hi = band.getextrema()
    return hi - lo, ImageStat.Stat(band).mean[0]


def _pick_spec(src_dir, specs):
    """The most detailed spec map: widest G range, then not the flat 127 placeholder."""
    best, best_score = None, None
    for name in specs:
        path = os.path.join(src_dir, name)
        if not os.path.exists(path):
            continue
        img = Image.open(path).convert("RGBA")
        rng, mean = _channel_stats(img, 1)
        score = (rng, abs(mean - PLACEHOLDER) > 2)
        if best_score is None or score > best_score:
            best, best_score = (name, img), score
    return best


def prepare(src_dir: str, mtl_path: str, manifest_path: str, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    maps = read_mtl_maps(mtl_path)
    keys = []
    with open(manifest_path) as f:
        for entry in json.load(f):
            for k in entry["materials"]:
                if k not in keys:
                    keys.append(k)
    written = set()

    def copy(src_name, out_name):
        if out_name not in written:
            shutil.copyfile(os.path.join(src_dir, src_name), os.path.join(out_dir, out_name))
            written.add(out_name)
        return out_name

    result = {}
    for key in keys:
        albedo_stem = key.split("_", 1)[1]
        albedo = f"{albedo_stem}_diff.png"
        files = {"Color": copy(albedo, f"{PREFIX}{albedo_stem}_Color.png"), "Normal": None,
                 "Roughness": None, "Metalness": None, "RoughnessValue": None, "MetalnessValue": None}
        found = maps.get(albedo, {"normal": None, "specs": []})
        if found["normal"] and os.path.exists(os.path.join(src_dir, found["normal"])):
            files["Normal"] = copy(found["normal"], f"{PREFIX}{_stem(found['normal'])}_Normal.png")
        picked = _pick_spec(src_dir, found["specs"])
        if picked:
            spec_name, img = picked
            for ch, slot in ((1, "Roughness"), (0, "Metalness")):
                rng, mean = _channel_stats(img, ch)
                if rng <= NEAR_FLAT:
                    files[slot + "Value"] = round(mean / 255.0, 3)
                else:
                    out_name = f"{PREFIX}{_stem(spec_name)}_{slot}.png"
                    if out_name not in written:
                        img.getchannel(ch).save(os.path.join(out_dir, out_name))
                        written.add(out_name)
                    files[slot] = out_name
        result[key] = files
    with open(os.path.join(out_dir, "textures.json"), "w") as f:
        json.dump(result, f, indent=1)
    return result


if __name__ == "__main__":
    r = prepare(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
    for k, v in sorted(r.items()):
        print(k, {s: f for s, f in v.items() if f is not None and s != "Color"})
    print(len(r), "materials,", len({f for v in r.values() for s, f in v.items()
                                      if isinstance(f, str)}), "texture files")
