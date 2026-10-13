from tools.vegetation.import_vegetation import batches, check_report, lua_table, parse_verify


def test_batches():
    assert batches(list(range(130)), 64) == [list(range(64)), list(range(64, 128)), [128, 129]]
    assert batches([], 64) == []


def test_lua_table():
    assert lua_table({"A": {"Color": "a.png", "Normal": None}, "B": ["x", "y"]}) == \
        '{["A"]={["Color"]="a.png"},["B"]={"x","y"}}'
    assert lua_table([1.5, 2]) == "{1.5,2}"
    assert lua_table('say "hi"\\') == '"say \\"hi\\"\\\\"'


MANIFEST = [{"name": "Oak-Old-Var01", "bounds": [-7, 0, -7, 7, 18.6, 7], "materials": ["Bark_Trunk27", "Leaf_Leaves3"]},
            {"name": "Bush-Oak-Var01", "bounds": [-2, -0.5, -2, 2, 2.9, 2], "materials": ["Leaf_Leaves3"]}]


def test_merged_materials_are_caught():
    # One Model where the manifest names two materials: bark and leaves merged on import.
    lines = ["BOUNDS\tOak-Old-Var01\t14\t18.6\t14\t1", "BOUNDS\tBush-Oak-Var01\t4\t3.4\t4\t1",
             "TEXTURES\t42", "MATERIALS\t24", "STRAY\t0"]
    report = parse_verify(lines, MANIFEST)
    assert report["model_mismatch"] == [{"name": "Oak-Old-Var01", "got": 1, "expected": 2}]
    assert report["bounds_mismatch"] == []
    assert not check_report(report, n_prefabs=2, n_materials=24)


def test_parse_verify_and_check_report():
    lines = ["BOUNDS\tOak-Old-Var01\t14\t18.6\t14\t2",
             "BOUNDS\tBush-Oak-Var01\t4\t3.4\t4.3\t1",      # z is off by more than 1%
             "TEXTURES\t42", "MATERIALS\t24", "STRAY\t0"]
    report = parse_verify(lines, MANIFEST)
    assert report["prefabs"] == 2 and report["textures"] == 42 and report["materials"] == 24 and report["stray"] == 0
    assert [m["name"] for m in report["bounds_mismatch"]] == ["Bush-Oak-Var01"]
    assert report["model_mismatch"] == []
    assert not check_report(report, n_prefabs=2, n_materials=24)
    report["bounds_mismatch"] = []
    assert check_report(report, n_prefabs=2, n_materials=24)
    report["textures"] = 49
    assert not check_report(report, n_prefabs=2, n_materials=24)
