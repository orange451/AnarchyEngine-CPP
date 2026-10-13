"""Reruns the shared-material pass on a studio that already holds the imported
vegetation (e.g. after prepare_textures changes): imports any prepared texture the
place lacks, updates the Materials in place, verifies, and saves if it passes.

Usage: py -m tools.vegetation.update_materials <split_dir> [port] [token]
"""
import glob
import json
import os
import sys

from .import_vegetation import HERE, check_report, import_files, lua_table, parse_verify
from .mcp_client import Studio


def main(split_dir, port=8765, token="veg") -> int:
    s = Studio(port, token)
    s.initialize()
    with open(os.path.join(split_dir, "manifest.json")) as f:
        manifest = json.load(f)
    with open(os.path.join(split_dir, "textures", "textures.json")) as f:
        textures = json.load(f)

    have = set(s.lua('for _, t in ipairs(game.Assets.Textures.Vegetation:GetChildren()) do print(t.Name) end'))
    missing = [p for p in sorted(glob.glob(os.path.join(split_dir, "textures", "*.png")))
               if os.path.splitext(os.path.basename(p))[0] not in have]
    if missing:
        import_files(s, missing)
    print("textures added:", [os.path.basename(p) for p in missing], flush=True)

    with open(os.path.join(HERE, "fix_materials.luau")) as f:
        lua = f.read()
    lua = (lua.replace("__TEXTURES__", lua_table(textures))
              .replace("__ASSET_MATERIALS__", lua_table({e["name"]: e["materials"] for e in manifest}))
              .replace("__FAMILIES__", lua_table({e["name"]: e["family"] for e in manifest})))
    print([line.replace("\t", " ") for line in s.lua(lua) if line.startswith("FIX_DONE")], flush=True)

    with open(os.path.join(HERE, "verify_import.luau")) as f:
        report = parse_verify(s.lua(f.read()), manifest)
    print({k: (len(v) if k.endswith("_mismatch") else v) for k, v in report.items()}, flush=True)
    if not check_report(report, n_prefabs=len(manifest), n_materials=len(textures)):
        print("NOT SAVED: verification failed")
        return 1
    print("saved:", json.dumps(s.call("save_place"))[:160])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], *(int(a) if i == 0 else a for i, a in enumerate(sys.argv[2:]))))
