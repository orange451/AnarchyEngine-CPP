from tools.vegetation.catalog import GROUND_Y, GroupInfo, build_catalog, read_mtl_albedos


def gi(name, mat, minx, miny, minz, maxx, maxy, maxz, faces=100):
    return GroupInfo(name, mat, (minx, miny, minz, maxx, maxy, maxz), faces)


ALBEDO = {"T1": "Trunk1_diff.png", "T2": "Trunk62_diff.png", "L22": "Leaves22_diff.png",
          "L2": "Leaves2_diff.png", "L3": "Leaves3_diff.png", "L1": "Leaves1_diff.png", "LG": "Log1_diff.png"}


def spruce_row():
    # Row z = -630, trunks at x = -90, -60, -30, 0. Trunk1 has no crown (dead).
    return [gi("Trunk1", "T2", -98, -2, -642, -82, 58, -626),
            gi("Trunk2", "T1", -68, -2, -642, -52, 48, -626),
            gi("Trunk3", "T1", -38, -2, -642, -22, 38, -626),
            gi("Trunk4", "T1", -8, -2, -642, 8, 28, -626),
            gi("Leaves10", "L22", -75, 20, -650, -45, 50, -618),
            gi("Leaves11", "L22", -75, 20, -650, -45, 50, -618),
            gi("Leaves12", "L22", -45, 15, -648, -15, 40, -620),
            gi("Leaves13", "L22", -15, 10, -646, 15, 30, -622)]


def test_tree_ages_and_dead():
    by = {a.name: a for a in build_catalog(spruce_row(), ALBEDO)}
    # Three leafy trees: old = ceil(3/2) = 2, young = 1 (ledgered ruling).
    assert set(by) == {"Spruce-Dead", "Spruce-Old-Var01", "Spruce-Old-Var02", "Spruce-Young-Var01"}
    assert by["Spruce-Dead"].groups == ["Trunk1"] and by["Spruce-Dead"].age == "Dead"
    assert by["Spruce-Old-Var01"].groups == ["Trunk2", "Leaves10", "Leaves11"]
    assert by["Spruce-Old-Var01"].pivot == (-60.0, GROUND_Y, -634.0)   # the ground plane, not the trunk's base
    assert by["Spruce-Young-Var01"].trunk == "Trunk4"


def test_ages_by_thirds_for_six_or_more():
    rows = []
    for i in range(7):  # seven leafy oaks, tallest first
        x = i * 40
        rows.append(gi(f"Trunk{i + 1}", "T1", x - 5, 0, -65, x + 5, 70 - i * 5, -55))
        rows.append(gi(f"Leaves{i + 1}", "L3", x - 15, 20, -75, x + 15, 70 - i * 5, -45))
    ages = sorted(a.name for a in build_catalog(rows, ALBEDO))
    # old = ceil(7/3) = 3, young = 7 // 3 = 2, adult = 2
    assert ages == ["Oak-Adult-Var01", "Oak-Adult-Var02", "Oak-Old-Var01", "Oak-Old-Var02",
                    "Oak-Old-Var03", "Oak-Young-Var01", "Oak-Young-Var02"]


def test_crown_goes_to_nearest_trunk():
    rows = spruce_row()
    # A crown centred between Trunk3 (x=-30) and Trunk4 (x=0), nearer Trunk4, high off the ground.
    rows.append(gi("Leaves99", "L22", -28, 12, -646, 2, 40, -622))
    owner = [a for a in build_catalog(rows, ALBEDO) if "Leaves99" in a.groups]
    assert len(owner) == 1 and owner[0].trunk == "Trunk4"


def test_unknown_row_is_an_error():
    import pytest
    with pytest.raises(ValueError, match="unknown row"):
        build_catalog([gi("Trunk1", "T1", -5, 0, 995, 5, 30, 1005)], ALBEDO)


def test_bushes_cards_and_logs():
    rows = spruce_row() + [
        gi("Leaves20", "L2", 268, -2.3, -9, 284, 13.7, 9),          # Bush-Sycamore big
        gi("Leaves21", "L2", 270, -2.6, -6, 282, 6.8, 4),           # Bush-Sycamore small
        gi("Leaves22", "L3", 266, -2.1, -9, 283, 12.5, 9),          # Bush-Oak
        gi("Leaves23", "L1", 268, -2.3, -10, 284, 13.1, 8),         # BerryBush
        gi("Leaves24", "L1", 222, -1.0, -4.5, 232.5, 6.1, 4.3),     # BerryBush small
        gi("Leaves25", "L2", 273, -0.15, -2, 278, 6.05, 2, faces=8),  # Card
        gi("Log1", "LG", 340, 0, -190, 346, 2.2, -186),             # ground log
        gi("Log2", "T1", -68, -2, -700, -52, 44, -690),             # far from every trunk: ground log
        gi("Log3", "T1", -66, 0, -640, -54, 45, -628),              # inside Trunk2's footprint: chop log
        gi("Strump1", "T1", -66, -2, -640, -54, 3, -628)]
    cat = build_catalog(rows, ALBEDO)
    names = {a.name for a in cat}
    assert {"Bush-Sycamore-Var01", "Bush-Sycamore-Var02", "Bush-Oak-Var01", "BerryBush-Var01",
            "BerryBush-Var02", "Card-Var01", "GroundLog-Var01", "GroundLog-Var02"} <= names
    assert not any("Log3" in a.groups or "Strump1" in a.groups for a in cat)
    by = {a.name: a for a in cat}
    assert by["Bush-Sycamore-Var01"].groups == ["Leaves20"]
    assert by["Bush-Sycamore-Var01"].pivot == (276.0, GROUND_Y, 0.0)
    assert by["Bush-Sycamore-Var01"].family == "Bush"
    assert by["BerryBush-Var02"].groups == ["Leaves24"] and by["BerryBush-Var02"].family == "BerryBush"
    assert by["Card-Var01"].family == "Card"
    assert by["GroundLog-Var01"].groups == ["Log2"]          # largest first
    assert by["GroundLog-Var02"].pivot == (343.0, 0.0, -188.0)


def test_read_mtl_albedos(tmp_path):
    p = tmp_path / "t.mtl"
    p.write_text("newmtl A\nmap_Kd Leaves1_diff.png\nnewmtl B\nKs 0 0 0\n")
    assert read_mtl_albedos(str(p)) == {"A": "Leaves1_diff.png"}
