# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Generates the DNG fixtures the RAW import tests read.

Run it with ``just fixtures``; it rewrites every ``*.dng`` next to this script.
The generated files are committed, and they -- not this script -- are what the
tests load. See ADR 004 for why fixtures are committed binaries from a
committed generator, and README.md for the table of what each one is for.

Adapted from ``tests/fixtures/make_test_dng.py`` on the ``main`` branch, which
produced a single LinearRaw gradient. The variants here exist because that one
file cannot observe most of what can go wrong: it is already demosaiced, its
white balance is unity, and it carries no orientation, so a decoder that
skipped demosaicing, ignored the as-shot neutral, or rotated the frame would
all decode it identically and correctly.

Stdlib only: a DNG is a TIFF with extra tags, and a TIFF writer that emits one
uncompressed strip is about eighty lines.
"""

from __future__ import annotations

import pathlib
import struct

W, H = 32, 24

TYPE_BYTE, TYPE_ASCII, TYPE_SHORT, TYPE_LONG, TYPE_RATIONAL = 1, 2, 3, 4, 5
TYPE_SRATIONAL = 10
TYPE_SIZE = {TYPE_BYTE: 1, TYPE_ASCII: 1, TYPE_SHORT: 2, TYPE_LONG: 4,
             TYPE_RATIONAL: 8, TYPE_SRATIONAL: 8}
PACK = {TYPE_BYTE: "B", TYPE_SHORT: "H", TYPE_LONG: "I"}

PHOTOMETRIC_CFA = 32803
PHOTOMETRIC_LINEAR_RAW = 34892

# XYZ (D65) -> linear sRGB. Declaring camera space as sRGB keeps the fixtures
# readable: with a unity as-shot neutral, a neutral input stays neutral, so any
# colour cast in a decode is a bug rather than a colour-space artefact.
COLOR_MATRIX_1 = [3.2406, -1.5372, -0.4986,
                  -0.9689, 1.8758, 0.0415,
                  0.0557, -0.2040, 1.0570]

STRIP = ["STRIP"]  # placeholder patched with the strip's real offset


def pack_values(vtype, values):
    if vtype == TYPE_ASCII:
        return values  # already bytes, NUL-terminated
    if vtype in (TYPE_RATIONAL, TYPE_SRATIONAL):
        fmt = "<ii" if vtype == TYPE_SRATIONAL else "<II"
        return b"".join(struct.pack(fmt, num, den) for num, den in values)
    return b"".join(struct.pack("<" + PACK[vtype], v) for v in values)


def write_dng(path: pathlib.Path, entries: list, pixels: bytes) -> None:
    """Writes a single-IFD, uncompressed TIFF/DNG.

    ``entries`` is a list of ``(tag, type, values)`` and must be sorted by tag,
    which the TIFF specification requires and some readers rely on.
    """
    assert entries == sorted(entries, key=lambda e: e[0]), "IFD entries must be sorted by tag"

    ifd_offset = 8
    data_offset = ifd_offset + 2 + len(entries) * 12 + 4

    # First pass to know where the strip lands: everything that does not fit in
    # a four-byte value field goes to the overflow area, pixels go last.
    blobs = {}
    cursor = data_offset
    for tag, vtype, values in entries:
        if values == STRIP:
            continue
        size = len(values) * TYPE_SIZE[vtype]
        if size > 4:
            cursor += cursor % 2
            blobs[tag] = cursor
            cursor += size
    cursor += cursor % 2
    strip_offset = cursor

    cursor = data_offset
    ifd = struct.pack("<H", len(entries))
    overflow = bytearray()
    for tag, vtype, values in entries:
        if values == STRIP:
            values = [strip_offset]
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
        ifd += struct.pack("<HHI", tag, vtype, len(values)) + value_field
    ifd += struct.pack("<I", 0)  # no next IFD
    if cursor % 2:
        overflow += b"\0"

    path.write_bytes(struct.pack("<2sHI", b"II", 42, ifd_offset) + ifd + overflow + pixels)


def base_entries(*, pixel_bytes: int, samples_per_pixel: int, photometric: int,
                 as_shot_neutral: tuple[float, float, float],
                 orientation: int | None) -> list:
    """Builds the tags every fixture shares, in tag order."""
    entries = [
        (254, TYPE_LONG, [0]),                              # NewSubfileType: main image
        (256, TYPE_LONG, [W]),                              # ImageWidth
        (257, TYPE_LONG, [H]),                              # ImageLength
        (258, TYPE_SHORT, [16] * samples_per_pixel),        # BitsPerSample
        (259, TYPE_SHORT, [1]),                             # Compression: none
        (262, TYPE_SHORT, [photometric]),                   # PhotometricInterpretation
        (273, TYPE_LONG, STRIP),                            # StripOffsets (patched)
    ]
    if orientation is not None:
        entries.append((274, TYPE_SHORT, [orientation]))    # Orientation
    entries += [
        (277, TYPE_SHORT, [samples_per_pixel]),             # SamplesPerPixel
        (278, TYPE_LONG, [H]),                              # RowsPerStrip
        (279, TYPE_LONG, [pixel_bytes]),                    # StripByteCounts
        (284, TYPE_SHORT, [1]),                             # PlanarConfiguration
        (50706, TYPE_BYTE, [1, 4, 0, 0]),                   # DNGVersion
        (50708, TYPE_ASCII, b"arraw-test\0"),               # UniqueCameraModel
        (50717, TYPE_LONG, [65535]),                        # WhiteLevel
        (50721, TYPE_SRATIONAL,                             # ColorMatrix1
         [(round(v * 10000), 10000) for v in COLOR_MATRIX_1]),
        (50728, TYPE_RATIONAL,                              # AsShotNeutral
         [(round(v * 10000), 10000) for v in as_shot_neutral]),
        (50778, TYPE_SHORT, [21]),                          # CalibrationIlluminant1: D65
    ]
    return sorted(entries, key=lambda e: e[0])


def linear_gradient() -> bytes:
    """A horizontal black-to-white ramp, three 16-bit samples per pixel.

    The ramp is neutral and the step is exact, which is what makes the
    bit-exactness assertion possible: with a unity as-shot neutral, an sRGB
    camera matrix, linear output gamma and no auto-brightening, every sample
    must come back as the integer it went in as.
    """
    pixels = bytearray()
    for _y in range(H):
        for x in range(W):
            value = round(x / (W - 1) * 65535)
            pixels += struct.pack("<HHH", value, value, value)
    return bytes(pixels)


def bayer_flat() -> bytes:
    """A flat field whose RGGB sites hold three *different* constants.

    One 16-bit sample per pixel, in an RGGB mosaic. Undemosaiced, this is a
    checkerboard; correctly demosaiced, every interior pixel is the same
    non-neutral colour. That makes two things observable at once: uniformity
    proves interpolation happened at all, and the colour's ordering proves the
    CFA pattern was read rather than assumed.
    """
    red, green, blue = 48000, 32000, 16000
    pixels = bytearray()
    for y in range(H):
        for x in range(W):
            if y % 2 == 0:
                value = red if x % 2 == 0 else green
            else:
                value = green if x % 2 == 0 else blue
            pixels += struct.pack("<H", value)
    return bytes(pixels)


def main() -> None:
    here = pathlib.Path(__file__).parent
    linear = linear_gradient()

    # The reference fixture: everything neutral, so the decode settings that
    # matter most -- no_auto_bright and a linear output gamma -- are asserted
    # bit-exactly. Any stray brightening or gamma shows up immediately.
    write_dng(here / "linear-32x24-neutral.dng",
              base_entries(pixel_bytes=len(linear), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=(1.0, 1.0, 1.0), orientation=None),
              linear)

    # A non-unity as-shot neutral: multipliers of (2.0, 1.0, 1.25), so a neutral
    # ramp must come back warm. Proves use_camera_wb is honoured -- invisible on
    # the fixture above, where camera white balance and none are the same thing.
    write_dng(here / "linear-32x24-warmwb.dng",
              base_entries(pixel_bytes=len(linear), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=(0.5, 1.0, 0.8), orientation=None),
              linear)

    # Orientation 6 (rotate 90 CW). arraw keeps rotation a develop setting, so a
    # correct decode stays 32x24 and a decoder that applied the tag returns
    # 24x32. That also makes this the one fixture that can tell from the outside
    # *which decoder ran*: KDE's kimg_raw plugin honours the tag, ours must not.
    write_dng(here / "linear-32x24-rotated.dng",
              base_entries(pixel_bytes=len(linear), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=(1.0, 1.0, 1.0), orientation=6),
              linear)

    # A real mosaic, so the demosaic step is exercised rather than skipped.
    bayer = bayer_flat()
    entries = base_entries(pixel_bytes=len(bayer), samples_per_pixel=1,
                           photometric=PHOTOMETRIC_CFA,
                           as_shot_neutral=(1.0, 1.0, 1.0), orientation=None)
    entries += [
        (33421, TYPE_SHORT, [2, 2]),          # CFARepeatPatternDim
        (33422, TYPE_BYTE, [0, 1, 1, 2]),     # CFAPattern: RGGB
        (50710, TYPE_BYTE, [0, 1, 2]),        # CFAPlaneColor: RGB
        (50711, TYPE_SHORT, [1]),             # CFALayout: rectangular
        (50714, TYPE_SHORT, [0]),             # BlackLevel
    ]
    write_dng(here / "bayer-32x24.dng", sorted(entries, key=lambda e: e[0]), bayer)

    for path in sorted(here.glob("*.dng")):
        print(f"{path.name}: {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
