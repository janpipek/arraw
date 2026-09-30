import pytest

import arraw


def test_defaults_match_cpp_defaults():
    s = arraw.DevelopSettings()
    assert s.color.white_balance == arraw.WhiteBalanceMode.AS_SHOT
    assert s.color.temperature is None
    assert s.color.tint is None
    t = s.tone
    assert (t.exposure, t.contrast, t.shadows, t.highlights, t.blacks, t.whites) == (0,) * 6
    assert t.filmic_highlights == 25.0
    g = s.geometry
    assert g.rotation == arraw.QuarterTurn.NONE
    assert not g.flip_horizontal and not g.flip_vertical
    assert g.straighten == 0.0
    assert g.crop.rectangle is None
    assert g.crop.aspect == arraw.FreeCropAspect()


def test_nested_defaults_equal_default_settings():
    assert arraw.DevelopSettings() == arraw.DevelopSettings(
        color=arraw.ColorSettings(),
        geometry=arraw.GeometrySettings(),
        tone=arraw.ToneSettings(),
    )


@pytest.mark.parametrize(
    "obj, attr, value",
    [
        (arraw.ToneSettings(), "exposure", 1.0),
        (arraw.ColorSettings(), "tint", 3.0),
        (arraw.GeometrySettings(), "rotation", arraw.QuarterTurn.CLOCKWISE_90),
        (arraw.UprightCropRect(), "left", 0.1),
        (arraw.CropRatio(1.5), "width_over_height", 2.0),
        (arraw.DevelopSettings(), "tone", arraw.ToneSettings()),
        (arraw.ImageSize(1, 2), "width", 5),
    ],
)
def test_frozen_assignment_raises(obj, attr, value):
    with pytest.raises(AttributeError):
        setattr(obj, attr, value)


def test_nested_mutation_is_impossible():
    s = arraw.DevelopSettings()
    with pytest.raises(AttributeError):
        s.tone.exposure = 1.0
    assert s.tone.exposure == 0.0


def test_no_dynamic_attributes():
    with pytest.raises(AttributeError):
        arraw.ToneSettings().nonsense = 1


def test_replace_returns_modified_copy():
    tone = arraw.ToneSettings(exposure=1.0, contrast=10.0)
    changed = tone.replace(contrast=-5.0)
    assert changed.contrast == -5.0
    assert changed.exposure == 1.0
    assert tone.contrast == 10.0
    assert changed is not tone


def test_replace_nested():
    s = arraw.DevelopSettings()
    s2 = s.replace(tone=arraw.ToneSettings(exposure=2.0))
    assert s2.tone.exposure == 2.0
    assert s2.geometry == s.geometry
    assert s.tone.exposure == 0.0


def test_replace_unknown_key_is_type_error():
    with pytest.raises(TypeError):
        arraw.ToneSettings().replace(bogus=1)


def test_replace_with_nothing_is_equal():
    t = arraw.ToneSettings(exposure=0.5)
    assert t.replace() == t


def test_keyword_constructor_rejects_unknown_keywords():
    with pytest.raises(TypeError):
        arraw.ToneSettings(bogus=1)


def test_frozen_settings_constructors_are_keyword_only():
    with pytest.raises(TypeError):
        arraw.ToneSettings(1.0, 2.0)
    assert arraw.CropRatio(1.5) == arraw.CropRatio(width_over_height=1.5)
    assert arraw.UprightCropRect(0.1, 0.2, 0.9, 0.8).right == pytest.approx(0.9)


def test_equality_and_hash():
    a = arraw.ToneSettings(exposure=0.5)
    b = arraw.ToneSettings(exposure=0.5)
    c = arraw.ToneSettings(exposure=0.6)
    assert a == b and a != c
    assert hash(a) == hash(b)
    assert len({a, b, c}) == 2
    assert hash(arraw.DevelopSettings()) == hash(arraw.DevelopSettings())
    assert hash(arraw.ImageSize(3, 4)) == hash(arraw.ImageSize(3, 4))
    assert arraw.ImageSize(3, 4) != arraw.ImageSize(4, 3)


def test_crop_aspect_variants_are_distinct():
    free, orig, ratio = arraw.FreeCropAspect(), arraw.OriginalCropAspect(), arraw.CropRatio(1.5)
    assert free != orig
    assert free == arraw.FreeCropAspect()
    assert ratio == arraw.CropRatio(1.5)
    assert ratio != arraw.CropRatio(2.0)
    assert ratio.width_over_height == 1.5
    assert len({free, orig, ratio}) == 3


@pytest.mark.parametrize(
    "aspect", [arraw.FreeCropAspect(), arraw.OriginalCropAspect(), arraw.CropRatio(2.0)]
)
def test_crop_aspect_lands_in_settings(aspect):
    crop = arraw.CropSettings(aspect=aspect)
    assert crop.aspect == aspect
    assert type(crop.aspect) is type(aspect)
    assert arraw.GeometrySettings(crop=crop).crop.aspect == aspect


def test_crop_rectangle_roundtrip():
    rect = arraw.UprightCropRect(0.1, 0.2, 0.9, 0.8)
    crop = arraw.CropSettings(rectangle=rect)
    assert crop.rectangle == rect
    assert (rect.left, rect.top, rect.right, rect.bottom) == pytest.approx((0.1, 0.2, 0.9, 0.8))


def test_repr_is_readable_and_evaluable():
    s = arraw.DevelopSettings(
        tone=arraw.ToneSettings(exposure=0.5),
        geometry=arraw.GeometrySettings(
            crop=arraw.CropSettings(aspect=arraw.CropRatio(1.5)),
            rotation=arraw.QuarterTurn.CLOCKWISE_90,
        ),
        color=arraw.ColorSettings(white_balance=arraw.WhiteBalanceMode.CUSTOM, temperature=5200.0),
    )
    text = repr(s)
    assert "exposure=0.5" in text and "CropRatio" in text and "temperature=5200" in text
    # The repr is constructor syntax, so evaluating it rebuilds an equal object.
    assert eval(text, vars(arraw)) == s


@pytest.mark.parametrize(
    "obj",
    [arraw.ToneSettings(exposure=0.25), arraw.ColorSettings(), arraw.FreeCropAspect(),
     arraw.CropRatio(1.5), arraw.UprightCropRect(0.1, 0.1, 0.8, 0.9), arraw.ImageSize(3, 4)],
)
def test_repr_roundtrip(obj):
    assert eval(repr(obj), vars(arraw)) == obj
