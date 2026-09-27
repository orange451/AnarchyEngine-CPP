#!/usr/bin/env python3
"""Rebuilds the program icons from anarchy.png. Needs Pillow (pip install pillow).

anarchy.ico is the Windows icon, embedded in the .exe by anarchy.rc.
anarchy.icns is the macOS icon, copied into the app bundle.
Both are committed, so a build does not need Python.
"""

from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent


def main() -> None:
    source = Image.open(HERE / "anarchy.png").convert("RGBA")
    # Windows picks the closest size for each place it draws the icon.
    source.save(HERE / "anarchy.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    # Pillow writes every size macOS asks for, 16 through 1024, from one square image.
    source.save(HERE / "anarchy.icns")


if __name__ == "__main__":
    main()
