"""Streaming reader for a big OBJ: yields one Group per `g` line with its own
re-based vertex/uv/normal lists, so each group can be written standalone."""
from dataclasses import dataclass, field
from typing import Iterator


@dataclass
class Group:
    name: str
    material: str | None = None
    v: list = field(default_factory=list)
    vt: list = field(default_factory=list)
    vn: list = field(default_factory=list)
    # 0-based (v, vt, vn) indices into THIS group's lists; vt/vn may be None.
    faces: list = field(default_factory=list)


def _parse_index(token: str, total: int, start: int) -> int | None:
    """OBJ index (1-based, or negative from the end) -> local 0-based, or None."""
    if token == "":
        return None
    i = int(token)
    global_index = total + i if i < 0 else i - 1
    return global_index - start


def scan_groups(path: str) -> Iterator[Group]:
    nv = nvt = nvn = 0  # global counts so far
    cur: Group | None = None
    start = (0, 0, 0)  # global index where cur's lists begin

    def current() -> Group:
        nonlocal cur, start
        if cur is None:
            cur = Group(name="default")
            start = (nv, nvt, nvn)
        return cur

    with open(path, "r", errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                current().v.append((float(p[1]), float(p[2]), float(p[3])))
                nv += 1
            elif line.startswith("vt "):
                p = line.split()
                current().vt.append((float(p[1]), float(p[2])))
                nvt += 1
            elif line.startswith("vn "):
                p = line.split()
                current().vn.append((float(p[1]), float(p[2]), float(p[3])))
                nvn += 1
            elif line.startswith("f "):
                face = []
                for tok in line.split()[1:]:
                    parts = (tok.split("/") + ["", ""])[:3]
                    face.append((_parse_index(parts[0], nv, start[0]),
                                 _parse_index(parts[1], nvt, start[1]),
                                 _parse_index(parts[2], nvn, start[2])))
                current().faces.append(face)
            elif line.startswith("usemtl"):
                current().material = line.split(maxsplit=1)[1].strip()
            elif line.startswith("g ") or line.startswith("o "):
                if cur is not None:
                    yield cur
                cur = Group(name=line[2:].strip())
                start = (nv, nvt, nvn)
    if cur is not None:
        yield cur


def bounds(g: Group):
    xs = [p[0] for p in g.v]
    ys = [p[1] for p in g.v]
    zs = [p[2] for p in g.v]
    return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))
