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


def test_colour_defaults():
    s = arraw.DevelopSettings()
    assert (s.color.saturation, s.color.vibrance) == (0.0, 0.0)
    for band in ("red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"):
        assert getattr(s.hsl, band) == arraw.HueBand()
        assert getattr(s.black_and_white, band) == 0.0
    assert (s.hsl.red.hue, s.hsl.red.saturation, s.hsl.red.luminance) == (0.0, 0.0, 0.0)
    assert s.black_and_white.convert_to_grayscale is False


def test_nested_defaults_equal_default_settings():
    assert arraw.DevelopSettings() == arraw.DevelopSettings(
        color=arraw.ColorSettings(),
        geometry=arraw.GeometrySettings(),
        tone=arraw.ToneSettings(),
        hsl=arraw.HslSettings(),
        black_and_white=arraw.BlackAndWhiteSettings(),
    )


def test_colour_settings_construct_and_round_trip_json():
    settings = arraw.DevelopSettings(
        color=arraw.ColorSettings(saturation=-20.0, vibrance=35.5),
        hsl=arraw.HslSettings(
            red=arraw.HueBand(hue=10.0, saturation=-5.0, luminance=2.5),
            magenta=arraw.HueBand(luminance=-40.0),
        ),
        black_and_white=arraw.BlackAndWhiteSettings(convert_to_grayscale=True, red=60.0, blue=-30.0),
    )
    assert settings.color.vibrance == 35.5
    assert settings.hsl.red.hue == 10.0
    assert settings.hsl.magenta.luminance == -40.0
    assert settings.black_and_white.convert_to_grayscale
    assert arraw.DevelopSettings.from_json(settings.to_json()) == settings
    assert arraw.DevelopSettings() != settings


def test_flat_keywords_reach_the_colour_leaves():
    settings = arraw.DevelopSettings().with_(
        saturation=15.0, hue_red=20.0, luminance_aqua=-30.0, convert_to_grayscale=True, gray_blue=45.0
    )
    assert settings.color.saturation == 15.0
    assert settings.hsl.red.hue == 20.0
    assert settings.hsl.aqua.luminance == -30.0
    assert settings.black_and_white.convert_to_grayscale
    assert settings.black_and_white.blue == 45.0


def test_curve_defaults_are_identities():
    curves = arraw.DevelopSettings().tone_curve
    assert curves == arraw.ToneCurveSettings()
    for curve in (curves.luma, curves.red, curves.green, curves.blue):
        assert curve == arraw.ToneCurve()
        assert curve.points == [(0.0, 0.0), (1.0, 1.0)]
        assert curve.is_identity


def test_curve_points_are_tuples_that_keep_their_spelling():
    curve = arraw.ToneCurve([[0, 0], (0.3, 0.7), (1.0, 1)])
    assert curve.points == [(0.0, 0.0), (0.3, 0.7), (1.0, 1.0)]
    assert not curve.is_identity
    assert arraw.ToneCurve([(0, 0), (1, 1)]).is_identity
    assert hash(curve) == hash(arraw.ToneCurve([(0, 0), (0.3, 0.7), (1, 1)]))
    assert curve.replace(points=[(0, 0), (1, 1)]).is_identity
    with pytest.raises(TypeError):
        arraw.ToneCurve([(0, 0), (1,)])
    with pytest.raises(TypeError):
        arraw.ToneCurve([(0, True), (1, 1)])


def test_curve_points_may_be_given_in_any_order_and_are_sorted():
    expected = [(0.0, 0.0), (0.3, 0.7), (1.0, 1.0)]
    assert arraw.ToneCurve([(1, 1), (0.3, 0.7), (0, 0)]).points == expected
    assert arraw.ToneCurve().replace(points=[(1, 1), (0.3, 0.7), (0, 0)]).points == expected
    flat = arraw.DevelopSettings().with_(tone_curve_green=[(1, 1), (0.3, 0.7), (0, 0)])
    assert flat.tone_curve.green.points == expected
    # An end a rounding error off its x is snapped, as in the sidecar and on the command line.
    assert arraw.ToneCurve([(0, 0), (0.5, 0.5), (1.0000004, 1)]).points[-1] == (1.0, 1.0)


@pytest.mark.parametrize("point", [b"\x00\x00", bytearray(b"\x00\x01"), "01"])
def test_a_curve_point_is_not_bytes_or_text(point):
    with pytest.raises(TypeError):
        arraw.ToneCurve([point, (1, 1)])


def test_curves_construct_flat_set_and_round_trip_json():
    settings = arraw.DevelopSettings(
        tone_curve=arraw.ToneCurveSettings(
            luma=arraw.ToneCurve([(0, 0), (0.25, 0.2), (1, 1)]),
            blue=arraw.ToneCurve([(0, 0.1), (1, 1)]),
        )
    )
    assert settings.tone_curve.luma.points[1] == (0.25, 0.2)
    assert settings.tone_curve.red.is_identity
    flat = arraw.DevelopSettings().with_(
        tone_curve_luma=[(0, 0), (0.25, 0.2), (1, 1)],
        tone_curve_blue=arraw.ToneCurve([(0, 0.1), (1, 1)]),
    )
    assert flat == settings
    assert arraw.DevelopSettings.from_json(settings.to_json()) == settings
    assert arraw.DevelopSettings() != settings
    with pytest.raises(TypeError):
        arraw.DevelopSettings().with_(tone_curve=0.5)


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
