import json

from PIL import Image

from tools.vegetation.prepare_textures import prepare, read_mtl_maps


def png(path, pixel):  # pixel: (x, y) -> (r, g, b, a) over 4x4
    im = Image.new("RGBA", (4, 4))
    for y in range(4):
        for x in range(4):
            im.putpixel((x, y), pixel(x, y))
    im.save(path)


def test_read_mtl_maps(tmp_path):
    p = tmp_path / "t.mtl"
    p.write_text("newmtl A\nmap_Kd Trunk10_diff.png\nmap_Bump Log9_nmap.png\nmap_Ns Log9_spec.png\n"
                 "newmtl B\nmap_Kd Trunk10_diff.png\nmap_Bump Other_nmap.png\nmap_Ns Other_spec.png\n")
    assert read_mtl_maps(str(p)) == {"Trunk10_diff.png": {"normal": "Log9_nmap.png",
                                                          "specs": ["Log9_spec.png", "Other_spec.png"]}}


def test_scalars_maps_and_shared_files(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    png(src / "Trunk10_diff.png", lambda x, y: (90, 70, 50, 255))
    png(src / "Log9_diff.png", lambda x, y: (80, 60, 40, 255))
    png(src / "Log9_nmap.png", lambda x, y: (128, 128, 255, 255))
    png(src / "Log9_spec.png", lambda x, y: (0, 127, 0, 255))                  # flat: scalar 0.498
    png(src / "Leaves1_diff.png", lambda x, y: (0, 120, 0, 255))
    png(src / "Leaves1_spec.png", lambda x, y: (0, 127, 0, 255))               # placeholder
    png(src / "Leaves138_spec.png", lambda x, y: (0, 0 if x == 0 else 240, 0, 255))  # textured: a map
    mtl = tmp_path / "t.mtl"
    mtl.write_text("newmtl A\nmap_Kd Trunk10_diff.png\nmap_Bump Log9_nmap.png\nmap_Ns Log9_spec.png\n"
                   "newmtl B\nmap_Kd Log9_diff.png\nmap_Bump Log9_nmap.png\nmap_Ns Log9_spec.png\n"
                   "newmtl C\nmap_Kd Leaves1_diff.png\nmap_Ns Leaves1_spec.png\n"
                   "newmtl D\nmap_Kd Leaves1_diff.png\nmap_Ns Leaves138_spec.png\n")
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps([{"name": "A", "materials": ["Bark_Trunk10", "Log_Log9", "Leaf_Leaves1"]}]))
    out = tmp_path / "out"
    r = prepare(str(src), str(mtl), str(manifest), str(out))
    assert r["Bark_Trunk10"]["Normal"] == r["Log_Log9"]["Normal"] == "Veg_Log9_Normal.png"
    assert r["Bark_Trunk10"]["RoughnessValue"] == 0.498 and r["Bark_Trunk10"]["Roughness"] is None
    assert r["Leaf_Leaves1"]["Roughness"] == "Veg_Leaves138_Roughness.png"
    assert r["Leaf_Leaves1"]["MetalnessValue"] == 0.0
    assert Image.open(out / "Veg_Leaves138_Roughness.png").mode == "L"
    assert sorted(p.name for p in out.glob("*.png")) == [
        "Veg_Leaves138_Roughness.png", "Veg_Leaves1_Color.png", "Veg_Log9_Color.png", "Veg_Log9_Normal.png",
        "Veg_Trunk10_Color.png"]
    assert json.load(open(out / "textures.json")) == r
