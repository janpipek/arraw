# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Generates the PNG fixtures the integration tests read.

Run it with ``just fixtures``; it rewrites every file next to this script.
The generated PNGs are committed, and they -- not this script -- are what the
tests load. If you change anything here, regenerate and commit the result.

Why a generator at all, rather than saving images from Qt in the tests: the
round-trip test's job is to prove that ``loadImage`` followed by
``exportImage`` preserves an image. If Qt also wrote the input, a Qt codec bug
would cancel itself out and the test would pass through it. Everything below
is written with ``zlib`` and ``struct`` only, so no image library -- least of
all the one under test -- has a hand in producing the ground truth.

Why hand-rolled PNG writing rather than Pillow: Pillow has no 16-bit RGB mode
and no direct control over the ``sRGB``/``iCCP`` chunks, both of which the
import fixtures need. A PNG with filter type 0 on every scanline is about
thirty lines, so the dependency buys nothing.

See README.md for the table of what each fixture is for.
"""

from __future__ import annotations

import pathlib
import struct
import zlib

WIDTH = 61
HEIGHT = 41

# --------------------------------------------------------------------------
# PNG writing
# --------------------------------------------------------------------------

GREY = 0
RGB = 2
RGBA = 6

CHANNELS = {GREY: 1, RGB: 3, RGBA: 4}


def _chunk(tag: bytes, data: bytes) -> bytes:
    """Frames one PNG chunk: length, type, payload, CRC."""
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def write_png(
    path: pathlib.Path,
    samples: list[int],
    *,
    colour_type: int,
    bit_depth: int = 8,
    extra_chunks: tuple[bytes, ...] = (),
) -> None:
    """Writes a non-interlaced PNG.

    Every scanline uses filter type 0 (None). Filtering only affects how well
    the result compresses, and these images are a couple of kilobytes either
    way; unfiltered scanlines keep the encoder short enough to audit by eye.

    ``samples`` is row-major and interleaved, one integer per channel value.
    """
    row_length = WIDTH * CHANNELS[colour_type]
    assert len(samples) == row_length * HEIGHT, "sample count does not match the image"

    pack = struct.Struct(">H" if bit_depth == 16 else ">B").pack
    raw = bytearray()
    for y in range(HEIGHT):
        raw.append(0)
        for value in samples[y * row_length : (y + 1) * row_length]:
            raw += pack(value)

    header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, bit_depth, colour_type, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", header)
        + b"".join(extra_chunks)
        + _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + _chunk(b"IEND", b"")
    )


def srgb_chunk() -> bytes:
    """``sRGB`` chunk with the perceptual rendering intent."""
    return _chunk(b"sRGB", b"\x00")


def iccp_chunk(name: str, profile: bytes) -> bytes:
    """``iCCP`` chunk carrying a deflated ICC profile."""
    return _chunk(
        b"iCCP", name.encode("latin-1") + b"\x00\x00" + zlib.compress(profile, 9)
    )


# --------------------------------------------------------------------------
# The test card
# --------------------------------------------------------------------------


def _hue(position: float, value: int) -> tuple[int, int, int]:
    """Converts a position on the hue circle to a fully saturated colour.

    ``position`` runs over [0, 1]; ``value`` is the peak channel value.
    """
    sector = position * 6.0
    fraction = sector - int(sector)
    rising = round(value * fraction)
    falling = round(value * (1.0 - fraction))

    match int(sector) % 6:
        case 0:
            return (value, rising, 0)
        case 1:
            return (falling, value, 0)
        case 2:
            return (0, value, rising)
        case 3:
            return (0, falling, value)
        case 4:
            return (rising, 0, value)
        case _:
            return (value, 0, falling)


# Flat patches along the bottom band, left to right. The first four and the
# last few greys are the point of the whole fixture: deep shadow is where a
# linear working space would crush values, and near-white is where a transfer
# function that overshoots would clip. The test asserts these by name.
PATCHES: list[tuple[str, tuple[int, int, int]]] = [
    ("black", (0, 0, 0)),
    ("code1", (1, 1, 1)),
    ("code2", (2, 2, 2)),
    ("code3", (3, 3, 3)),
    ("quarterGrey", (64, 64, 64)),
    ("midGrey", (128, 128, 128)),
    ("threeQuarterGrey", (192, 192, 192)),
    ("nearWhite", (252, 252, 252)),
    ("white", (255, 255, 255)),
    ("red", (255, 0, 0)),
    ("green", (0, 255, 0)),
    ("blue", (0, 0, 255)),
]

# 61 = 12 * 5 + 1, so the rightmost column spills into the final patch. That is
# deliberate: the odd width is what stops a row-stride bug from hiding behind
# four-byte alignment, and the test's patch bounds are computed the same way.
PATCH_WIDTH = WIDTH // len(PATCHES)

# Band boundaries, top to bottom. The bands have different heights and
# different content so a vertical flip is obvious at a glance; every band runs
# dark-to-bright left to right so a horizontal flip is too.
BAND_HUE = (0, 8)
BAND_GREY = (8, 14)
BAND_RED = (14, 20)
BAND_GREEN = (20, 26)
BAND_BLUE = (26, 32)
BAND_PATCHES = (32, 41)


def render_card() -> list[tuple[int, int, int]]:
    """Builds the test card as one RGB tuple per pixel, row-major.

    Six bands: a saturated hue sweep (which exercises the Rec.2020 matrix
    round trip), independent grey, red, green and blue ramps (which walk the
    transfer function across most of its range), and a row of named flat
    patches.
    """
    pixels: list[tuple[int, int, int]] = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            ramp = round(255 * x / (WIDTH - 1))
            if BAND_HUE[0] <= y < BAND_HUE[1]:
                pixels.append(_hue(x / (WIDTH - 1), 255))
            elif BAND_GREY[0] <= y < BAND_GREY[1]:
                pixels.append((ramp, ramp, ramp))
            elif BAND_RED[0] <= y < BAND_RED[1]:
                pixels.append((ramp, 0, 0))
            elif BAND_GREEN[0] <= y < BAND_GREEN[1]:
                pixels.append((0, ramp, 0))
            elif BAND_BLUE[0] <= y < BAND_BLUE[1]:
                pixels.append((0, 0, ramp))
            else:
                index = min(x // PATCH_WIDTH, len(PATCHES) - 1)
                pixels.append(PATCHES[index][1])
    return pixels


# --------------------------------------------------------------------------
# A minimal Adobe RGB (1998) ICC profile
# --------------------------------------------------------------------------

# D50-adapted primaries and white point, as published for Adobe RGB (1998).
# The PCS of an ICC profile is always D50, so these are the chromatic-adaptation
# transformed values rather than the D65 ones quoted in the colour-space spec.
_ADOBE_RGB_PRIMARIES = {
    "rXYZ": (0.60974, 0.31111, 0.01947),
    "gXYZ": (0.20528, 0.62567, 0.06087),
    "bXYZ": (0.14919, 0.06322, 0.74457),
}
_D50 = (0.96420, 1.00000, 0.82491)
# Adobe RGB's transfer function is a pure 563/256 power law.
_ADOBE_RGB_GAMMA = 563


def _s15f16(value: float) -> bytes:
    return struct.pack(">i", round(value * 65536))


def _xyz_tag(xyz: tuple[float, float, float]) -> bytes:
    return b"XYZ \x00\x00\x00\x00" + b"".join(_s15f16(component) for component in xyz)


def _curve_tag(gamma: int) -> bytes:
    """``curv`` tag holding a single u8Fixed8 gamma exponent."""
    return b"curv\x00\x00\x00\x00" + struct.pack(">IH", 1, gamma)


def _text_description_tag(text: str) -> bytes:
    """ICC v2 ``desc`` tag; the Unicode and ScriptCode halves stay empty."""
    ascii_text = text.encode("ascii") + b"\x00"
    return (
        b"desc\x00\x00\x00\x00"
        + struct.pack(">I", len(ascii_text))
        + ascii_text
        + struct.pack(">II", 0, 0)
        + struct.pack(">HB", 0, 0)
        + bytes(67)
    )


def _text_tag(text: str) -> bytes:
    return b"text\x00\x00\x00\x00" + text.encode("ascii") + b"\x00"


def adobe_rgb_profile() -> bytes:
    """Builds a matrix/TRC display profile describing Adobe RGB (1998).

    Hand-built because no dependency-free way to obtain one exists: Pillow's
    ``ImageCms`` can only create sRGB, LAB and XYZ profiles, and reading a
    profile off the host is not portable. It is a v2 profile with the minimum
    tag set a matrix/TRC reader needs, which is all Qt consults.
    """
    tags: list[tuple[bytes, bytes]] = [
        (b"desc", _text_description_tag("Adobe RGB (1998) [arraw test fixture]")),
        (b"rXYZ", _xyz_tag(_ADOBE_RGB_PRIMARIES["rXYZ"])),
        (b"gXYZ", _xyz_tag(_ADOBE_RGB_PRIMARIES["gXYZ"])),
        (b"bXYZ", _xyz_tag(_ADOBE_RGB_PRIMARIES["bXYZ"])),
        (b"rTRC", _curve_tag(_ADOBE_RGB_GAMMA)),
        (b"gTRC", _curve_tag(_ADOBE_RGB_GAMMA)),
        (b"bTRC", _curve_tag(_ADOBE_RGB_GAMMA)),
        (b"wtpt", _xyz_tag(_D50)),
        (b"cprt", _text_tag("Public domain test fixture")),
    ]

    table_size = 4 + 12 * len(tags)
    offset = 128 + table_size
    table = bytearray(struct.pack(">I", len(tags)))
    body = bytearray()
    for signature, data in tags:
        table += signature + struct.pack(">II", offset, len(data))
        padding = -len(data) % 4
        body += data + bytes(padding)
        offset += len(data) + padding

    total = 128 + table_size + len(body)
    header = (
        struct.pack(">I", total)
        + b"\x00\x00\x00\x00"  # preferred CMM: none
        + struct.pack(">I", 0x02100000)  # version 2.1
        + b"mntr"
        + b"RGB "
        + b"XYZ "
        + struct.pack(">HHHHHH", 2026, 1, 1, 0, 0, 0)  # fixed, to stay reproducible
        + b"acsp"
        + bytes(4 * 4)  # platform, flags, manufacturer, model
        + bytes(8)  # attributes
        + struct.pack(">I", 0)  # rendering intent: perceptual
        + b"".join(_s15f16(component) for component in _D50)
        + bytes(4)  # creator
        + bytes(16)  # profile ID
        + bytes(28)  # reserved
    )
    assert len(header) == 128, "ICC header must be exactly 128 bytes"
    return bytes(header) + bytes(table) + bytes(body)


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------


def main() -> None:
    here = pathlib.Path(__file__).parent
    card = render_card()
    flat = [value for pixel in card for value in pixel]

    # The one fixture the round-trip test reads: plain 8-bit sRGB, the shape
    # that a browser, a phone and every screenshot tool produce.
    write_png(here / "testcard-61x41-srgb8.png", flat, colour_type=RGB,
              extra_chunks=(srgb_chunk(),))

    # --- Generated for the import tests that do not exist yet. ---
    # loadImage has no unit tests at all, and the round-trip fixture above
    # exercises exactly one path through its chooseLayout. These cover the
    # rest. Do not delete them for being unused; see README.md.

    # Wide input: must not be narrowed to eight bits on the way in.
    write_png(here / "testcard-61x41-srgb16.png", [value * 257 for value in flat],
              colour_type=RGB, bit_depth=16, extra_chunks=(srgb_chunk(),))

    # Single-channel input: must arrive as neutral RGB, not a channel swap.
    luma = [round(0.2126 * r + 0.7152 * g + 0.0722 * b) for r, g, b in card]
    write_png(here / "testcard-61x41-grey8.png", luma, colour_type=GREY,
              extra_chunks=(srgb_chunk(),))

    # Straight alpha ramping left to right, including fully transparent and
    # fully opaque columns: the colour conversion inside loadImage must not
    # premultiply, and exportImage must refuse this one as a JPEG.
    with_alpha: list[int] = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            with_alpha.extend(card[y * WIDTH + x])
            with_alpha.append(round(255 * x / (WIDTH - 1)))
    write_png(here / "testcard-61x41-alpha8.png", with_alpha, colour_type=RGBA,
              extra_chunks=(srgb_chunk(),))

    # No colour chunk at all: exercises the "untagged means sRGB" fallback, so
    # it must decode identically to testcard-61x41-srgb8.png.
    write_png(here / "testcard-61x41-untagged8.png", flat, colour_type=RGB)

    # Same numbers, different meaning: an honoured profile makes these pixels
    # decode to visibly different colours from the sRGB fixture.
    write_png(here / "testcard-61x41-adobergb8.png", flat, colour_type=RGB,
              extra_chunks=(iccp_chunk("Adobe RGB (1998)", adobe_rgb_profile()),))

    for path in sorted(here.glob("*.png")):
        print(f"{path.name}: {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
