import pathlib
import sys
import textwrap

import pytest

# Import tools.vegetation from the repo root.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3]))

# Trunk1 and Leaves1 sit in the Oak row (z = -60) so the end-to-end split
# test finds a species; Leaves2 is a free-standing bush at z = 0.
TINY_OBJ = textwrap.dedent("""\
    # tiny
    mtllib tiny.mtl
    g Trunk1
    usemtl Trunk1Mtl
    v 0 0 -60
    v 1 0 -60
    v 0 2 -60
    vt 0 0
    vt 1 0
    vt 0 1
    vn 0 0 1
    f 1/1/1 2/2/1 3/3/1
    g Leaves1
    usemtl Leaves1Mtl
    v 0 2 -60
    v 1 2 -60
    v 0 3 -60
    v 1 3 -60
    vt 0 0
    vt 1 0
    f 4/4 5/5 6/4
    f -1 -2 -3
    g Leaves2
    usemtl Leaves1Mtl
    v 5 0 0
    v 6 0 0
    v 5 1 0
    vn 0 1 0
    f 8//2 9//2 10//2
    """)


@pytest.fixture
def tiny_obj(tmp_path):
    p = tmp_path / "tiny.obj"
    p.write_text(TINY_OBJ)
    return str(p)
