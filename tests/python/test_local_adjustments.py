"""Local adjustments (masks) in Python: the value types, the Photo edits, persistence, rendering
and parity with the command line (ADR 044, section 10)."""

import math
import os
import shutil
import subprocess
import sys

import numpy as np
import pytest

import arraw

DELTAS = [
    "relative_temperature",
    "relative_tint",
    "exposure",
    "contrast",
    "highlights",
    "shadows",
    "whites",
    "blacks",
    "texture",
    "clarity",
    "dehaze",
    "saturation",
    "vibrance",
]


@pytest.fixture
def photo(dng):
    return arraw.open(dng, sidecar=False)


def rendered(p, **kwargs):
    return arraw.develop(p, **kwargs).pixels


# ---- the value types -------------------------------------------------------------------------


def test_mask_defaults_and_positional_construction():
    linear = arraw.LinearMask()
    assert linear.from_ == (0.5, 0.25)
    assert linear.to == (0.5, 0.75)
    assert arraw.LinearMask((0.1, 0.2), (0.3, 0.4)).to == (0.3, 0.4)

    radial = arraw.RadialMask()
    assert radial.centre == (0.5, 0.5)
    assert (radial.radius_x, radial.radius_y, radial.angle, radial.feather) == (0.25, 0.25, 0.0, 0.5)
    assert arraw.RadialMask((0.1, 0.9), 0.2, 0.1, 15, 0.25).angle == 15.0


def test_points_read_back_as_the_decimal_that_was_written():
    assert arraw.LinearMask((0.3, 0.1), (0.7, 0.9)).from_ == (0.3, 0.1)
    assert arraw.RadialMask((0.3, 0.7), 0.3, 0.1).centre == (0.3, 0.7)


def test_points_refuse_what_is_no_pair_of_numbers():
    for bad in [(0.5,), (0.1, 0.2, 0.3), "ab", (True, 0.5), 3]:
        with pytest.raises(TypeError):
            arraw.LinearMask(bad, (0.5, 0.5))


def test_values_are_frozen_hashable_and_compare_by_value():
    mask = arraw.LinearMask((0.1, 0.2), (0.3, 0.4))
    with pytest.raises(AttributeError):
        mask.to = (0.0, 0.0)
    assert mask == arraw.LinearMask((0.1, 0.2), (0.3, 0.4))
    assert mask != arraw.LinearMask((0.1, 0.2), (0.3, 0.5))
    assert hash(mask) == hash(arraw.LinearMask((0.1, 0.2), (0.3, 0.4)))
    assert mask.replace(to=(0.5, 0.5)).to == (0.5, 0.5)
    assert repr(mask) == "LinearMask(from_=(0.1, 0.2), to=(0.3, 0.4))"
    # A different kind is never equal.
    assert arraw.LinearMask() != arraw.RadialMask()


def test_local_deltas_are_the_tables_thirteen_controls_in_order():
    deltas = arraw.LocalDeltas()
    for name in DELTAS:
        assert getattr(deltas, name) == 0.0
    assert arraw.LocalDeltas(exposure=0.5, dehaze=-20).replace(clarity=3).clarity == 3.0
    # The repr lists them in the table's order.
    text = repr(arraw.LocalDeltas())
    assert [text.index(name + "=") for name in DELTAS] == sorted(text.index(n + "=") for n in DELTAS)
    with pytest.raises(TypeError):
        arraw.LocalDeltas(exposure=True)
    with pytest.raises(TypeError):
        arraw.LocalDeltas(tint=1)


def test_local_adjustment_defaults():
    adjustment = arraw.LocalAdjustment()
    assert adjustment.id == 0
    assert adjustment.name == ""
    assert adjustment.enabled is True
    assert adjustment.opacity == 1.0
    assert adjustment.invert is False
    assert adjustment.shape == arraw.LinearMask()
    assert adjustment.deltas == arraw.LocalDeltas()
    shaped = arraw.LocalAdjustment(shape=arraw.RadialMask(), deltas=arraw.LocalDeltas(exposure=1))
    assert isinstance(shaped.shape, arraw.RadialMask)
    assert shaped.replace(opacity=0.5).opacity == 0.5
    assert hash(shaped) == hash(arraw.LocalAdjustment(shape=arraw.RadialMask(),
                                                      deltas=arraw.LocalDeltas(exposure=1)))


