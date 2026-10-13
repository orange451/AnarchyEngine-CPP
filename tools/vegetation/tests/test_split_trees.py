import json

from tools.vegetation.catalog import Asset
from tools.vegetation.objscan import Group
from tools.vegetation.split_trees import SCALE, material_key, split, write_asset


def tri(v):
    return Group("x", None, v=v, faces=[[(0, None, None), (1, None, None), (2, None, None)]])


def test_material_key():
    assert material_key("Leaves3_diff.png") == "Leaf_Leaves3"
    assert material_key("Trunk27_diff.png") == "Bark_Trunk27"
    assert material_key("Log9_diff.png") == "Log_Log9"


def test_write_tree_pivot_scale_and_reindex(tmp_path):
    trunk = Group("Trunk1", "T1", v=[(10, 5, 0), (12, 5, 0), (11, 25, 0)], vt=[(0, 0), (1, 0), (0, 1)],
                  faces=[[(0, 0, None), (1, 1, None), (2, 2, None)]])
    leaves = Group("Leaves1", "L3", v=[(5, 20, -5), (17, 20, 5), (11, 30, 0)], vt=[(0, 0), (1, 0), (0, 1)],
                   faces=[[(0, 0, None), (1, 1, None), (2, 2, None)]])
    flipped = Group("Leaves2", "L3", v=list(leaves.v), vt=list(leaves.vt),
                    faces=[[(2, 2, None), (1, 1, None), (0, 0, None)]])
    asset = Asset("Oak-Old-Var01", "Tree", "Oak", "Old", "Trunk1", ["Trunk1", "Leaves1", "Leaves2"], (11.0, 5.0, 0.0))
    entry = write_asset(asset, {"Trunk1": trunk, "Leaves1": leaves, "Leaves2": flipped},
                        {"T1": "Trunk27_diff.png", "L3": "Leaves3_diff.png"}, str(tmp_path))
    text = (tmp_path / "Oak-Old-Var01.obj").read_text()
    assert "mtllib Oak-Old-Var01.mtl" in text
    assert text.count("usemtl Bark_Trunk27") == 1 and text.count("usemtl Leaf_Leaves3") == 2
    # the trunk's base vertex sits one unit left of the pivot, at ground height, scaled
    assert "v -0.2300 0.0000 0.0000" in text
    # faces of later groups are re-indexed after the earlier groups' vertices
    assert "f 4/4 5/5 6/6" in text and "f 9/9 8/8 7/7" in text
    mtl = (tmp_path / "Oak-Old-Var01.mtl").read_text()
    assert "map_" not in mtl and mtl.count("newmtl") == 2
    assert entry["materials"] == ["Bark_Trunk27", "Leaf_Leaves3"]
    assert abs(entry["height"] - 25 * SCALE) < 1e-6
    assert abs(entry["crownRadius"] - (6 ** 2 + 5 ** 2) ** 0.5 * SCALE) < 1e-6
    assert abs(entry["crownBase"] - 15 * SCALE) < 1e-6
    assert abs(entry["trunkRadius"] - 1 * SCALE) < 1e-6
    assert abs(entry["bounds"][1]) < 1e-9


def test_bush_height_excludes_stem_below_ground(tmp_path):
    # Stem reaches 2 export units below the ground plane; leaves reach 10 above it.
    g = tri([(0, -2.15, 0), (1, -0.15, 0), (0, 9.85, 1)])
    g.name, g.material = "Leaves7", "LA"
    asset = Asset("Bush-Oak-Var01", "Bush", None, None, None, ["Leaves7"], (0.0, -0.15, 0.0))
    entry = write_asset(asset, {"Leaves7": g}, {"LA": "Leaves3_diff.png"}, str(tmp_path))
    assert abs(entry["height"] - 10 * SCALE) < 1e-6
    assert abs(entry["bounds"][1] - (-2 * SCALE)) < 1e-6   # the stem stays in the mesh and the bounds
    assert entry["crownBase"] == 0.0 and entry["trunkRadius"] == 0.0


def test_dead_tree_has_no_crown(tmp_path):
    g = tri([(0, 0, 0), (2, 0, 0), (1, 40, 0)])
    g.name, g.material = "Trunk9", "T1"
    asset = Asset("Oak-Dead", "Tree", "Oak", "Dead", "Trunk9", ["Trunk9"], (1.0, 0.0, 0.0))
    entry = write_asset(asset, {"Trunk9": g}, {"T1": "Trunk33_diff.png"}, str(tmp_path))
    assert entry["crownBase"] == 0.0 and entry["crownRadius"] == 0.0
    assert abs(entry["height"] - 40 * SCALE) < 1e-6


def test_same_albedo_groups_share_material(tmp_path):
    a = Group("Leaves1", "LA", v=[(0, 0, 0), (1, 0, 0), (0, 1, 0)], faces=[[(0, None, None), (1, None, None), (2, None, None)]])
    b = Group("Leaves2", "LB", v=[(0, 0, 1), (1, 0, 1), (0, 1, 1)], faces=[[(0, None, None), (1, None, None), (2, None, None)]])
    asset = Asset("Bush-Oak-Var01", "Bush", None, None, None, ["Leaves1", "Leaves2"], (0.5, -0.15, 0.5))
    entry = write_asset(asset, {"Leaves1": a, "Leaves2": b}, {"LA": "Leaves3_diff.png", "LB": "Leaves3_diff.png"},
                        str(tmp_path))
    assert entry["materials"] == ["Leaf_Leaves3"]
    assert (tmp_path / "Bush-Oak-Var01.mtl").read_text().count("newmtl") == 1


def test_split_end_to_end(tiny_obj, tmp_path):
    # tiny_obj: Trunk1 + its crown Leaves1 in the Oak row; Leaves2 stands free on the ground (a bush).
    mtl = tmp_path / "tiny.mtl"
    mtl.write_text("newmtl Trunk1Mtl\nmap_Kd Trunk27_diff.png\nnewmtl Leaves1Mtl\nmap_Kd Leaves3_diff.png\n")
    out = tmp_path / "split"
    entries = split(tiny_obj, str(mtl), str(out))
    assert sorted(e["name"] for e in entries) == ["Bush-Oak-Var01", "Oak-Old-Var01"]
    assert json.load(open(out / "manifest.json")) == entries
    assert {p.name for p in out.iterdir()} >= {"Oak-Old-Var01.obj", "Oak-Old-Var01.mtl", "Bush-Oak-Var01.obj",
                                                "manifest.json"}
