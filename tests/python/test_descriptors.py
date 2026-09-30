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
    "straighten": (-45.0, 45.0),
}
UNRANGED = {"white_balance", "rotation", "flip_horizontal", "flip_vertical",
            "crop_rectangle", "crop_aspect"}


def snake(key: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", key).lower()


@pytest.fixture(scope="module")
def descriptors():
    return arraw.setting_descriptors()


def test_one_row_per_leaf(descriptors):
    assert len(descriptors) == 16
    assert len({d.name for d in descriptors}) == 16
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


def test_descriptors_are_unhashable_but_comparable(descriptors):
    assert descriptors == arraw.setting_descriptors()
    with pytest.raises(TypeError):
        hash(descriptors[0])