def test_ids_are_ints_not_bools_not_negative():
    assert arraw.LocalAdjustment(id=7).id == 7
    for bad in [True, -1, 1.5, "1", 2**32]:
        with pytest.raises((TypeError, ValueError)):
            arraw.LocalAdjustment(id=bad)


def test_the_state_carries_the_list_as_a_tuple_and_keeps_its_counter_above_the_ids():
    state = arraw.DevelopState()
    assert state.local_adjustments == ()
    assert state.next_local_adjustment_id == 1
    built = arraw.DevelopState(local_adjustments=[arraw.LocalAdjustment(id=5),
                                                  arraw.LocalAdjustment(id=2)])
    assert isinstance(built.local_adjustments, tuple)
    assert built.next_local_adjustment_id == 6
    assert built.replace(next_local_adjustment_id=3).next_local_adjustment_id == 6
    assert hash(built) == hash(built.replace())
    assert built != arraw.DevelopState()


# ---- adding ----------------------------------------------------------------------------------


def test_a_photograph_starts_without_masks(photo):
    assert photo.local_adjustments == ()
    assert photo.state.local_adjustments == ()


def test_add_linear_mask_appends_and_returns_a_new_photograph(photo):
    edited = photo.add_linear_mask((0.2, 0.1), (0.4, 0.8), exposure=0.5, name="Sky")
    assert photo.local_adjustments == ()
    (sky,) = edited.local_adjustments
    assert sky.id == 1
    assert sky.name == "Sky"
    assert sky.enabled and not sky.invert and sky.opacity == 1.0
    assert sky.shape == arraw.LinearMask((0.2, 0.1), (0.4, 0.8))
    assert sky.deltas == arraw.LocalDeltas(exposure=0.5)
    assert edited.state.settings == photo.state.settings
    assert edited.path == photo.path and edited.marks == photo.marks


def test_add_radial_mask_takes_its_geometry_and_the_common_keywords(photo):
    edited = photo.add_radial_mask(
        (0.4, 0.6), 0.3, 0.15, 30, 0.25, name="Face", opacity=0.8, invert=True, enabled=False,
        dehaze=-40, relative_temperature=20,
    )
    (face,) = edited.local_adjustments
    assert face.shape == arraw.RadialMask((0.4, 0.6), 0.3, 0.15, 30, 0.25)
    assert (face.name, face.invert, face.enabled) == ("Face", True, False)
    assert face.opacity == pytest.approx(0.8)
    assert face.deltas == arraw.LocalDeltas(dehaze=-40, relative_temperature=20)
    # angle and feather default to 0 and 0.5.
    plain = photo.add_radial_mask((0.5, 0.5), 0.2, 0.2).local_adjustments[0].shape
    assert (plain.angle, plain.feather) == (0.0, 0.5)


def test_new_masks_come_last_with_rising_ids(photo):
    edited = (
        photo.add_linear_mask((0.5, 0.1), (0.5, 0.5), exposure=1)
        .add_radial_mask((0.5, 0.5), 0.2, 0.2, contrast=10)
        .add_linear_mask((0.1, 0.5), (0.5, 0.5), shadows=5)
    )
    assert [a.id for a in edited.local_adjustments] == [1, 2, 3]
    assert [type(a.shape).__name__ for a in edited.local_adjustments] == [
        "LinearMask", "RadialMask", "LinearMask"]
    assert edited.local_adjustments[-1].deltas.shadows == 5.0
    assert edited.state.next_local_adjustment_id == 4


