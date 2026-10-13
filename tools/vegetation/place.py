"""Places (part of) a vegetation plan into the open studio over MCP.

Takes the plan instances nearest a centre point (a square window, Chebyshev
distance) up to a count, so a slice fits under the place's instance cap, and
creates one GameObject per instance in Workspace.Vegetation/{Trees,Undergrowth},
1,000 per run_lua call, each call one undo step. Replaces an earlier placement.

Usage: py -m tools.vegetation.place <plan_dir> --center X Z --count N [--port 8765] [--save]
"""
import argparse
import json
import os
import sys

import numpy as np

from .import_vegetation import lua_table
from .mcp_client import Studio

DT = np.dtype([("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("yaw", "<f4"), ("tx", "<f4"), ("tz", "<f4"),
               ("s", "<f4"), ("a", "<u2"), ("c", "u1"), ("k", "u1")])

SETUP = r'''
local root = game:GetService("Assets").Prefabs.Vegetation
local byName = {}
for _, family in ipairs(root:GetChildren()) do
	for _, p in ipairs(family:GetChildren()) do byName[p.Name] = p end
end
local old = workspace:FindFirstChild("Vegetation")
if old then old:Destroy() end
local veg = Instance.new("Folder", workspace)
veg.Name = "Vegetation"
local t = Instance.new("Folder", veg); t.Name = "Trees"
local u = Instance.new("Folder", veg); u.Name = "Undergrowth"
local missing = 0
for _, name in ipairs(__NAMES__) do if byName[name] == nil then missing += 1 end end
print("SETUP", missing)
'''

CHUNK = r'''
local root = game:GetService("Assets").Prefabs.Vegetation
local byName = {}
for _, family in ipairs(root:GetChildren()) do
	for _, p in ipairs(family:GetChildren()) do byName[p.Name] = p end
end
local NAMES = __NAMES__
local ROWS = __ROWS__
local veg = workspace.Vegetation
local history = game:GetService("ChangeHistoryService")
local rec = history:TryBeginRecording("Place vegetation")
local made = 0
for _, r in ipairs(ROWS) do
	local name = NAMES[r[1] + 1]
	local go = Instance.new("GameObject")
	go.Name = name
	go.Prefab = byName[name]
	go.Transform = Matrix4.new(r[2], r[3], r[4]) * Matrix4.fromEulerAngles(r[6], r[5], r[7])
	go.Scale = r[8]
	go.Parent = r[9] <= 1 and veg.Trees or veg.Undergrowth
	made += 1
end
if rec then history:FinishRecording(rec, Enum.FinishRecordingOperation.Commit) end
print("MADE", made)
'''


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("plan_dir")
    ap.add_argument("--center", nargs=2, type=float, required=True)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", default="veg")
    ap.add_argument("--save", action="store_true")
    a = ap.parse_args(argv)

    meta = json.load(open(os.path.join(a.plan_dir, "plan.json")))
    names = meta["assets"]
    rec = np.fromfile(os.path.join(a.plan_dir, "plan.bin"), dtype=DT)
    cx, cz = a.center
    d = np.maximum(np.abs(rec["x"] - cx), np.abs(rec["z"] - cz))
    pick = np.argsort(d, kind="stable")[:a.count]
    sub = rec[pick]
    half = float(d[pick].max())
    kinds = np.bincount(sub["k"], minlength=6)
    print(f"window {2 * half:.0f} units square around ({cx:.0f}, {cz:.0f}): {len(sub)} instances; "
          f"trees {kinds[0]}, dead {kinds[1]}, bushes {kinds[2]}, saplings {kinds[3]}, berry {kinds[4]}, logs {kinds[5]}",
          flush=True)

    s = Studio(a.port, a.token)
    s.initialize()
    if s.call("playtest", action="status").get("session") != "stopped":
        print("A test is running in the studio; stop it first.")
        return 1
    out = s.lua(SETUP.replace("__NAMES__", lua_table(names)))
    print(out[-1].replace("\t", " "), flush=True)
    total = 0
    for i in range(0, len(sub), 1000):
        rows = [[int(r["a"]), round(float(r["x"]), 3), round(float(r["y"]), 3), round(float(r["z"]), 3),
                 round(float(r["yaw"]), 4), round(float(r["tx"]), 4), round(float(r["tz"]), 4),
                 round(float(r["s"]), 4), int(r["k"])] for r in sub[i:i + 1000]]
        src = CHUNK.replace("__NAMES__", lua_table(names)).replace("__ROWS__", lua_table(rows))
        made = [l for l in s.lua(src) if l.startswith("MADE")]
        total += int(made[0].split()[1]) if made else 0
        print(f"placed {total}/{len(sub)}", flush=True)
    if a.save:
        print("saved:", json.dumps(s.call("save_place"))[:160])
    return 0 if total == len(sub) else 1


if __name__ == "__main__":
    sys.exit(main())
