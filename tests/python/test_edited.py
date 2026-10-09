import shutil
import subprocess

import numpy as np
import pytest

import arraw

RECTANGLE = arraw.UprightCropRect(0.1, 0.2, 0.9, 0.8)


def crop_of(photo):
    return photo.state.settings.geometry.crop


@pytest.fixture
def photo(dng):
    return arraw.open(dng, sidecar=False)


def test_edited_sets_a_plain_setting_like_with(photo):
    assert photo.edited(exposure=0.5) == photo.with_(exposure=0.5)
    assert photo.edited(exposure=0.5).state.settings.tone.exposure == pytest.approx(0.5)
    assert photo.edited() == photo


def test_a_temperature_makes_the_white_balance_custom(photo):
    assert photo.state.settings.color.white_balance == arraw.WhiteBalanceMode.AS_SHOT
    edited = photo.edited(temperature=5000.0)
    assert edited.state.settings.color.white_balance == arraw.WhiteBalanceMode.CUSTOM
    assert edited.state.settings.color.temperature == pytest.approx(5000.0)
    # `with_` applies no rules: the temperature is assigned and the mode left alone.
    assigned = photo.with_(temperature=5000.0)
    assert assigned.state.settings.color.white_balance == arraw.WhiteBalanceMode.AS_SHOT


def test_grain_turned_on_gets_a_seed(photo):
    assert photo.state.settings.effects.grain.seed == 0
    assert photo.edited(grain_amount=50).state.settings.effects.grain.seed != 0
    assert photo.with_(grain_amount=50).state.settings.effects.grain.seed == 0


def test_a_turn_carries_the_crop(photo):
    cropped = photo.edited(crop_rectangle=RECTANGLE)
    assert crop_of(cropped).rectangle == RECTANGLE
    turned = cropped.edited(rotation=arraw.QuarterTurn.CLOCKWISE_90)
    assert turned.state.settings.geometry.rotation == arraw.QuarterTurn.CLOCKWISE_90
    assert crop_of(turned).rectangle != RECTANGLE
    assert turned.state == arraw.turned(photo.metadata, cropped.state, True)
    # `with_` leaves the rectangle where it was.
    assert crop_of(cropped.with_(rotation=arraw.QuarterTurn.CLOCKWISE_90)).rectangle == RECTANGLE


def test_keywords_apply_in_the_order_given(photo):
    ratio = arraw.CropRatio(1.0)
    aspect_then_rectangle = photo.edited(crop_aspect=ratio, crop_rectangle=RECTANGLE)
    rectangle_then_aspect = photo.edited(crop_rectangle=RECTANGLE, crop_aspect=ratio)
    # A rectangle frees the aspect; an aspect then fits the crop inside the rectangle.
    assert crop_of(aspect_then_rectangle).aspect == arraw.FreeCropAspect()
    assert crop_of(aspect_then_rectangle).rectangle == RECTANGLE
    assert crop_of(rectangle_then_aspect).aspect == ratio
    assert crop_of(rectangle_then_aspect).rectangle != RECTANGLE
    assert aspect_then_rectangle != rectangle_then_aspect


def test_operations_equal_their_keywords(photo):
    metadata = photo.metadata
    cropped = photo.edited(crop_rectangle=RECTANGLE)
    assert arraw.with_aspect(metadata, cropped.state, arraw.CropRatio(1.5)) == cropped.edited(
        crop_aspect=arraw.CropRatio(1.5)
    ).state
    assert arraw.flipped(metadata, cropped.state, True) == cropped.edited(flip_horizontal=True).state
    assert arraw.with_crop_reset(metadata, cropped.state) == cropped.edited(crop_rectangle=None).state
    locked = arraw.with_locked_aspect(metadata, cropped.state)
    assert isinstance(crop_of_state(locked).aspect, arraw.CropRatio)
    swapped = arraw.with_swapped_orientation(metadata, locked)
    assert crop_of_state(swapped).aspect.width_over_height == pytest.approx(
        1 / crop_of_state(locked).aspect.width_over_height
    )


def crop_of_state(state):
    return state.settings.geometry.crop


def test_a_displayed_straighten_follows_the_flips(photo):
    flipped = photo.edited(flip_horizontal=True)
    straightened = arraw.with_displayed_straighten(photo.metadata, flipped.state, 3.0)
    assert arraw.displayed_straighten(straightened) == pytest.approx(3.0)
    assert straightened.settings.geometry.straighten == pytest.approx(-3.0)


def test_errors(photo):
    with pytest.raises(TypeError):
        photo.edited(no_such_setting=1)
    with pytest.raises(TypeError):
        photo.edited(exposure="bright")
    with pytest.raises(ValueError):
        photo.edited(exposure=1000.0)
    with pytest.raises(ValueError):
        photo.edited(straighten=60.0)
    with pytest.raises(ValueError):
        photo.edited(crop_aspect=arraw.CropRatio(-1.0))


def test_edited_matches_the_cli_for_a_rotation_over_a_sidecar_crop(dng, cli, tmp_path):
    source = tmp_path / dng.name
    shutil.copy(dng, source)
    cropped = arraw.open(source, sidecar=False).edited(crop_rectangle=RECTANGLE)
    arraw.write_sidecar(cropped)

    edited = arraw.open(source).edited(rotation=arraw.QuarterTurn.CLOCKWISE_90, straighten=0.0)
    py_out = tmp_path / "py.tif"
    arraw.save(arraw.develop(edited), py_out, bit_depth=16)

    outdir = tmp_path / "cli"
    outdir.mkdir()
    result = subprocess.run(
        [str(cli), "export", "--quiet", "--device", "cpu", "--format", "tiff", "--bit-depth", "16",
         "-o", str(outdir), "--rotate", "90", str(source)],
        capture_output=True, text=True, timeout=120,
    )
    assert result.returncode == 0, result.stderr
    (cli_out,) = list(outdir.iterdir())

    py_px = arraw.load(py_out).pixels.astype(np.int64)
    cli_px = arraw.load(cli_out).pixels.astype(np.int64)
    assert py_px.shape == cli_px.shape
    assert np.array_equal(py_px, cli_px)
