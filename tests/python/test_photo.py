import math

import pytest

import arraw

CENTER = {
    "white_balance": arraw.WhiteBalanceMode.CUSTOM,
    "rotation": arraw.QuarterTurn.CLOCKWISE_180,
    "flip_horizontal": True,
    "flip_vertical": True,
    "crop_rectangle": arraw.UprightCropRect(0.1, 0.1, 0.9, 0.9),
    "crop_aspect": arraw.CropRatio(1.5),
    "convert_to_grayscale": True,
    "tone_curve_luma": arraw.ToneCurve([(0.0, 0.0), (0.25, 0.4), (1.0, 1.0)]),
    "tone_curve_red": arraw.ToneCurve([(0.0, 0.1), (1.0, 1.0)]),
    "tone_curve_green": arraw.ToneCurve([(0.0, 0.0), (0.5, 0.3), (1.0, 0.9)]),
    "tone_curve_blue": arraw.ToneCurve([(0.0, 0.0), (0.2, 0.1), (0.8, 0.9), (1.0, 1.0)]),
}

BANDS = ("red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta")


def sample(descriptor):
    """Pick a non-default value for a setting from its descriptor."""
    if descriptor.range is not None:
        low, high = descriptor.range
        return low + 0.3 * (high - low)
    return CENTER[descriptor.name]


def leaves(settings):
    """Read every descriptor's field by walking the nested settings."""
    out = {}
    for d in arraw.setting_descriptors():
        out[d.name] = find(settings, d.name)
    return out


def find(settings, name):
    for kind in ("hue", "saturation", "luminance"):
        for band in BANDS:
            if name == f"{kind}_{band}":
                return getattr(getattr(settings.hsl, band), kind)
    if name.startswith("tone_curve"):
        return getattr(settings.tone_curve, name.removeprefix("tone_curve").lstrip("_") or "luma")
    if name.startswith("gray_"):
        return getattr(settings.black_and_white, name.removeprefix("gray_"))
    for group in (settings.color, settings.tone, settings.geometry, settings.black_and_white):
        if hasattr(group, name):
            return getattr(group, name)
        if name.startswith("crop_") and hasattr(group, "crop"):
            return getattr(group.crop, name.removeprefix("crop_"))
    raise AssertionError(f"no field for {name}")


