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
PREVIEW_W, PREVIEW_H = 8, 6

TYPE_BYTE, TYPE_ASCII, TYPE_SHORT, TYPE_LONG, TYPE_RATIONAL = 1, 2, 3, 4, 5
TYPE_SRATIONAL = 10
TYPE_SIZE = {TYPE_BYTE: 1, TYPE_ASCII: 1, TYPE_SHORT: 2, TYPE_LONG: 4,
             TYPE_RATIONAL: 8, TYPE_SRATIONAL: 8}
PACK = {TYPE_BYTE: "B", TYPE_SHORT: "H", TYPE_LONG: "I"}

PHOTOMETRIC_RGB = 2
PHOTOMETRIC_CFA = 32803
PHOTOMETRIC_LINEAR_RAW = 34892

# XYZ (D65) -> linear sRGB. Declaring camera space as sRGB keeps the fixtures
# readable: with a unity as-shot neutral, a neutral input stays neutral, so any
# colour cast in a decode is a bug rather than a colour-space artefact.
COLOR_MATRIX_1 = [3.2406, -1.5372, -0.4986,
                  -0.9689, 1.8758, 0.0415,
                  0.0557, -0.2040, 1.0570]

# A camera that is *not* sRGB, for the colour maths the matrix above cannot
# see. Two things are wrong with it deliberately. Its rows are scaled (red
# doubled, blue at four fifths), which LibRaw divides out of the matrix and
# keeps in pre_mul -- so a decoder that ignores pre_mul loses how unevenly this
# sensor responds, and cannot turn a temperature into channel gains. And its
# red row is mixed with green, so the matrix itself is not the identity and a
# decode that skipped it would show. See ADR 007.
SKEWED_COLOR_MATRIX_1 = [2 * (3.2406 + 0.1 * -0.9689), 2 * (-1.5372 + 0.1 * 1.8758),
                         2 * (-0.4986 + 0.1 * 0.0415),
                         -0.9689, 1.8758, 0.0415,
                         0.8 * 0.0557, 0.8 * -0.2040, 0.8 * 1.0570]

STRIP = ["STRIP"]  # placeholder patched with the strip's real offset
SUBIFDS = ["SUBIFDS"]  # placeholder patched with the sub-IFD offsets


def pack_values(vtype, values):
    if vtype == TYPE_ASCII:
        return values  # already bytes, NUL-terminated
    if vtype in (TYPE_RATIONAL, TYPE_SRATIONAL):
        fmt = "<ii" if vtype == TYPE_SRATIONAL else "<II"
        return b"".join(struct.pack(fmt, num, den) for num, den in values)
    return b"".join(struct.pack("<" + PACK[vtype], v) for v in values)


def value_count(values, subifd_count: int) -> int:
    """Number of values an entry holds, resolving the placeholders."""
    if values == STRIP:
        return 1
    if values == SUBIFDS:
        return subifd_count
    return len(values)


def write_tiff(path: pathlib.Path, ifds: list) -> None:
    """Writes an uncompressed little-endian TIFF/DNG.

    ``ifds`` is a list of ``(entries, pixels)`` pairs. The first is IFD0; any
    others are written as its sub-IFDs, and IFD0 must then carry a ``SubIFDs``
    entry whose values are the ``SUBIFDS`` placeholder. Each IFD's ``entries``
    is a list of ``(tag, type, values)`` sorted by tag, which the TIFF
    specification requires and some readers rely on, and its ``StripOffsets``
    entry uses the ``STRIP`` placeholder.

    The layout is every IFD (each followed by its own overflow area), then
    every strip, in IFD order.
    """
    subifd_count = len(ifds) - 1
    for entries, _pixels in ifds:
        assert entries == sorted(entries, key=lambda e: e[0]), "IFD entries must be sorted by tag"

    # First pass: where everything lands. Sizes are known without the offsets,
    # because a placeholder's value count is known even when its value is not.
    cursor = 8
    ifd_offsets = []
    blob_offsets = []
    for entries, _pixels in ifds:
        ifd_offsets.append(cursor)
        cursor += 2 + len(entries) * 12 + 4
        blobs = {}
        for tag, vtype, values in entries:
            size = value_count(values, subifd_count) * TYPE_SIZE[vtype]
            if size > 4:
                cursor += cursor % 2
                blobs[tag] = cursor
                cursor += size
        blob_offsets.append(blobs)
    strip_offsets = []
    for _entries, pixels in ifds:
        cursor += cursor % 2
        strip_offsets.append(cursor)
        cursor += len(pixels)

    # Second pass: the bytes themselves, placeholders resolved.
    out = bytearray(struct.pack("<2sHI", b"II", 42, ifd_offsets[0]))
    for index, (entries, _pixels) in enumerate(ifds):
        assert len(out) == ifd_offsets[index]
        cursor = ifd_offsets[index] + 2 + len(entries) * 12 + 4
        ifd = struct.pack("<H", len(entries))
        overflow = bytearray()
        for tag, vtype, values in entries:
            if values == STRIP:
                values = [strip_offsets[index]]
            elif values == SUBIFDS:
                values = ifd_offsets[1:]
            packed = pack_values(vtype, values)
            if len(packed) <= 4:
                value_field = packed.ljust(4, b"\0")
            else:
                if cursor % 2:
                    overflow += b"\0"
                    cursor += 1
                assert blob_offsets[index][tag] == cursor
                value_field = struct.pack("<I", cursor)
                overflow += packed
                cursor += len(packed)
            ifd += struct.pack("<HHI", tag, vtype, value_count(values, subifd_count)) + value_field
        ifd += struct.pack("<I", 0)  # IFDs are chained through SubIFDs, not here
        out += ifd + overflow
    for index, (_entries, pixels) in enumerate(ifds):
        if len(out) % 2:
            out += b"\0"
        assert len(out) == strip_offsets[index]
        out += pixels

    path.write_bytes(bytes(out))


