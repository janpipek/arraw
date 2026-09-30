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
}


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
    for group in (settings.color, settings.tone, settings.geometry):
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
    default = leaves(photo.settings)
    value = sample(descriptor)
    changed = photo.with_(**{descriptor.name: value})
    after = leaves(changed.settings)
    assert same(after[descriptor.name], value)
    assert after[descriptor.name] != default[descriptor.name]
    for name in default:
        if name != descriptor.name:
            assert same(after[name], default[name]), f"{name} moved when setting {descriptor.name}"


def test_all_descriptors_at_once(photo):
    kwargs = {d.name: sample(d) for d in arraw.setting_descriptors()}
    after = leaves(photo.with_(**kwargs).settings)
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
    assert photo.with_(exposure=1).settings.tone.exposure == 1.0


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
    before = photo.settings
    changed = photo.with_(exposure=1.0, crop_aspect=arraw.OriginalCropAspect())
    assert photo.settings == before
    assert photo == arraw.open(photo.path)
    assert changed != photo
    assert changed.path == photo.path
    assert changed.metadata == photo.metadata


def test_with_nothing_is_equal(photo):
    assert photo.with_() == photo


def test_none_clears_optional_values(photo):
    custom = photo.with_(white_balance=arraw.WhiteBalanceMode.CUSTOM, temperature=5200,
                         tint=10, crop_rectangle=arraw.UprightCropRect(0.1, 0.1, 0.9, 0.9))
    assert custom.settings.color.temperature == pytest.approx(5200)
    assert custom.settings.color.tint == pytest.approx(10)
    assert custom.settings.geometry.crop.rectangle is not None
    cleared = custom.with_(temperature=None, tint=None, crop_rectangle=None)
    assert cleared.settings.color.temperature is None
    assert cleared.settings.color.tint is None
    assert cleared.settings.geometry.crop.rectangle is None
    # Only the cleared fields changed.
    assert cleared.settings.color.white_balance == arraw.WhiteBalanceMode.CUSTOM


def test_none_for_crop_aspect_is_rejected(photo):
    with pytest.raises(TypeError):
        photo.with_(crop_aspect=None)


def test_settings_then_flat_keys(photo):
    base = arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=1.0, contrast=20.0))
    result = photo.with_(settings=base, exposure=2.0)
    assert result.settings.tone.exposure == pytest.approx(2.0)
    assert result.settings.tone.contrast == pytest.approx(20.0)


def test_settings_replacement_alone(photo):
    base = arraw.DevelopSettings(tone=arraw.ToneSettings(shadows=30.0))
    assert photo.with_(settings=base).settings == base
    assert photo.with_(base).settings == base


def test_settings_replacement_is_validated(photo):
    bad = arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=99.0))
    with pytest.raises(ValueError):
        photo.with_(settings=bad)


def test_photo_equality_follows_settings(photo):
    assert photo.with_(exposure=0.5) == photo.with_(exposure=0.5)
    assert photo.with_(exposure=0.5) != photo.with_(exposure=0.6)


def test_photo_repr_names_the_file(photo, dng):
    assert dng.name in repr(photo)


def test_open_missing_file_raises(tmp_path):
    with pytest.raises(RuntimeError):
        arraw.open(tmp_path / "missing.dng")


def test_open_accepts_str_and_path(dng):
    assert arraw.open(str(dng)) == arraw.open(dng)