def same(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return a == pytest.approx(b, rel=1e-6)
    if isinstance(a, arraw.UprightCropRect):
        return (a.left, a.top, a.right, a.bottom) == pytest.approx(
            (b.left, b.top, b.right, b.bottom))
    return a == b


@pytest.fixture
def photo(dng):
    return arraw.open(dng)


@pytest.mark.parametrize("descriptor", arraw.setting_descriptors(), ids=lambda d: d.name)
def test_every_descriptor_name_lands_in_its_field(photo, descriptor):
    default = leaves(photo.state.settings)
    value = sample(descriptor)
    changed = photo.with_(**{descriptor.name: value})
    after = leaves(changed.state.settings)
    assert same(after[descriptor.name], value)
    assert after[descriptor.name] != default[descriptor.name]
    for name in default:
        if name != descriptor.name:
            assert same(after[name], default[name]), f"{name} moved when setting {descriptor.name}"


def test_all_descriptors_at_once(photo):
    kwargs = {d.name: sample(d) for d in arraw.setting_descriptors()}
    after = leaves(photo.with_(**kwargs).state.settings)
    for name, value in kwargs.items():
        assert same(after[name], value), name


def test_unknown_key_is_type_error(photo):
    with pytest.raises(TypeError, match="bogus"):
        photo.with_(bogus=1)
    with pytest.raises(TypeError):
        photo.with_(exposure=1.0, camelCase=2)
    with pytest.raises(TypeError):
        photo.with_(filmicHighlights=10.0)  # keys are snake_case


@pytest.mark.parametrize(
    "key, value",
    [("exposure", "bright"), ("exposure", True), ("temperature", False), ("straighten", True),
     ("rotation", 1), ("white_balance", 0), ("flip_horizontal", 1)],
)
def test_wrong_value_type_is_type_error(photo, key, value):
    with pytest.raises(TypeError, match=key):
        photo.with_(**{key: value})


def test_int_is_accepted_for_a_number(photo):
    assert photo.with_(exposure=1).state.settings.tone.exposure == 1.0


@pytest.mark.parametrize(
    "key, value",
    [("exposure", 5.5), ("exposure", -5.5), ("contrast", 101), ("filmic_highlights", -1),
     ("temperature", 1500), ("temperature", 13000), ("tint", 200), ("straighten", 46)],
)
def test_out_of_range_is_value_error(photo, key, value):
    with pytest.raises(ValueError):
        photo.with_(**{key: value})


@pytest.mark.parametrize("key", ["exposure", "contrast", "temperature", "tint", "straighten"])
@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_non_finite_is_value_error(photo, key, bad):
    with pytest.raises(ValueError):
        photo.with_(**{key: bad})


def test_range_edges_are_accepted(photo):
    for d in arraw.setting_descriptors():
        if d.range is not None:
            low, high = d.range
            photo.with_(**{d.name: low})
            photo.with_(**{d.name: high})


def test_original_is_unchanged(photo):
    before = photo.state.settings
    changed = photo.with_(exposure=1.0, crop_aspect=arraw.OriginalCropAspect())
    assert photo.state.settings == before
    assert photo == arraw.open(photo.path)
    assert changed != photo
    assert changed.path == photo.path
    assert changed.metadata == photo.metadata


def test_with_nothing_is_equal(photo):
    assert photo.with_() == photo


def test_none_clears_optional_values(photo):
    custom = photo.with_(white_balance=arraw.WhiteBalanceMode.CUSTOM, temperature=5200,
                         tint=10, crop_rectangle=arraw.UprightCropRect(0.1, 0.1, 0.9, 0.9))
    assert custom.state.settings.color.temperature == pytest.approx(5200)
    assert custom.state.settings.color.tint == pytest.approx(10)
    assert custom.state.settings.geometry.crop.rectangle is not None
    cleared = custom.with_(temperature=None, tint=None, crop_rectangle=None)
    assert cleared.state.settings.color.temperature is None
    assert cleared.state.settings.color.tint is None
    assert cleared.state.settings.geometry.crop.rectangle is None
    # Only the cleared fields changed.
    assert cleared.state.settings.color.white_balance == arraw.WhiteBalanceMode.CUSTOM


def test_none_for_crop_aspect_is_rejected(photo):
    with pytest.raises(TypeError):
        photo.with_(crop_aspect=None)


def test_state_then_flat_keys(photo):
    base = arraw.DevelopState(
        settings=arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=1.0, contrast=20.0)))
    result = photo.with_(state=base, exposure=2.0)
    assert result.state.settings.tone.exposure == pytest.approx(2.0)
    assert result.state.settings.tone.contrast == pytest.approx(20.0)


def test_state_replacement_alone(photo):
    base = arraw.DevelopState(
        settings=arraw.DevelopSettings(tone=arraw.ToneSettings(shadows=30.0)))
    assert photo.with_(state=base).state == base
    assert photo.with_(base).state == base


def test_state_replacement_keeps_path_and_marks(photo):
    marked = photo.with_(rating=3)
    replaced = marked.with_(state=arraw.DevelopState(
        settings=arraw.DevelopSettings(tone=arraw.ToneSettings(shadows=30.0))))
    assert replaced.path == marked.path
    assert replaced.marks == marked.marks


def test_state_replacement_is_validated(photo):
    bad = arraw.DevelopState(
        settings=arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=99.0)))
    with pytest.raises(ValueError):
        photo.with_(state=bad)


def test_photo_has_no_settings_attribute(photo):
    assert not hasattr(photo, "settings")


def test_photo_equality_follows_state(photo):
    assert photo.with_(exposure=0.5) == photo.with_(exposure=0.5)
    assert photo.with_(exposure=0.5) != photo.with_(exposure=0.6)


def test_photo_repr_names_the_file(photo, dng):
    assert dng.name in repr(photo)


def test_open_missing_file_raises(tmp_path):
    with pytest.raises(RuntimeError):
        arraw.open(tmp_path / "missing.dng")


def test_open_accepts_str_and_path(dng):
    assert arraw.open(str(dng)) == arraw.open(dng)


@pytest.mark.parametrize("points", [[(0.1, 0.0), (1.0, 1.0)], [(0.0, 0.0), (0.5, 2.0), (1.0, 1.0)]])
def test_a_malformed_curve_is_refused_when_set_on_a_photo(photo, points):
    with pytest.raises(ValueError, match="tone curve"):
        photo.with_(tone_curve_luma=points)