def test_numbers_out_of_range_are_clamped(photo):
    edited = photo.add_radial_mask((9.0, -9.0), 50, 0.2, 400, 3, opacity=7, exposure=40,
                                   contrast=-900)
    (mask,) = edited.local_adjustments
    assert mask.shape.centre == (3.0, -2.0)
    assert mask.shape.radius_x == 4.0
    assert -180.0 <= mask.shape.angle < 180.0
    assert mask.shape.feather == 1.0
    assert mask.opacity == 1.0
    assert mask.deltas.exposure == 4.0
    assert mask.deltas.contrast == -100.0


def test_every_delta_keyword_is_accepted(photo):
    edited = photo.add_linear_mask((0.5, 0.1), (0.5, 0.9), **{name: 1.0 for name in DELTAS})
    assert all(getattr(edited.local_adjustments[0].deltas, name) == 1.0 for name in DELTAS)


def test_bad_input_is_refused_and_leaves_the_photograph_alone(photo):
    with pytest.raises(TypeError, match="unknown local adjustment keyword 'brightness'"):
        photo.add_linear_mask((0.1, 0.1), (0.9, 0.9), brightness=1)
    with pytest.raises(TypeError):
        photo.add_linear_mask((0.1, 0.1), (0.9, 0.9), exposure=True)
    with pytest.raises(TypeError):
        photo.add_linear_mask((0.1, 0.1), (0.9, 0.9), exposure="1")
    with pytest.raises(ValueError):
        photo.add_linear_mask((0.1, 0.1), (0.1, 0.1))  # degenerate
    with pytest.raises(ValueError):
        photo.add_linear_mask((0.1, math.nan), (0.9, 0.9))
    with pytest.raises(ValueError):
        photo.add_radial_mask((0.5, 0.5), 0.0, 0.2)
    # A name XML 1.0 cannot carry would make the sidecar unreadable.
    for bad in ("x\ufffey", "x\uffffy", "x\ty", "x\x7fy"):
        with pytest.raises(ValueError):
            photo.add_radial_mask((0.5, 0.5), 0.2, 0.2, name=bad, exposure=1.0)
    with pytest.raises(ValueError):
        photo.add_radial_mask((0.5, 0.5), 0.2, 0.2, exposure=math.inf)
    with pytest.raises(ValueError):
        photo.add_linear_mask((0.1, 0.1), (0.9, 0.9), opacity=math.nan)
    with pytest.raises(TypeError):
        photo.add_linear_mask((0.1, 0.1), (0.9, 0.9), opacity=True)
    with pytest.raises(TypeError):
        photo.add_linear_mask((0.1, 0.1), (0.9, 0.9), invert=1)
    assert photo.local_adjustments == ()


def test_sixteen_masks_fit_and_a_seventeenth_does_not(photo):
    full = photo
    for index in range(16):
        full = full.add_radial_mask((0.5, 0.5), 0.2, 0.2, exposure=0.1 * index)
    assert len(full.local_adjustments) == 16
    with pytest.raises(ValueError, match="16|sixteen|full|at most"):
        full.add_linear_mask((0.1, 0.1), (0.9, 0.9))
    # Removing one makes room, and the new one gets a fresh id.
    room = full.without_local_adjustment(3).add_linear_mask((0.1, 0.1), (0.9, 0.9))
    assert room.local_adjustments[-1].id == 17


# ---- changing and removing ---------------------------------------------------------------------


@pytest.fixture
def two(photo):
    return photo.add_linear_mask((0.2, 0.1), (0.4, 0.8), exposure=0.5).add_radial_mask(
        (0.5, 0.5), 0.3, 0.2, dehaze=-40)


def test_with_local_adjustment_changes_fields_and_deltas(two):
    first, second = two.local_adjustments
    edited = two.with_local_adjustment(
        first.id, name="Sky", enabled=False, invert=True, opacity=0.5, exposure=2.0, saturation=-30)
    changed, untouched = edited.local_adjustments
    assert untouched == second
    assert (changed.id, changed.name, changed.enabled, changed.invert, changed.opacity) == (
        first.id, "Sky", False, True, 0.5)
    assert changed.deltas == arraw.LocalDeltas(exposure=2.0, saturation=-30)
    assert changed.shape == first.shape
    # The original is untouched.
    assert two.local_adjustments[0] == first


