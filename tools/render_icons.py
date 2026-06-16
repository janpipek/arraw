#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["cairosvg"]
# ///
"""Regenerate the runtime window-icon PNGs from resources/icon.svg (ADR 0013).

resources/icon.svg is the single source of truth; these PNGs are derived
artifacts baked into resources/icon.qrc and fed to QApplication::setWindowIcon.
The same PNGs are packed into resources/arraw.ico, which is embedded into the
Windows executable as its Win32 icon (Explorer / taskbar / title bar).
Do not hand-edit the PNGs or .ico — edit the SVG and rerun:

    uv run tools/render_icons.py
"""
import struct
from pathlib import Path

import cairosvg

SIZES = (16, 24, 32, 48, 64, 128, 256)

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "resources" / "icon.svg"
OUT_DIR = ROOT / "resources" / "icons"
ICO = ROOT / "resources" / "arraw.ico"


def build_ico(png_paths: list[Path], out: Path) -> None:
    """Pack PNGs into a single .ico (PNG-compressed entries, Vista+)."""
    blobs = [(p, p.read_bytes()) for p in png_paths]
    offset = 6 + len(blobs) * 16  # ICONDIR + ICONDIRENTRY[]
    entries = bytearray()
    for p, data in blobs:
        size = int(p.stem.rsplit("-", 1)[1])
        dim = 0 if size >= 256 else size  # 0 means 256 in the ICO header
        # bWidth, bHeight, bColorCount, bReserved, wPlanes, wBitCount, dwBytes, dwOffset
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out.write_bytes(
        struct.pack("<HHH", 0, 1, len(blobs)) + bytes(entries) + b"".join(d for _, d in blobs)
    )
    print(f"wrote {out.relative_to(ROOT)} ({len(blobs)} sizes)")


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    svg_bytes = SVG.read_bytes()
    pngs = []
    for size in SIZES:
        out = OUT_DIR / f"arraw-{size}.png"
        cairosvg.svg2png(
            bytestring=svg_bytes,
            write_to=str(out),
            output_width=size,
            output_height=size,
        )
        pngs.append(out)
        print(f"wrote {out.relative_to(ROOT)} ({size}x{size})")
    build_ico(pngs, ICO)


if __name__ == "__main__":
    main()
