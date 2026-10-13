import json

from PIL import Image

from tools.vegetation.prepare_textures import channel_is_flat, prepare, read_mtl_maps


def png(path, pixel):  # pixel: (x, y) -> (r, g, b, a) over 4x4
    im = Image.new("RGBA", (4, 4))
    for y in range(4):
        for x in range(4):
            im.putpixel((x, y), pixel(x, y))
    im.save(path)


def manifest(tmp_path, materials):
    m = tmp_path / "manifest.json"
    m.write_text(json.dumps([{"name": "A", "materials": materials}]))
    return str(m)


def test_channel_is_flat(tmp_path):
    p = tmp_path / "a.png"
    png(p, lambda x, y: (x * 40, 128, 0, 255))
    im = Image.open(p)
    assert channel_is_flat(im, 1) and not channel_is_flat(im, 0)


def test_read_mtl_maps(tmp_path):
    p = tmp_path / "t.mtl"
    p.write_text("newmtl A\nmap_Kd Trunk10_diff.png\nmap_Bump Log9_nmap.png\nmap_Ns Log9_spec.png\n"
                 "newmtl B\nmap_Kd Trunk10_diff.png\nmap_Bump Other_nmap.png\n")
    # The first material that names an albedo decides its normal and spec maps.
    assert read_mtl_maps(str(p)) == {"Trunk10_diff.png": {"normal": "Log9_nmap.png", "spec": "Log9_spec.png"}}


def test_flat_green_textured_red(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    png(src / "Trunk9_diff.png", lambda x, y: (100, 80, 60, 255))
    png(src / "Trunk9_nmap.png", lambda x, y: (128, 128, 255, 255))
    png(src / "Trunk9_spec.png", lambda x, y: (x * 60, 128, 0, 255))      # R textured, G flat
    png(src / "Leaves9_diff.png", lambda x, y: (0, 120, 0, 255 if x else 0))
    png(src / "Leaves9_spec.png", lambda x, y: (0, 128, 0, 255))
    mtl = tmp_path / "t.mtl"
    mtl.write_text("newmtl T\nmap_Kd Trunk9_diff.png\nmap_Bump Trunk9_nmap.png\nmap_Ns Trunk9_spec.png\n"
                   "newmtl L\nmap_Kd Leaves9_diff.png\nmap_Ns Leaves9_spec.png\n")
    out = tmp_path / "out"
    result = prepare(str(src), str(mtl), manifest(tmp_path, ["Bark_Trunk9", "Leaf_Leaves9"]), str(out))
    assert result["Bark_Trunk9"] == {"Color": "Veg_Trunk9_Color.png", "Normal": "Veg_Trunk9_Normal.png",
                                     "Roughness": None, "Metalness": "Veg_Trunk9_Metalness.png"}
    assert result["Leaf_Leaves9"] == {"Color": "Veg_Leaves9_Color.png", "Normal": None,
                                      "Roughness": None, "Metalness": None}
    assert Image.open(out / "Veg_Trunk9_Metalness.png").mode == "L"
    assert Image.open(out / "Veg_Leaves9_Color.png").mode == "RGBA"
    assert json.load(open(out / "textures.json")) == result


def test_shared_normal_written_once(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    png(src / "Trunk10_diff.png", lambda x, y: (90, 70, 50, 255))
    png(src / "Log9_diff.png", lambda x, y: (80, 60, 40, 255))
    png(src / "Log9_nmap.png", lambda x, y: (128, 128, 255, 255))
    png(src / "Log9_spec.png", lambda x, y: (0, 40 + x * 30, 0, 255))     # G textured
    mtl = tmp_path / "t.mtl"
    mtl.write_text("newmtl A\nmap_Kd Trunk10_diff.png\nmap_Bump Log9_nmap.png\nmap_Ns Log9_spec.png\n"
                   "newmtl B\nmap_Kd Log9_diff.png\nmap_Bump Log9_nmap.png\nmap_Ns Log9_spec.png\n")
    out = tmp_path / "out"
    result = prepare(str(src), str(mtl), manifest(tmp_path, ["Bark_Trunk10", "Log_Log9"]), str(out))
    assert result["Bark_Trunk10"]["Normal"] == result["Log_Log9"]["Normal"] == "Veg_Log9_Normal.png"
    assert result["Bark_Trunk10"]["Roughness"] == result["Log_Log9"]["Roughness"] == "Veg_Log9_Roughness.png"
    pngs = sorted(p.name for p in out.glob("*.png"))
    assert pngs == ["Veg_Log9_Color.png", "Veg_Log9_Normal.png", "Veg_Log9_Roughness.png", "Veg_Trunk10_Color.png"]