def test_with_local_adjustment_replaces_a_shape_of_the_same_kind(two):
    first, second = two.local_adjustments
    moved = two.with_local_adjustment(first.id, shape=arraw.LinearMask((0.0, 0.0), (1.0, 1.0)))
    assert moved.local_adjustments[0].shape == arraw.LinearMask((0.0, 0.0), (1.0, 1.0))
    turned = two.with_local_adjustment(second.id, shape=second.shape.replace(angle=45))
    assert turned.local_adjustments[1].shape.angle == 45.0
    with pytest.raises(ValueError, match="cannot take"):
        two.with_local_adjustment(first.id, shape=arraw.RadialMask())
    with pytest.raises(TypeError):
        two.with_local_adjustment(first.id, shape="round")


def test_with_local_adjustment_clamps_and_refuses(two):
    first = two.local_adjustments[0]
    assert two.with_local_adjustment(first.id, exposure=99).local_adjustments[0].deltas.exposure == 4.0
    assert two.with_local_adjustment(first.id, opacity=-3).local_adjustments[0].opacity == 0.0
    with pytest.raises(ValueError):
        two.with_local_adjustment(99, exposure=1)
    with pytest.raises(ValueError):
        two.with_local_adjustment(first.id, exposure=math.nan)
    with pytest.raises(TypeError):
        two.with_local_adjustment(first.id, brightness=1)
    with pytest.raises(TypeError):
        two.with_local_adjustment(first.id, enabled=1)
    with pytest.raises(TypeError):
        two.with_local_adjustment(first.id, name=3)
    with pytest.raises(TypeError):
        two.with_local_adjustment(True, exposure=1)


def test_a_failing_change_changes_nothing(two):
    before = two.local_adjustments
    with pytest.raises(ValueError):
        two.with_local_adjustment(1, exposure=1, shape=arraw.RadialMask())
    assert two.local_adjustments == before


def test_without_local_adjustment_removes_and_never_reuses_an_id(two):
    first, second = two.local_adjustments
    only = two.without_local_adjustment(first.id)
    assert only.local_adjustments == (second,)
    assert only.state.next_local_adjustment_id == 3
    again = only.add_linear_mask((0.1, 0.1), (0.9, 0.9))
    assert [a.id for a in again.local_adjustments] == [second.id, 3]
    assert again.without_local_adjustment(second.id).without_local_adjustment(3).local_adjustments == ()
    with pytest.raises(ValueError):
        only.without_local_adjustment(first.id)
    with pytest.raises(TypeError):
        only.without_local_adjustment("1")


def test_other_photograph_edits_keep_the_masks(two):
    assert two.edited(exposure=0.4).local_adjustments == two.local_adjustments
    assert two.with_(exposure=0.4).local_adjustments == two.local_adjustments
    assert two.with_(rating=3).local_adjustments == two.local_adjustments


def test_describe_change_is_about_settings(two):
    first = two.local_adjustments[0]
    edited = two.with_local_adjustment(first.id, exposure=2)
    assert arraw.describe_change(two.state, edited.state).keys == []


# ---- persistence -------------------------------------------------------------------------------


def test_masks_survive_the_sidecar(tmp_path, dng):
    work = tmp_path / "shot.dng"
    shutil.copy(dng, work)
    masked = (
        arraw.open(work)
        .add_linear_mask((0.2, 0.1), (0.35, 0.8), name='Sky "left" & <more>', exposure=0.5,
                         dehaze=20, opacity=0.35)
        .add_radial_mask((0.4, 0.6), 0.3, 0.15, 30, 0.25, invert=True, relative_temperature=-42.5,
                         vibrance=7)
        .without_local_adjustment(1)
        .add_radial_mask((0.7, 0.3), 0.1, 0.1, enabled=False, texture=10)
    )
    arraw.write_sidecar(masked)
    again = arraw.open(work)
    assert again.local_adjustments == masked.local_adjustments
    assert again.state.next_local_adjustment_id == masked.state.next_local_adjustment_id == 4
    assert [a.id for a in again.local_adjustments] == [2, 3]


