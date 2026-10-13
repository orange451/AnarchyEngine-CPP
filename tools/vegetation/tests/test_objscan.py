from tools.vegetation.objscan import bounds, scan_groups


def test_groups_and_materials(tiny_obj):
    groups = list(scan_groups(tiny_obj))
    assert [g.name for g in groups] == ["Trunk1", "Leaves1", "Leaves2"]
    assert [g.material for g in groups] == ["Trunk1Mtl", "Leaves1Mtl", "Leaves1Mtl"]
    assert len(groups[0].v) == 3 and len(groups[1].v) == 4 and len(groups[2].v) == 3


def test_face_index_forms(tiny_obj):
    trunk, leaves1, leaves2 = list(scan_groups(tiny_obj))
    assert trunk.faces == [[(0, 0, 0), (1, 1, 0), (2, 2, 0)]]
    # v/vt form: global v 4 -> local 0, global vt 4 -> local 0
    assert leaves1.faces[0] == [(0, 0, None), (1, 1, None), (2, 0, None)]
    # negative indices count back from the last vertex written so far
    assert leaves1.faces[1] == [(3, None, None), (2, None, None), (1, None, None)]
    # v//vn form: global vn 2 -> local 0 (this group's only normal)
    assert leaves2.faces[0] == [(0, None, 0), (1, None, 0), (2, None, 0)]


def test_bounds(tiny_obj):
    g = list(scan_groups(tiny_obj))[1]
    assert bounds(g) == (0.0, 2.0, -60.0, 1.0, 3.0, -60.0)


def test_vertices_before_any_group(tmp_path):
    p = tmp_path / "nogroup.obj"
    p.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
    (g,) = list(scan_groups(str(p)))
    assert g.name == "default" and g.faces == [[(0, None, None), (1, None, None), (2, None, None)]]
