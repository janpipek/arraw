#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["cairosvg"]
# ///
"""Regenerate the runtime window-icon PNGs from resources/icon.svg (ADR 0013).

resources/icon.svg is the single source of truth; these PNGs are derived
artifacts baked into resources/icon.qrc and fed to QApplication::setWindowIcon.
Do not hand-edit the PNGs — edit the SVG and rerun:

    uv run tools/render_icons.py
"""
from pathlib import Path

import cairosvg

SIZES = (16, 24, 32, 48, 64, 128, 256)

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "resources" / "icon.svg"
OUT_DIR = ROOT / "resources" / "icons"


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    svg_bytes = SVG.read_bytes()
    for size in SIZES:
        out = OUT_DIR / f"arraw-{size}.png"
        cairosvg.svg2png(
            bytestring=svg_bytes,
            write_to=str(out),
            output_width=size,
            output_height=size,
        )
        print(f"wrote {out.relative_to(ROOT)} ({size}x{size})")


if __name__ == "__main__":
    main()
