import gc

import numpy as np
import pytest

import arraw


def test_sample_is_perceptual_and_sized_as_develop(dng):
    photo = arraw.open(dng).with_(exposure=0.5, contrast=20, straighten=5.0)
    tapped = arraw.sample(photo, arraw.Tap.CURVE_INPUT, size=12)
    developed = arraw.develop(photo, size=12)
    assert tapped.encoding == arraw.NamedEncoding.REC2020_GAMMA22
    assert tapped.format == arraw.PixelFormat.RGBA_F32
    assert tapped.size == developed.size


def test_sample_photo_equals_sample_loaded_buffer(dng):
    photo = arraw.open(dng).with_(exposure=-0.3, shadows=25)
    direct = arraw.sample(photo, arraw.Tap.CURVE_INPUT)
    stepwise = arraw.sample(photo.load(), arraw.Tap.CURVE_INPUT, photo.state)
    assert np.array_equal(direct.pixels, stepwise.pixels)


def test_sample_ignores_the_curves(dng):
    photo = arraw.open(dng).with_(exposure=0.4)
    curved = photo.with_(tone_curve_luma=[(0.0, 0.2), (1.0, 0.8)], saturation=30)
    assert np.array_equal(arraw.sample(photo, arraw.Tap.CURVE_INPUT).pixels,
                          arraw.sample(curved, arraw.Tap.CURVE_INPUT).pixels)
    assert arraw.curve_histogram(photo) == arraw.curve_histogram(curved)
    assert arraw.curve_histogram(photo) != arraw.curve_histogram(photo.with_(exposure=1.5))


def test_curve_histogram_counts_every_pixel_once(dng):
    photo = arraw.open(dng)
    histogram = arraw.curve_histogram(photo, size=16)
    assert len(histogram.luma) == arraw.CURVE_HISTOGRAM_BINS == 256
    for bins in (histogram.luma, histogram.red, histogram.green, histogram.blue):
        assert isinstance(bins, np.ndarray)
        assert bins.dtype == np.uint64
        assert bins.shape == (arraw.CURVE_HISTOGRAM_BINS,)
        assert not bins.flags.writeable
        assert int(bins.sum()) == histogram.pixels
        with pytest.raises(ValueError):
            bins[0] = 1
    # The histogram always resizes bilinearly, so no ringing reaches the end bins.
    tapped = arraw.sample(photo, arraw.Tap.CURVE_INPUT, size=16, filter=arraw.ResizeFilter.BILINEAR)
    assert histogram.pixels == tapped.size.width * tapped.size.height
    assert arraw.curve_histogram(tapped) == histogram
    buffer = photo.load()
    assert arraw.curve_histogram(buffer, photo.state, size=16) == histogram


def test_curve_histogram_bins_outlive_the_histogram(dng):
    histogram = arraw.curve_histogram(arraw.open(dng), size=16)
    pixels = histogram.pixels
    bins = histogram.luma
    del histogram
    gc.collect()
    assert int(bins.sum()) == pixels


def test_curve_histogram_defaults_to_a_bounded_render(tmp_path, dng):
    photo = arraw.open(dng)
    large = arraw.develop(photo, size=1500, allow_upscale=True)
    arraw.save(large, tmp_path / "large.png")
    buffer = arraw.load(tmp_path / "large.png")
    assert buffer.size.width == 1500
    bounded = arraw.resolved_size(1024, buffer.size)
    histogram = arraw.curve_histogram(buffer, arraw.DevelopState())
    assert histogram.pixels == bounded.width * bounded.height
    full = arraw.curve_histogram(buffer, arraw.DevelopState(), size=None)
    assert full.pixels == buffer.size.width * buffer.size.height
    with pytest.raises(TypeError):
        arraw.curve_histogram(photo, filter=arraw.ResizeFilter.LANCZOS3)


def test_curve_histogram_refuses_a_buffer_that_is_not_a_sample(dng):
    with pytest.raises(ValueError):
        arraw.curve_histogram(arraw.load(dng))


def test_perceptual_encoding_is_not_an_output(tmp_path, dng):
    tapped = arraw.sample(arraw.open(dng), arraw.Tap.CURVE_INPUT)
    with pytest.raises(ValueError):
        arraw.save(tapped, tmp_path / "out.png", encoding=arraw.NamedEncoding.REC2020_GAMMA22)
