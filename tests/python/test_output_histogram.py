import numpy as np
import pytest

import arraw


def channels(histogram):
    return (histogram.luma, histogram.red, histogram.green, histogram.blue)


def test_counts_a_developed_buffer_as_srgb(dng):
    developed = arraw.develop(arraw.open(dng), size=16)
    assert developed.encoding == arraw.NamedEncoding.LINEAR_REC2020
    histogram = arraw.output_histogram(developed)
    assert histogram.encoding == arraw.NamedEncoding.SRGB
    assert arraw.OUTPUT_HISTOGRAM_BINS == 256
    assert histogram.pixels == developed.size.width * developed.size.height
    for bins in channels(histogram):
        assert bins.dtype == np.uint64
        assert bins.shape == (arraw.OUTPUT_HISTOGRAM_BINS,)
        assert int(bins.sum()) == histogram.pixels
    assert histogram == arraw.output_histogram(developed, arraw.NamedEncoding.SRGB)
    p3 = arraw.output_histogram(developed, arraw.NamedEncoding.DISPLAY_P3)
    assert p3.encoding == arraw.NamedEncoding.DISPLAY_P3


def test_counts_an_encoded_buffer_as_is(dng):
    # Python cannot make a buffer that is already in an output encoding (load() and develop() hand
    # back the working one), so this checks what it can: the default is sRGB, and the encoding
    # argument really chooses the encoding the developed buffer is counted in.
    developed = arraw.develop(arraw.open(dng).with_(exposure=1.0), size=16)
    default = arraw.output_histogram(developed)
    assert default == arraw.output_histogram(developed, arraw.NamedEncoding.SRGB)
    assert default != arraw.output_histogram(developed, arraw.NamedEncoding.ADOBE_RGB)


def test_encoding_mismatch_raises(dng):
    developed = arraw.develop(arraw.open(dng), size=16)
    with pytest.raises(ValueError):
        arraw.output_histogram(developed, arraw.NamedEncoding.LINEAR_REC2020)
    with pytest.raises(ValueError):
        arraw.output_histogram(developed, arraw.NamedEncoding.REC2020_GAMMA22)
    # A camera-native buffer is not developed yet, so it cannot be converted either.
    with pytest.raises(ValueError):
        arraw.output_histogram(arraw.load(dng))


def test_perceptual_buffer_raises(dng):
    tapped = arraw.sample(arraw.open(dng), arraw.Tap.CURVE_INPUT, size=16)
    assert tapped.encoding == arraw.NamedEncoding.REC2020_GAMMA22
    with pytest.raises(ValueError):
        arraw.output_histogram(tapped)
    with pytest.raises(ValueError):
        arraw.output_histogram(tapped, arraw.NamedEncoding.SRGB)


def test_arrays_are_read_only(dng):
    histogram = arraw.output_histogram(arraw.develop(arraw.open(dng), size=16))
    for bins in channels(histogram):
        assert not bins.flags.writeable
        with pytest.raises(ValueError):
            bins[0] = 1
    with pytest.raises(AttributeError):
        histogram.pixels = 0


def test_equals_histogram_of_saved_png(tmp_path, dng):
    developed = arraw.develop(arraw.open(dng).with_(exposure=0.8), size=20)
    arraw.save(developed, tmp_path / "b.png", bit_depth=8)
    # load() hands back the working encoding, which counts as sRGB again code for code.
    saved = arraw.output_histogram(arraw.load(tmp_path / "b.png"))
    assert arraw.output_histogram(developed) == saved
