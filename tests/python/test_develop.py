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


# --- rendering to a size -----------------------------------------------------


def _dims(buffer):
    return (buffer.size.width, buffer.size.height)


def test_size_int_is_the_long_edge(png):
    buf = arraw.load(png)  # 61x41
    assert _dims(arraw.develop(buf, size=30)) == (30, 20)
    portrait = arraw.DevelopSettings(
        geometry=arraw.GeometrySettings(rotation=arraw.QuarterTurn.CLOCKWISE_90))
    assert _dims(arraw.develop(buf, portrait, size=30)) == (20, 30)


def test_size_tuple_fits_inside_a_box(png):
    buf = arraw.load(png)
    assert _dims(arraw.develop(buf, size=(40, 40))) == (40, 27)
    assert _dims(arraw.develop(buf, size=(100, 20))) == (30, 20)


def test_size_float_is_a_scale_factor(png):
    buf = arraw.load(png)
    assert _dims(arraw.develop(buf, size=0.5)) == (31, 21)
    assert _dims(arraw.develop(buf, size=0.25)) == (15, 10)


def test_size_applies_after_the_crop(dng):
    photo = arraw.open(dng).with_(crop_rectangle=arraw.UprightCropRect(0.0, 0.0, 0.5, 1.0))
    whole = arraw.develop(photo)
    assert _dims(whole) == (16, 24)
    assert _dims(arraw.develop(photo, size=12)) == (8, 12)
    assert _dims(arraw.develop(photo, size=0.5)) == (8, 12)


def test_only_shrinks_unless_upscale_allowed(png):
    buf = arraw.load(png)
    assert _dims(arraw.develop(buf, size=500)) == (61, 41)
    assert _dims(arraw.develop(buf, size=(500, 500))) == (61, 41)
    assert _dims(arraw.develop(buf, size=2.0)) == (61, 41)
    assert _dims(arraw.develop(buf, size=122, allow_upscale=True)) == (122, 82)
    assert _dims(arraw.develop(buf, size=2.0, allow_upscale=True)) == (122, 82)


def test_filter_changes_pixels_and_lanczos_is_default(png):
    buf = arraw.load(png)
    default = arraw.develop(buf, size=20)
    lanczos = arraw.develop(buf, size=20, filter=arraw.ResizeFilter.LANCZOS3)
    bilinear = arraw.develop(buf, size=20, filter=arraw.ResizeFilter.BILINEAR)
    assert np.array_equal(default.pixels, lanczos.pixels)
    assert _dims(bilinear) == _dims(lanczos)
    assert not np.array_equal(bilinear.pixels, lanczos.pixels)


def test_resize_filter_enum_members():
    assert {m.name for m in arraw.ResizeFilter.__members__.values()} == {"LANCZOS3", "BILINEAR"}


def test_no_size_is_unchanged(png):
    buf = arraw.load(png)
    assert np.array_equal(arraw.develop(buf, size=None).pixels, arraw.develop(buf).pixels)


@pytest.mark.parametrize("size", [True, False, "30", (30,), (30, 20, 10), [30, 20], (30.0, 20),
                                  (True, 20), 30 + 0j, b"30", {"w": 1}, (30, None)])
def test_size_of_the_wrong_type_is_a_type_error(png, size):
    with pytest.raises(TypeError):
        arraw.develop(arraw.load(png), size=size)


@pytest.mark.parametrize("size", [0, -5, (0, 10), (10, 0), (-1, 10), 2**40, (2**40, 5),
                                  0.0, -0.5, float("nan"), float("inf"), -float("inf")])
def test_size_out_of_range_is_a_value_error(png, size):
    with pytest.raises(ValueError):
        arraw.develop(arraw.load(png), size=size)


@pytest.mark.parametrize("keywords", [{"filter": 0}, {"filter": "lanczos"},
                                      {"allow_upscale": 1}, {"allow_upscale": "yes"}])
def test_filter_and_upscale_are_strictly_typed(png, keywords):
    with pytest.raises(TypeError):
        arraw.develop(arraw.load(png), size=20, **keywords)


def test_size_keywords_are_keyword_only(png):
    with pytest.raises(TypeError):
        arraw.develop(arraw.load(png), None, 20)


@pytest.mark.parametrize("size", [None, 30, (40, 20), 0.5])
def test_develop_photo_with_size_equals_develop_loaded_buffer(dng, size):
    photo = arraw.open(dng).with_(exposure=0.4, contrast=15)
    direct = arraw.develop(photo, size=size, filter=arraw.ResizeFilter.BILINEAR)
    stepwise = arraw.develop(photo.load(), photo.settings, size=size,
                             filter=arraw.ResizeFilter.BILINEAR)
    assert _dims(direct) == _dims(stepwise)
    assert np.array_equal(direct.pixels, stepwise.pixels)


def test_develop_photo_with_size_allow_upscale(dng):
    photo = arraw.open(dng)
    assert _dims(arraw.develop(photo, size=64)) == (32, 24)
    assert _dims(arraw.develop(photo, size=64, allow_upscale=True)) == (64, 48)


def test_resolved_size_matches_develop(png):
    buf = arraw.load(png)
    for size in (30, (40, 40), 0.5, 500):
        got = arraw.resolved_size(size, buf.size)
        assert (got.width, got.height) == _dims(arraw.develop(buf, size=size))
    got = arraw.resolved_size(2.0, buf.size, allow_upscale=True)
    assert (got.width, got.height) == (122, 82)
    with pytest.raises(TypeError):
        arraw.resolved_size(True, buf.size)
