import gc

import numpy as np
import pytest

import arraw

DTYPES = {
    arraw.PixelFormat.RGB_U8: (np.uint8, 3),
    arraw.PixelFormat.RGBA_U8: (np.uint8, 4),
    arraw.PixelFormat.RGB_U16: (np.uint16, 3),
    arraw.PixelFormat.RGBA_U16: (np.uint16, 4),
    arraw.PixelFormat.RGB_F32: (np.float32, 3),
    arraw.PixelFormat.RGBA_F32: (np.float32, 4),
}


def test_png_metadata(png):
    meta = arraw.read_metadata(png)
    assert meta.size == arraw.ImageSize(61, 41)
    assert meta.orientation == arraw.ImageOrientation.NORMAL
    # Qt-decoded files report the working encoding, not the file's own space.
    assert meta.encoding == arraw.NamedEncoding.LINEAR_REC2020


def test_dng_metadata(dng):
    meta = arraw.open(dng).metadata
    assert meta.size == arraw.ImageSize(32, 24)
    assert meta.orientation == arraw.ImageOrientation.NORMAL
    assert isinstance(meta.encoding, arraw.CameraNative)
    assert not isinstance(meta.encoding, arraw.NamedEncoding)


def test_rotated_dng_reports_orientation(rotated_dng):
    meta = arraw.open(rotated_dng).metadata
    assert meta.orientation == arraw.ImageOrientation.ROTATE_90
    assert meta.size == arraw.ImageSize(32, 24)


def test_bayer_dng_metadata(bayer_dng):
    meta = arraw.read_metadata(bayer_dng)
    assert meta.size == arraw.ImageSize(32, 24)
    assert isinstance(meta.encoding, arraw.CameraNative)


def test_open_metadata_matches_read_metadata(dng, png):
    for path in (dng, png):
        assert arraw.open(path).metadata == arraw.read_metadata(path)
        assert arraw.open(path).path == path


def test_native_encoding_repr_and_equality(dng):
    enc = arraw.open(dng).metadata.encoding
    assert "CameraNative" in repr(enc)
    assert enc == arraw.open(dng).metadata.encoding


def test_load_matches_metadata(dng, png):
    for path in (dng, png):
        buf = arraw.open(path).load() if path == dng else arraw.load(path)
        meta = arraw.read_metadata(path)
        assert buf.size == meta.size
        assert buf.orientation == meta.orientation
        assert buf.encoding == meta.encoding


def test_image_buffer_is_not_constructible():
    with pytest.raises(TypeError):
        arraw.ImageBuffer()


def test_image_size_value_semantics():
    size = arraw.ImageSize(5, 7)
    assert (size.width, size.height) == (5, 7)
    assert "5" in repr(size) and "7" in repr(size)


@pytest.mark.parametrize("which", ["png", "dng"])
def test_pixels_shape_and_dtype_follow_format(request, which):
    path = request.getfixturevalue(which)
    buf = arraw.load(path)
    dtype, channels = DTYPES[buf.format]
    px = buf.pixels
    assert px.shape == (buf.size.height, buf.size.width, channels)
    assert px.dtype == dtype


def test_developed_output_is_float32_rgba(dng):
    out = arraw.develop(arraw.open(dng))
    assert out.format == arraw.PixelFormat.RGBA_F32
    assert out.pixels.shape == (24, 32, 4)
    assert out.pixels.dtype == np.float32


def test_pixels_are_zero_copy(dng):
    buf = arraw.load(dng)
    a, b = buf.pixels, buf.pixels
    assert np.shares_memory(a, b)
    assert a.ctypes.data == b.ctypes.data


def test_pixel_writes_are_visible_through_a_second_view(dng):
    buf = arraw.load(dng)
    a, b = buf.pixels, buf.pixels
    assert a.flags.writeable
    old = a[0, 0].copy()
    a[0, 0] = old + 1
    assert np.array_equal(b[0, 0], old + 1)


def test_pixel_writes_change_develop_output(dng):
    buf = arraw.load(dng)
    before = arraw.develop(buf).pixels.copy()
    buf.pixels[:] = 0
    after = arraw.develop(buf).pixels
    assert not np.array_equal(before, after)


def test_view_survives_buffer_deletion(dng):
    buf = arraw.load(dng)
    expected = buf.pixels.copy()
    view = buf.pixels
    del buf
    gc.collect()
    # Churn allocations so a freed buffer would be overwritten.
    junk = [np.random.rand(64, 64) for _ in range(50)]
    assert np.array_equal(view, expected)
    del junk


def test_developed_view_survives_deletion(dng):
    out = arraw.develop(arraw.open(dng))
    expected = out.pixels.copy()
    view = out.pixels
    del out
    gc.collect()
    assert np.array_equal(view, expected)
