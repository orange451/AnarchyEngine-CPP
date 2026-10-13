"""Swaps new geometry into the existing vegetation prefabs in an open studio.

After a re-split (e.g. new pivots), imports the OBJs again, points each existing
Prefab's Models at the freshly imported Meshes (matched by material name), then
removes the extra Prefabs, their Materials and the old Meshes. Placed GameObjects
keep their Prefabs; shared Materials are untouched. Verifies and saves.

Usage: py -m tools.vegetation.reimport_meshes <split_dir> [port] [token]
"""
import json
import os
import sys

from .import_vegetation import HERE, batches, check_report, import_files, lua_table, parse_verify
from .mcp_client import Studio

SWAP = r'''
-- Destroy alone orphans an instance's children (they stay alive and hold slots).
local function destroyTree(i) for _, c in ipairs(i:GetChildren()) do destroyTree(c) end i:Destroy() end
local NAMES = __NAMES__
local assets = game:GetService("Assets")
local prefabRoot = assets.Prefabs.Vegetation
local meshRoot = assets.Meshes.Vegetation
local history = game:GetService("ChangeHistoryService")
local rec = history:TryBeginRecording("Swap vegetation meshes")
local current = {}
for _, family in ipairs(prefabRoot:GetChildren()) do
	for _, p in ipairs(family:GetChildren()) do current[p.Name] = p end
end
local swapped, problems = 0, 0
for _, name in ipairs(NAMES) do
	local fresh = assets.Prefabs:FindFirstChild(name)
	local keep = current[name]
	if fresh == nil or keep == nil then
		problems += 1
		print("MISSING", name)
	else
		local byMat = {}
		for _, m in ipairs(fresh:GetChildren()) do byMat[m.Name] = m end
		for _, m in ipairs(keep:GetChildren()) do
			local f = byMat[m.Name]
			if f == nil then problems += 1; print("NOMATCH", name, m.Name) else m.Mesh = f.Mesh; swapped += 1 end
		end
		local oldMeshes = meshRoot:FindFirstChild(name)
		if oldMeshes then destroyTree(oldMeshes) end
		local newMeshes = assets.Meshes:FindFirstChild(name)
		if newMeshes then newMeshes.Parent = meshRoot end
		local mats = assets.Materials:FindFirstChild(name)
		if mats then destroyTree(mats) end
		destroyTree(fresh)
	end
end
if rec then history:FinishRecording(rec, Enum.FinishRecordingOperation.Commit) end
print("SWAP", swapped, problems)
'''


def main(split_dir, port=8765, token="veg") -> int:
    s = Studio(port, token)
    s.initialize()
    if s.call("playtest", action="status").get("session") != "stopped":
        print("A test is running in the studio; stop it first.")
        return 1
    with open(os.path.join(split_dir, "manifest.json")) as f:
        manifest = json.load(f)
    with open(os.path.join(split_dir, "textures", "textures.json")) as f:
        textures = json.load(f)
    names = [e["name"] for e in manifest]
    for b in batches([os.path.join(split_dir, n + ".obj") for n in names]):
        print("imported:", len(import_files(s, b)), flush=True)
    for b in batches(names, 32):
        out = s.lua(SWAP.replace("__NAMES__", lua_table(b)))
        print(" ".join(l.replace("\t", " ") for l in out if l.startswith(("SWAP", "MISSING", "NOMATCH"))), flush=True)
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
