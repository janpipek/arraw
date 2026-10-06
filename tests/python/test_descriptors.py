import re

import pytest

import arraw

# RANGES and UNRANGED are a deliberate golden copy of the descriptor table: a change
# to a bound or a new setting must be made here on purpose.
RAW_ONLY = {"temperature", "tint"}
RANGES = {
    "exposure": (-5.0, 5.0),
    "contrast": (-100.0, 100.0),
    "shadows": (-100.0, 100.0),
    "highlights": (-100.0, 100.0),
    "blacks": (-100.0, 100.0),
    "whites": (-100.0, 100.0),
    "filmic_highlights": (0.0, 100.0),
    "temperature": (2000.0, 12000.0),
    "tint": (-150.0, 150.0),
    "saturation": (-100.0, 100.0),
    "vibrance": (-100.0, 100.0),
    "straighten": (-45.0, 45.0),
}
BANDS = ("red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta")
HSL = {f"{kind}_{band}" for kind in ("hue", "saturation", "luminance") for band in BANDS}
GRAY = {f"gray_{band}" for band in BANDS}
RANGES.update({name: (-100.0, 100.0) for name in HSL | GRAY})
ZONES = ("shadow", "midtone", "highlight")
GRADE = {f"grade_{zone}_{kind}": (0.0, 360.0 if kind == "hue" else 100.0)
         for zone in ZONES for kind in ("hue", "saturation")}
GRADE.update({"grade_balance": (-100.0, 100.0), "grade_blending": (0.0, 100.0)})
RANGES.update(GRADE)
EFFECTS = {"vignette_amount": (-100.0, 100.0), "vignette_midpoint": (0.0, 100.0),
           "vignette_feather": (0.0, 100.0), "grain_amount": (0.0, 100.0),
           "grain_size": (0.0, 100.0), "grain_roughness": (0.0, 100.0),
           "grain_seed": (0.0, 4294967295.0)}
RANGES.update(EFFECTS)
DETAIL = {"luminance_noise_reduction": (0.0, 100.0), "luminance_noise_detail": (0.0, 100.0),
          "color_noise_reduction": (0.0, 100.0), "color_noise_smoothness": (0.0, 100.0)}
RANGES.update(DETAIL)
PRESENCE = {"texture": (-100.0, 100.0), "clarity": (-100.0, 100.0), "dehaze": (-100.0, 100.0)}
RANGES.update(PRESENCE)
CURVES = {"tone_curve_luma", "tone_curve_red", "tone_curve_green", "tone_curve_blue"}
UNRANGED = {"white_balance", "rotation", "flip_horizontal", "flip_vertical",
            "crop_rectangle", "crop_aspect", "convert_to_grayscale", "grain_model",
            "luminance_noise_filter"} | CURVES


def snake(key: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", key).lower()


@pytest.fixture(scope="module")
def descriptors():
    return arraw.setting_descriptors()


def test_one_row_per_leaf(descriptors):
    assert len(descriptors) == len(RANGES) + len(UNRANGED) == 79
    assert len({d.name for d in descriptors}) == len(RANGES) + len(UNRANGED) == 79
    assert {d.name for d in descriptors} == set(RANGES) | UNRANGED


def test_names_are_snake_case_of_keys(descriptors):
    for d in descriptors:
        assert d.name == snake(d.key)
        assert re.fullmatch(r"[a-z]+(_[a-z]+)*", d.name)


def test_ranges_match_cpp_constants(descriptors):
    for d in descriptors:
        if d.name in RANGES:
            assert d.range == pytest.approx(RANGES[d.name]), d.name
        else:
            assert d.range is None, d.name


def test_only_temperature_and_tint_are_raw_only(descriptors):
    raw_only = {d.name for d in descriptors if d.applies == arraw.Applicability.RAW_ONLY}
    assert raw_only == RAW_ONLY
    assert all(d.applies == arraw.Applicability.ALWAYS
               for d in descriptors if d.name not in RAW_ONLY)


def test_groups_and_stages(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in ("exposure", "contrast", "filmic_highlights"):
        assert by_name[name].group == arraw.SettingGroup.TONE
        assert by_name[name].affects == arraw.Stage.POINTWISE
    for name in ("white_balance", "temperature", "tint"):
        assert by_name[name].group == arraw.SettingGroup.COLOR
    for name in ("rotation", "straighten", "crop_rectangle", "crop_aspect",
                 "flip_horizontal", "flip_vertical"):
        assert by_name[name].group == arraw.SettingGroup.GEOMETRY
        assert by_name[name].affects == arraw.Stage.GEOMETRY


def test_colour_groups(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in ("saturation", "vibrance"):
        assert by_name[name].group == arraw.SettingGroup.COLOR
        assert by_name[name].affects == arraw.Stage.POINTWISE
    for name in HSL:
        assert by_name[name].group == arraw.SettingGroup.HSL
        assert by_name[name].affects == arraw.Stage.POINTWISE
    for name in GRAY | {"convert_to_grayscale"}:
        assert by_name[name].group == arraw.SettingGroup.BLACK_AND_WHITE
        assert by_name[name].affects == arraw.Stage.POINTWISE


def test_colour_grading_descriptors(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in GRADE:
        assert by_name[name].group == arraw.SettingGroup.COLOR_GRADING
        assert by_name[name].affects == arraw.Stage.POINTWISE
        assert by_name[name].applies == arraw.Applicability.ALWAYS


def test_effects_descriptors(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in set(EFFECTS) | {"grain_model"}:
        assert by_name[name].group == arraw.SettingGroup.EFFECTS
        assert by_name[name].affects == arraw.Stage.EFFECTS
        assert by_name[name].applies == arraw.Applicability.ALWAYS


def test_only_the_grain_seed_is_the_photographs_own(descriptors):
    own = {d.name for d in descriptors if d.scope == arraw.SettingScope.PHOTO}
    assert own == {"grain_seed"}
    assert all(d.scope == arraw.SettingScope.LOOK for d in descriptors if d.name not in own)


def test_curve_descriptors(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in CURVES:
        assert by_name[name].range is None
        assert by_name[name].group == arraw.SettingGroup.TONE_CURVE
        assert by_name[name].affects == arraw.Stage.POINTWISE
        assert by_name[name].applies == arraw.Applicability.ALWAYS


def test_descriptors_are_unhashable_but_comparable(descriptors):
    assert descriptors == arraw.setting_descriptors()
    with pytest.raises(TypeError):
        hash(descriptors[0])


def test_noise_reduction_descriptors(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in set(DETAIL) | {"luminance_noise_filter"}:
        assert by_name[name].group == arraw.SettingGroup.DETAIL
        assert by_name[name].affects == arraw.Stage.DENOISE
        assert by_name[name].applies == arraw.Applicability.ALWAYS


def test_presence_descriptors(descriptors):
    by_name = {d.name: d for d in descriptors}
    for name in PRESENCE:
        assert by_name[name].group == arraw.SettingGroup.PRESENCE
        assert by_name[name].affects == arraw.Stage.POINTWISE
        assert by_name[name].applies == arraw.Applicability.ALWAYS
