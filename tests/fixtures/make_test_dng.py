#!/usr/bin/env python3
"""Generate gradient-32x24.dng — the tiny RawProcessor test fixture.

A minimal linear-RGB DNG (PhotometricInterpretation = LinearRaw, 16-bit,
uncompressed): a horizontal black-to-white gradient, 32x24. Camera space is
declared as linear sRGB via ColorMatrix1 (XYZ D65 -> sRGB), AsShotNeutral is
unity so use_camera_wb is a no-op. No embedded thumbnail, on purpose - the
tests also assert that loadEmbeddedPreview fails cleanly.

Stdlib only. Rerun to regenerate:  python3 make_test_dng.py
"""
import struct
from pathlib import Path

W, H = 32, 24

TYPE_BYTE, TYPE_ASCII, TYPE_SHORT, TYPE_LONG, TYPE_RATIONAL = 1, 2, 3, 4, 5
TYPE_SRATIONAL = 10
TYPE_SIZE = {TYPE_BYTE: 1, TYPE_ASCII: 1, TYPE_SHORT: 2, TYPE_LONG: 4,
             TYPE_RATIONAL: 8, TYPE_SRATIONAL: 8}
PACK = {TYPE_BYTE: "B", TYPE_SHORT: "H", TYPE_LONG: "I"}

# XYZ (D65) -> linear sRGB, as SRATIONALs over 10000
COLOR_MATRIX_1 = [3.2406, -1.5372, -0.4986,
                  -0.9689, 1.8758, 0.0415,
                  0.0557, -0.2040, 1.0570]


def pack_values(vtype, values):
    if vtype == TYPE_ASCII:
        return values  # already bytes, NUL-terminated
    if vtype in (TYPE_RATIONAL, TYPE_SRATIONAL):
        fmt = "<ii" if vtype == TYPE_SRATIONAL else "<II"
        return b"".join(struct.pack(fmt, num, den) for num, den in values)
    return b"".join(struct.pack("<" + PACK[vtype], v) for v in values)


def main():
    pixels = bytearray()
    for _y in range(H):
        for x in range(W):
            v = round(x / (W - 1) * 65535)
            pixels += struct.pack("<HHH", v, v, v)

    model = b"arraw-test\0"
    entries = [  # (tag, type, values) — must stay sorted by tag
        (254, TYPE_LONG, [0]),                       # NewSubfileType: main image
        (256, TYPE_LONG, [W]),                       # ImageWidth
        (257, TYPE_LONG, [H]),                       # ImageLength
        (258, TYPE_SHORT, [16, 16, 16]),             # BitsPerSample
        (259, TYPE_SHORT, [1]),                      # Compression: none
        (262, TYPE_SHORT, [34892]),                  # Photometric: LinearRaw
        (273, TYPE_LONG, ["STRIP"]),                 # StripOffsets (patched)
        (277, TYPE_SHORT, [3]),                      # SamplesPerPixel
        (278, TYPE_LONG, [H]),                       # RowsPerStrip
        (279, TYPE_LONG, [len(pixels)]),             # StripByteCounts
        (284, TYPE_SHORT, [1]),                      # PlanarConfiguration
        (50706, TYPE_BYTE, [1, 4, 0, 0]),            # DNGVersion
        (50708, TYPE_ASCII, model),                  # UniqueCameraModel
        (50717, TYPE_LONG, [65535]),                 # WhiteLevel
        (50721, TYPE_SRATIONAL,                      # ColorMatrix1
         [(round(v * 10000), 10000) for v in COLOR_MATRIX_1]),
        (50728, TYPE_RATIONAL, [(1, 1)] * 3),        # AsShotNeutral
        (50778, TYPE_SHORT, [21]),                   # CalibrationIlluminant1: D65
    ]

    ifd_offset = 8
    data_offset = ifd_offset + 2 + len(entries) * 12 + 4
    overflow = bytearray()
    ifd = struct.pack("<H", len(entries))

    # First pass to know where the strip lands: everything that doesn't fit
    # inline goes to the overflow area, pixels go last.
    blobs = {}
    cursor = data_offset
    for tag, vtype, values in entries:
        count = len(values)
        if values and values == ["STRIP"]:
            continue
        size = count * TYPE_SIZE[vtype]
        if size > 4:
            if cursor % 2:
                cursor += 1
            blobs[tag] = cursor
            cursor += size
    if cursor % 2:
        cursor += 1
    strip_offset = cursor

    cursor = data_offset
    for tag, vtype, values in entries:
        if values == ["STRIP"]:
            values = [strip_offset]
        count = len(values)
        packed = pack_values(vtype, values)
        if len(packed) <= 4:
            value_field = packed.ljust(4, b"\0")
        else:
            if cursor % 2:
                overflow += b"\0"
                cursor += 1
            assert blobs[tag] == cursor
            value_field = struct.pack("<I", cursor)
            overflow += packed
            cursor += len(packed)
        ifd += struct.pack("<HHI", tag, vtype, count) + value_field
    ifd += struct.pack("<I", 0)  # no next IFD
    if cursor % 2:
        overflow += b"\0"

    out = struct.pack("<2sHI", b"II", 42, ifd_offset) + ifd + overflow + pixels
    path = Path(__file__).parent / "gradient-32x24.dng"
    path.write_bytes(out)
    print(f"wrote {path} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
