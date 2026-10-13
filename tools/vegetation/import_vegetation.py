"""Drives AnarchyStudio over MCP: copies the project, imports the split assets and
textures, runs fix_materials.luau, verifies, and saves only if verification passes.

Usage:
  py -m tools.vegetation.import_vegetation --project-src "<...>\\H1Z1 Z1" \\
     --project-dst "<...>\\H1Z1 Z1 Vegetation" --exe <AnarchyStudio.exe> \\
     --split-dir "C:\\Users\\Andrew\\Desktop\\Trees\\split" [--appdata <scratch>] [--reuse] [--keep-open]
"""
import argparse
import glob
import json
import os
import sys

from .mcp_client import McpError, Studio, copy_project, launch_studio

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_TEXTURES = 48
SIZE_TOLERANCE = 0.01   # relative, plus an absolute 0.02 units


def batches(items, n=64):
    return [items[i:i + n] for i in range(0, len(items), n)]


def lua_table(value) -> str:
    if isinstance(value, dict):
        return "{" + ",".join(f'["{k}"]={lua_table(v)}' for k, v in value.items() if v is not None) + "}"
    if isinstance(value, list):
        return "{" + ",".join(lua_table(v) for v in value) + "}"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_verify(lines, manifest) -> dict:
    size_of = {e["name"]: [e["bounds"][i + 3] - e["bounds"][i] for i in range(3)] for e in manifest}
    models_of = {e["name"]: len(e["materials"]) for e in manifest}
    report = {"prefabs": 0, "bounds_mismatch": [], "model_mismatch": [], "textures": None, "materials": None,
              "stray": None}
    for line in lines:
        p = line.split()
        if not p:
            continue
        if p[0] == "BOUNDS":
            report["prefabs"] += 1
            name, got = p[1], [float(x) for x in p[2:5]]
            expected = size_of.get(name)
            if expected is None or any(abs(g - e) > SIZE_TOLERANCE * e + 0.02 for g, e in zip(got, expected)):
                report["bounds_mismatch"].append({"name": name, "got": got, "expected": expected})
            # One Model per material: fewer means the importer merged materials into one mesh.
            if int(p[5]) != models_of.get(name):
                report["model_mismatch"].append({"name": name, "got": int(p[5]), "expected": models_of.get(name)})
        elif p[0] in ("TEXTURES", "MATERIALS", "STRAY"):
            report[p[0].lower()] = int(p[1])
    return report


def check_report(report, n_prefabs: int, n_materials: int) -> bool:
    return (report["prefabs"] == n_prefabs and report["materials"] == n_materials
            and report["textures"] is not None and report["textures"] <= MAX_TEXTURES
            and report["stray"] == 0 and not report["bounds_mismatch"] and not report["model_mismatch"])


def import_files(s: Studio, files):
    r = s.call("import_assets", files=files)
    rows = r.get("files", [])
    bad = [row for row in rows if row.get("error")]
    if bad or r.get("failed") or len(rows) != len(files):
        raise McpError("import_assets failed: " + json.dumps(bad or r)[:2000])
    notes = [n if isinstance(n, str) else json.dumps(n) for row in rows for n in row.get("left_out", [])]
    if any("texture" in n.lower() for n in notes):
        raise McpError("the importer looked for textures: " + "; ".join(notes)[:2000])
    return rows


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--project-src", required=True)
    ap.add_argument("--project-dst", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--split-dir", required=True)
    ap.add_argument("--appdata", help="scratch APPDATA for the studio, so the user's preferences stay untouched")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", default="veg")
    ap.add_argument("--reuse", action="store_true", help="use an existing project-dst instead of copying")
    ap.add_argument("--keep-open", action="store_true", help="leave the studio running afterwards")
    a = ap.parse_args(argv)

    if not (a.reuse and os.path.exists(a.project_dst)):
        copy_project(a.project_src, a.project_dst, os.path.basename(a.project_dst.rstrip("\\/")))
    if a.appdata:
        os.environ["APPDATA"] = a.appdata
    proc = launch_studio(os.path.abspath(a.exe), a.project_dst, a.port, a.token)
    ok = False
    try:
        s = Studio(a.port, a.token)
        s.wait_until_ready(180)
        with open(os.path.join(a.split_dir, "manifest.json")) as f:
            manifest = json.load(f)
        with open(os.path.join(a.split_dir, "textures", "textures.json")) as f:
            textures = json.load(f)

        tex_files = sorted(glob.glob(os.path.join(a.split_dir, "textures", "*.png")))
        for b in batches(tex_files):
            import_files(s, b)
        print("textures imported:", len(tex_files), flush=True)
        objs = [os.path.join(a.split_dir, e["name"] + ".obj") for e in manifest]
        for b in batches(objs):
            print("prefabs imported:", len(import_files(s, b)), flush=True)

        with open(os.path.join(HERE, "fix_materials.luau")) as f:
            lua = f.read()
        lua = (lua.replace("__TEXTURES__", lua_table(textures))
                  .replace("__ASSET_MATERIALS__", lua_table({e["name"]: e["materials"] for e in manifest}))
                  .replace("__FAMILIES__", lua_table({e["name"]: e["family"] for e in manifest})))
        out = s.lua(lua)
        done = [line for line in out if line.startswith("FIX_DONE")]
        if not done:
            raise McpError("the material pass did not finish:\n" + "\n".join(out[-20:]))
        print(done[0].replace("\t", " "), flush=True)

        with open(os.path.join(HERE, "verify_import.luau")) as f:
            report = parse_verify(s.lua(f.read()), manifest)
        report["fix"] = done[0].split()
        with open(os.path.join(a.split_dir, "import_report.json"), "w") as f:
            json.dump(report, f, indent=1)
        print(json.dumps({k: v for k, v in report.items() if not k.endswith("_mismatch")}),
              "bounds mismatches:", len(report["bounds_mismatch"]),
              "model mismatches:", len(report["model_mismatch"]), flush=True)
        ok = check_report(report, n_prefabs=len(manifest), n_materials=len(textures))
        if ok:
            print("saved:", json.dumps(s.call("save_place"))[:300], flush=True)
        else:
            print("NOT SAVED: verification failed, see import_report.json", flush=True)
    finally:
        if a.keep_open and ok:
            print("studio left running on port", a.port, "pid", proc.pid)
        else:
            proc.terminate()
            proc.wait(30)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