def write_dng(path: pathlib.Path, entries: list, pixels: bytes) -> None:
    """Writes a single-IFD DNG -- the shape all but one fixture has."""
    write_tiff(path, [(entries, pixels)])


def image_entries(*, width: int, height: int, bits: int, samples_per_pixel: int,
                  photometric: int, pixel_bytes: int, reduced: bool = False) -> list:
    """Builds the structure tags that describe one uncompressed strip."""
    return [
        (254, TYPE_LONG, [1 if reduced else 0]),            # NewSubfileType
        (256, TYPE_LONG, [width]),                          # ImageWidth
        (257, TYPE_LONG, [height]),                         # ImageLength
        (258, TYPE_SHORT, [bits] * samples_per_pixel),      # BitsPerSample
        (259, TYPE_SHORT, [1]),                             # Compression: none
        (262, TYPE_SHORT, [photometric]),                   # PhotometricInterpretation
        (273, TYPE_LONG, STRIP),                            # StripOffsets (patched)
        (277, TYPE_SHORT, [samples_per_pixel]),             # SamplesPerPixel
        (278, TYPE_LONG, [height]),                         # RowsPerStrip
        (279, TYPE_LONG, [pixel_bytes]),                    # StripByteCounts
        (284, TYPE_SHORT, [1]),                             # PlanarConfiguration
    ]


def camera_entries(as_shot_neutral: tuple[float, float, float] | None,
                   colour_matrix: list[float] | None = None) -> list:
    """Builds the DNG tags that describe the camera rather than the pixels.

    ``as_shot_neutral`` of ``None`` omits the tag, leaving a file that declares
    no camera white balance at all.
    """
    entries = [
        (50706, TYPE_BYTE, [1, 4, 0, 0]),                   # DNGVersion
        (50708, TYPE_ASCII, b"arraw-test\0"),               # UniqueCameraModel
        (50721, TYPE_SRATIONAL,                             # ColorMatrix1
         [(round(v * 10000), 10000) for v in (colour_matrix or COLOR_MATRIX_1)]),
        (50778, TYPE_SHORT, [21]),                          # CalibrationIlluminant1: D65
    ]
    if as_shot_neutral is not None:
        entries.append((50728, TYPE_RATIONAL,               # AsShotNeutral
                        [(round(v * 10000), 10000) for v in as_shot_neutral]))
    return entries


def base_entries(*, pixel_bytes: int, samples_per_pixel: int, photometric: int,
                 as_shot_neutral: tuple[float, float, float] | None,
                 orientation: int | None, colour_matrix: list[float] | None = None) -> list:
    """Builds the tags a single-IFD fixture carries, in tag order."""
    entries = image_entries(width=W, height=H, bits=16,
                            samples_per_pixel=samples_per_pixel,
                            photometric=photometric, pixel_bytes=pixel_bytes)
    if orientation is not None:
        entries.append((274, TYPE_SHORT, [orientation]))    # Orientation
    entries.append((50717, TYPE_LONG, [65535]))             # WhiteLevel
    entries += camera_entries(as_shot_neutral, colour_matrix)
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


def linear_halves(left: int, right: int) -> bytes:
    """A neutral field of two flat halves, three 16-bit samples per pixel.

    Both values are constants rather than a ramp, so what a decode did to them
    is readable as a single number instead of a curve.
    """
    pixels = bytearray()
    for _y in range(H):
        for x in range(W):
            value = left if x < W // 2 else right
            pixels += struct.pack("<HHH", value, value, value)
    return bytes(pixels)


