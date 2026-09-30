import numpy as np
import pytest

import arraw


def test_develop_photo_equals_develop_loaded_buffer(dng):
    photo = arraw.open(dng).with_(exposure=0.4, contrast=15, shadows=10)
    direct = arraw.develop(photo)
    stepwise = arraw.develop(photo.load(), photo.settings)
    assert direct.size == stepwise.size
    assert direct.format == stepwise.format
    assert np.array_equal(direct.pixels, stepwise.pixels)


def test_develop_photo_equals_buffer_for_bayer(bayer_dng):
    photo = arraw.open(bayer_dng).with_(exposure=-0.5)
    assert np.array_equal(arraw.develop(photo).pixels,
                          arraw.develop(photo.load(), photo.settings).pixels)


def test_develop_photo_with_explicit_settings_overrides_own(dng):
    photo = arraw.open(dng).with_(exposure=2.0)
    other = arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=-1.0))
    assert np.array_equal(arraw.develop(photo, other).pixels,
                          arraw.develop(photo.load(), other).pixels)


def test_develop_without_settings_uses_defaults(dng):
    buf = arraw.load(dng)
    assert np.array_equal(arraw.develop(buf).pixels,
                          arraw.develop(buf, arraw.DevelopSettings()).pixels)


def test_develop_does_not_modify_source(dng):
    buf = arraw.load(dng)
    before = buf.pixels.copy()
    arraw.develop(buf, arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=2.0)))
    assert np.array_equal(buf.pixels, before)


def test_output_is_rgba_with_opaque_alpha(dng):
    px = arraw.develop(arraw.open(dng)).pixels
    assert px.shape[-1] == 4
    assert np.all(px[..., 3] == 1.0)
    assert np.all(np.isfinite(px))


def test_exposure_plus_one_doubles_linear_values(dng):
    # Filmic roll-off only bends values near white, so turn it off and look at
    # pixels well below it.
    def developed(exposure):
        photo = arraw.open(dng).with_(exposure=exposure, filmic_highlights=0)
        return arraw.develop(photo).pixels[..., :3]

    base, bright = developed(0.0), developed(1.0)
    mask = (base > 0.02) & (base < 0.3)
    assert mask.sum() > 20, "fixture has too few mid-tone samples for this check"
    ratio = bright[mask] / base[mask]
    assert np.median(ratio) == pytest.approx(2.0, rel=0.03)
    assert np.all(np.abs(ratio - 2.0) < 0.2)


def test_exposure_is_monotonic(dng):
    px = [arraw.develop(arraw.open(dng).with_(exposure=e)).pixels[..., :3].mean()
          for e in (-1.0, 0.0, 1.0)]
    assert px[0] < px[1] < px[2]


def test_geometry_changes_output_size(dng):
    out = arraw.develop(arraw.open(dng).with_(rotation=arraw.QuarterTurn.CLOCKWISE_90))
    assert out.size == arraw.ImageSize(24, 32)
    assert out.pixels.shape == (32, 24, 4)


def test_flip_reverses_columns(dng):
    plain = arraw.develop(arraw.open(dng)).pixels
    flipped = arraw.develop(arraw.open(dng).with_(flip_horizontal=True)).pixels
    assert np.allclose(flipped, plain[:, ::-1], atol=1e-6)


def test_orientation_is_honoured(rotated_dng):
    out = arraw.develop(arraw.open(rotated_dng))
    assert out.size == arraw.ImageSize(24, 32)


def test_develop_rejects_other_types():
    with pytest.raises(TypeError):
        arraw.develop("not an image")
