"""Places each species row / bush family beside the house, screenshots it, then
removes the gallery and saves. Needs the studio from import_vegetation --keep-open.

Usage: py -m tools.vegetation.gallery <split_dir> [port] [token]
"""
import json
import os
import sys
import time

from .import_vegetation import lua_table
from .mcp_client import Studio

HERE = os.path.dirname(os.path.abspath(__file__))
ORIGIN = (1010.0, 42.0, -660.0)   # beside the House brushes at (990, 41.75, -694)
AGE_ORDER = {"Old": 0, "Adult": 1, "Young": 2, "Dead": 3}


def rows(manifest) -> dict:
    out = {}
    for e in manifest:
        key = e["species"] if e["family"] == "Tree" else e["name"].rsplit("-Var", 1)[0]
        out.setdefault(key, []).append(e)
    for es in out.values():
        es.sort(key=lambda e: (AGE_ORDER.get(e["age"], 9), e["name"]))
    return out


def main(split_dir, port=8765, token="veg"):
    s = Studio(port, token)
    s.initialize()
    with open(os.path.join(split_dir, "manifest.json")) as f:
        manifest = json.load(f)
    with open(os.path.join(HERE, "gallery.luau")) as f:
        lua = f.read()
    out_dir = os.path.join(split_dir, "gallery")
    os.makedirs(out_dir, exist_ok=True)
    for key, es in rows(manifest).items():
        src = (lua.replace("__ROW__", lua_table([e["name"] for e in es]))
                  .replace("__SPACING__", lua_table([max(2.4 * e["crownRadius"], 2.0) for e in es]))
                  .replace("__ORIGIN__", ", ".join(repr(v) for v in ORIGIN))
                  .replace("__TALLEST__", repr(max(e["height"] for e in es))))
        print(key, s.lua(src)[-1].replace("\t", " "), flush=True)
        time.sleep(4)   # let new textures stream in before the capture
        png = s.screenshot(1600)
        with open(os.path.join(out_dir, f"{key}.png"), "wb") as f:
            f.write(png)
    print(s.lua('local function destroyTree(i) for _, c in ipairs(i:GetChildren()) do destroyTree(c) end i:Destroy() end; '
                'local g = workspace:FindFirstChild("VegetationGallery"); if g then destroyTree(g) end; print("CLEANED")'))
    print("saved:", json.dumps(s.call("save_place"))[:200])


if __name__ == "__main__":
    main(sys.argv[1], *(int(a) if i == 0 else a for i, a in enumerate(sys.argv[2:])))