def linear_flat(red: int, green: int, blue: int, *, right_half: tuple[int, int, int] | None = None
                ) -> bytes:
    """A strongly non-neutral field, three 16-bit samples per pixel.

    ``right_half`` replaces the right half of the frame, which changes the
    channel sums an automatic white balance would be computed from while
    leaving the left half's colour untouched.
    """
    left = struct.pack("<HHH", red, green, blue)
    right = left if right_half is None else struct.pack("<HHH", *right_half)
    row = left * (W // 2) + right * (W - W // 2)
    return row * H


def preview_rgb(width: int, height: int) -> bytes:
    """An ordinary 8-bit RGB image, the kind any TIFF reader can decode.

    Flat magenta: a colour the RAW beside it does not contain anywhere, so a
    test that gets this image instead of the photograph can say so.
    """
    return bytes([255, 0, 255]) * (width * height)


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

    # A camera that is not sRGB: scaled matrix rows, so the daylight calibration
    # LibRaw keeps in pre_mul is not unity, and a red row mixed with green, so
    # the matrix itself is not the identity. Every other fixture here would pass
    # with the colour maths deleted; this one would not (ADR 007).
    write_dng(here / "linear-32x24-skewed.dng",
              base_entries(pixel_bytes=len(linear), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=(0.5, 1.0, 0.8), orientation=None,
                           colour_matrix=SKEWED_COLOR_MATRIX_1),
              linear)

    # The skewed camera again, this time declaring no white balance at all. The
    # decode substitutes the daylight multipliers its matrix implies, so the
    # gains the file records and the gains the pixels went through are different
    # numbers -- and with a calibration this far from unity, resolving a setting
    # from the wrong one is visible. The other missing-neutral fixtures cannot
    # show it: their calibration is near unity, so the two agree.
    write_dng(here / "linear-32x24-skewed-nowb.dng",
              base_entries(pixel_bytes=len(linear), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=None, orientation=None,
                           colour_matrix=SKEWED_COLOR_MATRIX_1),
              linear)

    # A frame whose brightest value is below WhiteLevel but above LibRaw's
    # adjust_maximum_thr of 0.75. Left to itself LibRaw lowers the white level
    # to 52000 and stretches everything by 65535/52000, which is the per-frame
    # brightness dependence no_auto_bright alone does not remove. The ramp
    # fixtures cannot see it: they reach 65535, so there is nothing to lower.
    halves = linear_halves(16000, 52000)
    write_dng(here / "linear-32x24-highmax.dng",
              base_entries(pixel_bytes=len(halves), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=(1.0, 1.0, 1.0), orientation=None),
              halves)

    # No AsShotNeutral at all, over a strongly coloured field. LibRaw answers a
    # missing camera white balance with an automatic one computed from the
    # frame, which would pull this colour towards grey; arraw's daylight
    # fallback leaves it alone. The colour is what makes the two visible apart.
    flat = linear_flat(48000, 32000, 16000)
    write_dng(here / "linear-32x24-nowb.dng",
              base_entries(pixel_bytes=len(flat), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=None, orientation=None),
              flat)

    # The same colour again, over a frame that is half dark. Daylight
    # multipliers come from the colour matrix and cannot tell the two frames
    # apart; an automatic white balance is computed from the channel sums and
    # cannot help but. So the left half is the assertion: same pixels in, same
    # pixels out, or the white balance moved with the content.
    flat_dark = linear_flat(48000, 32000, 16000, right_half=(2000, 2000, 2000))
    write_dng(here / "linear-32x24-nowb-dark.dng",
              base_entries(pixel_bytes=len(flat_dark), samples_per_pixel=3,
                           photometric=PHOTOMETRIC_LINEAR_RAW,
                           as_shot_neutral=None, orientation=None),
              flat_dark)

    # The shape of a real RAW: an ordinary RGB preview in IFD0 and the sensor
    # data in a sub-IFD. Every TIFF reader decodes the preview happily, so this
    # is the fixture that can tell whether loadImage returned the photograph or
    # a thumbnail of it. The sub-IFD holds the same ramp as the neutral
    # fixture, so a correct decode is comparable against it pixel for pixel.
    preview = preview_rgb(PREVIEW_W, PREVIEW_H)
    preview_ifd = image_entries(width=PREVIEW_W, height=PREVIEW_H, bits=8,
                                samples_per_pixel=3, photometric=PHOTOMETRIC_RGB,
                                pixel_bytes=len(preview), reduced=True)
    preview_ifd.append((330, TYPE_LONG, SUBIFDS))           # SubIFDs (patched)
    preview_ifd += camera_entries((1.0, 1.0, 1.0))
    raw_ifd = image_entries(width=W, height=H, bits=16, samples_per_pixel=3,
                            photometric=PHOTOMETRIC_LINEAR_RAW,
                            pixel_bytes=len(linear))
    raw_ifd.append((50717, TYPE_LONG, [65535]))             # WhiteLevel
    write_tiff(here / "preview-32x24.dng",
               [(sorted(preview_ifd, key=lambda e: e[0]), preview),
                (sorted(raw_ifd, key=lambda e: e[0]), linear)])

    for path in sorted(here.glob("*.dng")):
        print(f"{path.name}: {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