def test_state_json_is_not_asked_of_python_yet_but_settings_json_ignores_masks(two):
    # The settings JSON carries the global settings only; the masks are the state's.
    assert "localAdjustments" not in two.state.settings.to_json()


# ---- rendering ---------------------------------------------------------------------------------


def test_masks_change_the_render_where_they_have_weight(photo):
    plain = rendered(photo)
    # Exposure on the left, fading out towards the middle: weight 1 before u = 0.2, 0 after 0.6.
    masked = rendered(photo.add_linear_mask((0.2, 0.5), (0.6, 0.5), exposure=1.0))
    assert plain.shape == masked.shape
    width = plain.shape[1]
    assert np.all(masked[:, 1 : width // 8, :3] > plain[:, 1 : width // 8, :3])
    assert np.array_equal(masked[:, -width // 3 :], plain[:, -width // 3 :])


def test_masks_that_do_nothing_render_as_none(photo):
    plain = rendered(photo)
    inert = (
        photo.add_linear_mask((0.2, 0.5), (0.6, 0.5), exposure=1.0, enabled=False)
        .add_radial_mask((0.5, 0.5), 0.2, 0.2, exposure=1.0, opacity=0.0)
        .add_radial_mask((0.5, 0.5), 0.2, 0.2)
    )
    assert np.array_equal(rendered(inert), plain)


def test_masks_that_cancel_render_as_none(photo):
    plain = rendered(photo)
    cancelled = photo.add_radial_mask((0.5, 0.5), 0.3, 0.2, exposure=1.0, dehaze=30).add_radial_mask(
        (0.5, 0.5), 0.3, 0.2, exposure=-1.0, dehaze=-30)
    assert np.array_equal(rendered(cancelled), plain)


def test_a_full_weight_mask_equals_the_global_setting(photo):
    everywhere = photo.add_linear_mask((0.5, 2.0), (0.5, 2.5), exposure=0.7, saturation=30)
    global_ = photo.edited(exposure=0.7, saturation=30)
    a = rendered(everywhere)
    b = rendered(global_)
    assert not np.array_equal(a, rendered(photo))
    assert np.allclose(a, b, rtol=1e-5, atol=1e-6)


def test_masks_follow_a_resized_and_a_cropped_render(photo):
    masked = photo.add_radial_mask((0.5, 0.5), 0.3, 0.3, exposure=1.0)
    assert rendered(masked, size=16).shape == rendered(photo, size=16).shape
    cropped = masked.edited(crop_rectangle=arraw.UprightCropRect(0.25, 0.25, 0.75, 0.75))
    assert rendered(cropped).shape == rendered(photo.edited(
        crop_rectangle=arraw.UprightCropRect(0.25, 0.25, 0.75, 0.75))).shape


def test_sample_reads_the_masks(photo):
    masked = photo.add_linear_mask((0.5, 0.1), (0.5, 0.9), exposure=1.5)
    image = photo.load()
    a = arraw.sample(image, arraw.Tap.CURVE_INPUT, photo.state).pixels
    b = arraw.sample(image, arraw.Tap.CURVE_INPUT, masked.state).pixels
    assert not np.array_equal(a, b)


def test_python_needs_no_graphics_for_the_cpu(dng, tmp_path):
    # No display and no Qt platform: a build that needed graphics to render masks would fail.
    script = (
        "import arraw, sys\n"
        "p = arraw.open(sys.argv[1], sidecar=False)\n"
        "p = p.add_linear_mask((0.2, 0.5), (0.6, 0.5), exposure=1.0, relative_tint=30)\n"
        "p = p.add_radial_mask((0.5, 0.5), 0.3, 0.2, 20, dehaze=-30, texture=20)\n"
        "image = arraw.develop(p)\n"
        "print(image.size.width, image.size.height)\n"
    )
    env = {k: v for k, v in os.environ.items()
           if k not in {"DISPLAY", "WAYLAND_DISPLAY", "QT_QPA_PLATFORM"}}
    result = subprocess.run([sys.executable, "-c", script, str(dng)], env=env, capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert result.stdout.split() == ["32", "24"]


# ---- the command line ----------------------------------------------------------------------------

MASKED = {
    "tone": dict(linear=dict(exposure=0.8, contrast=25, shadows=40, highlights=-30, blacks=-20,
                             whites=15),
                 radial=dict(exposure=-0.4, highlights=20)),
    "colour": dict(linear=dict(relative_temperature=60, relative_tint=-30),
                   radial=dict(saturation=35, vibrance=40, invert=True, opacity=0.8)),
    "presence": dict(linear=dict(texture=40, clarity=50, dehaze=60),
                     radial=dict(dehaze=-70, clarity=-20)),
    "everything": dict(linear={name: 20.0 if name != "exposure" else 0.6 for name in DELTAS},
                       radial={**{name: -15.0 for name in DELTAS}, "exposure": -0.3,
                               "invert": True, "opacity": 0.6}),
}


def masked_photo(path, case):
    spec = MASKED[case]
    return (
        arraw.open(path, sidecar=False)
        .add_linear_mask((0.2, 0.1), (0.7, 0.8), **spec["linear"])
        .add_radial_mask((0.4, 0.6), 0.3, 0.15, 30, 0.25, **spec["radial"])
    )


@pytest.mark.parametrize("resize", [None, "20"])
@pytest.mark.parametrize("case", MASKED)
@pytest.mark.parametrize("fixture", ["dng", "bayer_dng"])
def test_python_matches_cli_export_of_a_sidecar_with_masks(request, cli, tmp_path, fixture, case,
                                                           resize):
    source = request.getfixturevalue(fixture)
    work = tmp_path / source.name
    shutil.copy(source, work)
    photo = masked_photo(work, case)
    arraw.write_sidecar(photo)

    # What Python renders is the sidecar's state: read back, not the object that wrote it.
    reopened = arraw.open(work)
    assert reopened.local_adjustments == photo.local_adjustments
    size = None if resize is None else int(resize)
    py_out = tmp_path / "py.tif"
    arraw.save(arraw.develop(reopened, size=size), py_out, bit_depth=16)

    outdir = tmp_path / "cli"
    outdir.mkdir()
    flags = ["--resize", resize] if resize else []
    result = subprocess.run(
        [str(cli), "export", "--quiet", "--device", "cpu", "--format", "tiff", "--bit-depth", "16",
         "-o", str(outdir), *flags, str(work)],
        capture_output=True, text=True, timeout=120,
    )
    assert result.returncode == 0, result.stderr
    (cli_out,) = list(outdir.iterdir())

    py_px = arraw.load(py_out).pixels.astype(np.int64)
    cli_px = arraw.load(cli_out).pixels.astype(np.int64)
    assert py_px.shape == cli_px.shape
    # The same CPU pipeline on the same floats: bit-exact, and not the unmasked picture.
    assert np.array_equal(py_px, cli_px)
    plain = arraw.develop(arraw.open(work, sidecar=False), size=size).pixels.astype(np.int64)
    assert np.abs(py_px - plain).max() > 100


def test_the_cli_info_lists_the_masks_python_wrote(cli, tmp_path, dng):
    work = tmp_path / "shot.dng"
    shutil.copy(dng, work)
    photo = arraw.open(work, sidecar=False).add_linear_mask(
        (0.2, 0.1), (0.7, 0.8), name="Sky", exposure=0.5).add_radial_mask(
        (0.4, 0.6), 0.3, 0.15, dehaze=-30)
    arraw.write_sidecar(photo)
    result = subprocess.run([str(cli), "info", str(work)], capture_output=True, text=True,
                            timeout=60)
    assert result.returncode == 0, result.stderr
    assert "local adjustments" in result.stdout
    assert "Sky (linear)" in result.stdout
    assert "(radial)" in result.stdout
