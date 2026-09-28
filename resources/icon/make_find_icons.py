#!/usr/bin/env python3
"""Draws the find and replace icons, the explorer's insert +, and the Window menu's check into ../icons.
Needs only the standard library.

Each icon is 32 pixels square and is shown at 16 points, so it stays sharp on a
2x display. Shapes are sampled 4x4 per pixel for smooth edges. The PNGs are
committed, so a build does not need Python.

The button icons are pure white. The studio colors them with the CSS
image-color: currentColor, so they take the text color in any theme. Search.png
is the Search pane's tab icon and keeps its own colors, like the other tabs'.
"""

import math
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "icons"
SIZE = 32
SAMPLES = 4
INK = (255, 255, 255)
STROKE = 2.2


def segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    x, y = ax + dx * t, ay + dy * t
    return math.hypot(px - x, py - y)


def polyline(points, width=STROKE):
    """A stroke through points with round caps and joins."""
    half = width / 2

    def inside(x, y):
        return any(segment_distance(x, y, *points[i], *points[i + 1]) <= half for i in range(len(points) - 1))

    return inside


def box(left, top, right, bottom):
    return lambda x, y: left <= x <= right and top <= y <= bottom


def frame(left, top, right, bottom, width=STROKE):
    outer = box(left, top, right, bottom)
    inner = box(left + width, top + width, right - width, bottom - width)
    return lambda x, y: outer(x, y) and not inner(x, y)


def disc(cx, cy, radius):
    return lambda x, y: math.hypot(x - cx, y - cy) <= radius


def ring(cx, cy, radius, width):
    return lambda x, y: abs(math.hypot(x - cx, y - cy) - radius) <= width / 2


def draw(layers):
    """layers: (shape, (r, g, b), alpha) drawn bottom first. Returns RGBA rows."""
    rows = []
    for py in range(SIZE):
        row = bytearray()
        for px in range(SIZE):
            r = g = b = a = 0.0
            for shape, color, alpha in layers:
                hits = 0
                for sy in range(SAMPLES):
                    for sx in range(SAMPLES):
                        if shape(px + (sx + 0.5) / SAMPLES, py + (sy + 0.5) / SAMPLES):
                            hits += 1
                cover = alpha * hits / (SAMPLES * SAMPLES)
                if cover <= 0:
                    continue
                # Source over, in straight alpha.
                out = cover + a * (1 - cover)
                r = (color[0] * cover + r * a * (1 - cover)) / out
                g = (color[1] * cover + g * a * (1 - cover)) / out
                b = (color[2] * cover + b * a * (1 - cover)) / out
                a = out
            row += bytes((round(r), round(g), round(b), round(a * 255)))
        rows.append(bytes(row))
    return rows


def write_png(path, rows):
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
                     chunk(b"IEND", b""))


def ink(*shapes):
    return [(shape, INK, 1.0) for shape in shapes]


ICONS = {
    "FindPrevious.png": ink(polyline([(16, 26), (16, 7)]), polyline([(8.5, 14.5), (16, 7), (23.5, 14.5)])),
    "FindNext.png": ink(polyline([(16, 6), (16, 25)]), polyline([(8.5, 17.5), (16, 25), (23.5, 17.5)])),
    "FindClose.png": ink(polyline([(9, 9), (23, 23)]), polyline([(23, 9), (9, 23)])),
    "FindCollapsed.png": ink(polyline([(12, 8), (20, 16), (12, 24)])),
    "FindExpanded.png": ink(polyline([(8, 12), (16, 20), (24, 12)])),
    # The explorer's insert chip on a hovered row.
    "Plus.png": ink(polyline([(16, 8), (16, 24)]), polyline([(8, 16), (24, 16)])),
    # The Window menu's mark beside a window that is open.
    "Check.png": ink(polyline([(7, 16.5), (13, 22.5), (25, 9.5)], 3)),
    # The old text in outline, an arrow, and the new text filled in.
    "Replace.png": ink(
        frame(3, 4, 16, 13),
        polyline([(9.5, 15), (9.5, 23.5), (15, 23.5)]),
        polyline([(12, 20), (15.5, 23.5), (12, 27)]),
        box(18, 19, 29, 28),
    ),
    "ReplaceAll.png": ink(
        frame(3, 3, 16, 11),
        polyline([(9.5, 13), (9.5, 21.5), (15, 21.5)]),
        polyline([(12, 18), (15.5, 21.5), (12, 25)]),
        box(18, 15, 29, 20),
        box(18, 23, 29, 28),
    ),
    # The Search pane's tab: a glass lens and a handle.
    "Search.png": [
        (disc(13, 13, 8), (206, 226, 250), 1.0),
        (ring(13, 13, 8.5, 3), (59, 109, 179), 1.0),
        (polyline([(19.8, 19.8), (27, 27)], 5), (107, 79, 42), 1.0),
    ],
}


def main() -> None:
    for name, layers in ICONS.items():
        write_png(OUT / name, draw(layers))


if __name__ == "__main__":
    main()
