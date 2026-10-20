"""Lens corrections (ADR 050): settings, database, matching, resolution, render and history."""

import gc
import logging
import shutil
import subprocess

import numpy as np
import pytest

import arraw

KIT = ("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM")


@pytest.fixture
def raw(lens_dng, tmp_path):
    """A copy of the lens fixture, so that a sidecar can sit beside it."""
    path = tmp_path / "frame.dng"
    shutil.copy(lens_dng, path)
    return path


def recorded(origin=arraw.LensOrigin.MATCHED):
    return arraw.RecordedLens(
        maker=KIT[0], model=KIT[1], crop_factor=1.6, origin=origin, database="stamp"
    )


# ---- the settings (no lensfun needed) -----------------------------------------------------


def test_corrections_are_off_by_default_and_nothing_is_recorded(lens_dng):
    assert arraw.NegativeSettings().lens == arraw.LensSettings()
    settings = arraw.LensSettings()
    assert (settings.distortion, settings.vignetting_amount, settings.lateral_ca) == (False, 0, False)
    assert settings.lens is None
    assert not settings.any
    photo = arraw.open(lens_dng, sidecar=False)
    assert photo.lens == arraw.LensSettings()
    assert photo.negative.lens == photo.lens


def test_lens_origins_print_as_their_tokens():
    assert [str(member) for member in arraw.LensOrigin] == ["matched", "chosen"]


def test_lens_settings_are_frozen_values():
    on = arraw.LensSettings(distortion=True, lens=recorded())
    assert on.any
    assert on == arraw.LensSettings(distortion=True, lens=recorded())
    assert hash(on) == hash(arraw.LensSettings(distortion=True, lens=recorded()))
    assert on != arraw.LensSettings(distortion=True)
    assert on.replace(distortion=False).lens == recorded()
    assert recorded().replace(origin=arraw.LensOrigin.CHOSEN).origin == arraw.LensOrigin.CHOSEN
    with pytest.raises(AttributeError):
        on.distortion = False
    with pytest.raises(TypeError):
        arraw.LensSettings(True)


def test_the_lens_is_part_of_the_negative(lens_dng):
    photo = arraw.open(lens_dng, sidecar=False)
    negative = arraw.NegativeSettings(lens=arraw.LensSettings(vignetting_amount=100))
    assert negative.demosaic == arraw.Demosaic.AHD
    assert photo.with_(negative=negative).lens.vignetting_amount == 100
    assert negative != arraw.NegativeSettings()
    assert hash(negative) != hash(arraw.NegativeSettings())


def test_the_vignetting_amount_is_a_number_from_0_to_150(lens_dng):
    photo = arraw.open(lens_dng, sidecar=False)
    assert arraw.LensSettings(vignetting_amount=60).vignetting_amount == 60
    assert photo.with_lens_corrections(vignetting_amount=37.5).lens.vignetting_amount == 37.5
    assert photo.with_lens_corrections(vignetting_amount=150).lens.vignetting_amount == 150
    # A bool is refused as for every number, and the old keyword is gone.
    with pytest.raises(TypeError):
        arraw.LensSettings(vignetting_amount=True)
    with pytest.raises(TypeError):
        photo.with_lens_corrections(vignetting_amount=True)
    with pytest.raises(TypeError):
        photo.with_lens_corrections(vignetting=True)
    with pytest.raises(TypeError):
        arraw.LensSettings(vignetting=True)
    for bad in (200, -1, float("nan")):
        with pytest.raises(ValueError):
            photo.with_lens_corrections(vignetting_amount=bad)
    # None keeps the amount.
    kept = photo.with_lens_corrections(vignetting_amount=60).with_lens_corrections(distortion=True)
    assert kept.lens.vignetting_amount == 60


def test_the_session_refuses_a_bool_or_a_bad_amount(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database)
    with pytest.raises(TypeError):
        session.set_lens_corrections(vignetting_amount=True)
    with pytest.raises(ValueError):
        session.set_lens_corrections(vignetting_amount=200)
    session.set_lens_corrections(vignetting_amount=60)
    assert session.photo.lens.vignetting_amount == 60
    assert session.photo.lens.lens is not None


def test_a_dragged_amount_is_one_step(lens_dng):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False))
    steps = len(session.history)
    negative = session.photo.negative
    session.begin()
    for amount in (30, 60):
        session.update_negative(
            negative.replace(lens=arraw.LensSettings(vignetting_amount=amount))
        )
    assert session.photo.lens.vignetting_amount == 60
    with pytest.raises(ValueError):
        session.update_negative(
            negative.replace(lens=arraw.LensSettings(vignetting_amount=200))
        )
    assert session.photo.lens.vignetting_amount == 60
    session.commit()
    assert len(session.history) == steps + 1
    session.undo()
    assert session.photo.lens.vignetting_amount == 0
    with pytest.raises(RuntimeError):
        session.update_negative(negative)


def test_a_recorded_lens_must_name_a_lens(lens_dng):
    photo = arraw.open(lens_dng, sidecar=False)
    bad = arraw.NegativeSettings(lens=arraw.LensSettings(lens=arraw.RecordedLens()))
    with pytest.raises(ValueError):
        photo.with_(negative=bad)


def test_switches_set_on_a_raw_and_skipped_on_a_png(lens_dng, png):
    raw = arraw.open(lens_dng, sidecar=False)
    on = raw.with_lens_corrections(distortion=True, lateral_ca=True)
    assert (on.lens.distortion, on.lens.vignetting_amount, on.lens.lateral_ca) == (True, 0, True)
    # None keeps a switch.
    assert on.with_lens_corrections(vignetting_amount=100).lens.distortion
    assert not on.with_lens_corrections(distortion=False).lens.distortion
    card = arraw.open(png, sidecar=False)
    assert card.with_lens_corrections(distortion=True) == card


def test_a_png_refuses_a_bad_vignetting_amount_as_a_raw_does(png):
    card = arraw.open(png, sidecar=False)
    assert card.with_lens_corrections(vignetting_amount=60) == card
    with pytest.raises(TypeError):
        card.with_lens_corrections(vignetting_amount=True)
    with pytest.raises(TypeError):
        card.with_lens_corrections(vignetting_amount="x")
    for bad in (200, -1, float("nan")):
        with pytest.raises(ValueError):
            card.with_lens_corrections(vignetting_amount=bad)


def test_the_vignetting_amount_scales_the_gain_in_stops(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False)
    full = photo.with_lens_corrections(vignetting_amount=100).lens_correction(lens_database)
    half = photo.with_lens_corrections(vignetting_amount=50).lens_correction(lens_database)
    assert len(full.vignetting) > 1 and len(half.vignetting) == len(full.vignetting)
    assert np.allclose(half.vignetting, np.sqrt(full.vignetting), rtol=1e-6)


def test_the_stage_and_the_metadata_facts(lens_dng, png):
    assert arraw.Stage.LENS.name == "LENS"
    facts = arraw.read_metadata(lens_dng).lens
    assert facts.camera_maker == "Canon"
    assert facts.camera_model == "Canon EOS 80D"
    assert facts.lens_names == ["EF-S18-135mm f/3.5-5.6 IS USM"]
    assert facts.focal_length == 50.0
    assert facts.aperture == pytest.approx(5.6)
    assert facts.focal_length_35 == 80.0
    assert facts.focus_distance == pytest.approx(3.0)
    assert facts.min_focal is None
    assert arraw.read_metadata(png).lens == arraw.LensFacts()


def test_render_is_unchanged_with_corrections_off(lens_dng):
    photo = arraw.open(lens_dng, sidecar=False)
    plain = arraw.develop(arraw.load(lens_dng), photo.state)
    assert np.array_equal(arraw.develop(photo).pixels, plain.pixels)
    assert not photo.lens_correction().active


def test_a_directory_with_no_database_says_why(tmp_path):
    database = arraw.LensDatabase.from_directory(tmp_path)
    assert not database.loaded
    expected = (
        arraw.LensProblem.LENS_DATABASE_MISSING
        if arraw.lensfun_available()
        else arraw.LensProblem.LENSFUN_UNAVAILABLE
    )
    assert database.problem == expected
    assert database.lens_count == 0
    assert database.find_camera("Canon", "Canon EOS 80D") is None


# ---- the database and matching ------------------------------------------------------------


def test_the_fixture_database_lists_the_cameras_and_lenses(lens_database):
    assert lens_database.loaded and lens_database.problem is None
    assert lens_database.stamp
    camera = lens_database.find_camera("Canon", "Canon EOS 80D")
    assert camera.crop_factor == pytest.approx(1.6)
    assert lens_database.mount_of("Canon", "Canon EOS 80D") == camera.mount
    kit = lens_database.lens(*KIT)
    assert kit.model == KIT[1] and kit.crop_factor > 0
    assert kit in lens_database.lenses("18-135")
    assert lens_database.lens("Canon", "Nothing like it") is None
    found = lens_database.find_lenses("EF-S18-135mm f/3.5-5.6 IS USM", mount=camera.mount)
    assert found[0].model == KIT[1]


def test_the_lens_is_matched_from_the_exif(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False)
    match = photo.lens_match(lens_database)
    assert match.lens == arraw.LensIdentity(*KIT)
    assert match.crop_factor == pytest.approx(1.6)
    assert match.crop_source == arraw.CropSource.CAMERA
    assert match.camera_listed and match.focal_length == 50.0 and match.focal_length_read


def test_a_png_has_no_lens_to_match(png, lens_database):
    assert arraw.open(png, sidecar=False).lens_match(lens_database) is None


# ---- recording ----------------------------------------------------------------------------


def test_a_correction_is_recorded_once_and_kept_when_switched_off(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False)
    # Nothing on: nothing recorded, and the same photograph back.
    assert arraw.record_lens_match(photo, lens_database) == photo

    on = photo.with_lens_corrections(distortion=True)
    done = arraw.record_lens_match(on, lens_database)
    lens = done.lens.lens
    assert (lens.maker, lens.model) == KIT
    assert lens.crop_factor == pytest.approx(1.6)
    assert lens.origin == arraw.LensOrigin.MATCHED
    assert lens.database == lens_database.stamp
    assert done.lens.distortion and done.lens.vignetting_amount == 0
    # Once: again changes nothing; off keeps it.
    assert arraw.record_lens_match(done, lens_database) == done
    assert done.with_lens_corrections(distortion=False).lens.lens == lens


def test_a_chosen_lens_is_recorded_as_chosen(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False)
    chosen = photo.with_chosen_lens("Canon", "Canon EF 50mm f/1.8 STM", lens_database)
    assert chosen.lens.lens.origin == arraw.LensOrigin.CHOSEN
    assert chosen.lens.lens.model == "Canon EF 50mm f/1.8 STM"
    assert chosen.lens.lens.database == lens_database.stamp
    with pytest.raises(ValueError, match="not in the lens database"):
        photo.with_chosen_lens("Canon", "Canon EF 85mm f/1.2L II USM", lens_database)


# ---- resolution and the render ------------------------------------------------------------


def test_the_correction_resolves_its_tables_and_provenance(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False).with_lens_corrections(
        distortion=True, vignetting_amount=100, lateral_ca=True
    )
    lens = photo.lens_correction(lens_database)
    assert lens.active
    assert len(lens.distortion) == len(lens.vignetting) > 1
    assert len(lens.red_scale) == len(lens.blue_scale) == len(lens.distortion)
    assert lens.distortion_from.lens == arraw.LensIdentity(*KIT)
    assert lens.distortion_from.focal_length == 50.0 and lens.distortion_from.focal_length_read
    assert lens.distortion_from.database == lens_database.stamp
    assert lens.fill >= 1.0
    assert not arraw.open(lens_dng, sidecar=False).lens_correction(lens_database).active


def test_each_correction_alone_resolves_only_its_table(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False)
    only = photo.with_lens_corrections(vignetting_amount=100).lens_correction(lens_database)
    assert only.vignetting and not only.distortion and not only.red_scale
    assert only.vignetting_from is not None and only.distortion_from is None


def test_develop_applies_the_photographs_corrections(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False)
    plain = arraw.develop(photo, lens_database=lens_database)
    corrected = photo.with_lens_corrections(distortion=True, vignetting_amount=100, lateral_ca=True)
    result = arraw.develop(corrected, lens_database=lens_database)
    # The silent wrong pixels of a correction nobody applied: the picture is another.
    assert not np.array_equal(result.pixels, plain.pixels)
    # Each correction alone is another picture again.
    only = arraw.develop(photo.with_lens_corrections(distortion=True), lens_database=lens_database)
    assert not np.array_equal(only.pixels, result.pixels)
    assert not np.array_equal(only.pixels, plain.pixels)
    # It is the library's render: the resolved correction through the buffer form.
    lens = corrected.lens_correction(lens_database)
    again = arraw.develop(arraw.load(lens_dng), corrected.state, lens=lens)
    assert np.array_equal(again.pixels, result.pixels)
    # sample and curve_histogram follow.
    assert not np.array_equal(
        arraw.sample(corrected, arraw.Tap.CURVE_INPUT, lens_database=lens_database).pixels,
        arraw.sample(photo, arraw.Tap.CURVE_INPUT, lens_database=lens_database).pixels,
    )
    assert arraw.curve_histogram(corrected, lens_database=lens_database) != arraw.curve_histogram(
        photo, lens_database=lens_database
    )


def test_a_recorded_lens_is_used_over_the_match(lens_dng, lens_database):
    photo = arraw.open(lens_dng, sidecar=False).with_lens_corrections(distortion=True)
    matched = photo.lens_correction(lens_database)
    other = photo.with_chosen_lens("Canon", "Canon EF 50mm f/1.8 STM", lens_database)
    chosen = other.lens_correction(lens_database)
    assert chosen.distortion_from.lens.model == "Canon EF 50mm f/1.8 STM"
    assert chosen != matched
    assert other.lens_match(lens_database).lens.model == "Canon EF 50mm f/1.8 STM"


def test_a_missing_lens_is_skipped_with_a_notice(lens_dng, lens_database, caplog):
    photo = arraw.open(lens_dng, sidecar=False).with_(
        negative=arraw.NegativeSettings(
            lens=arraw.LensSettings(
                distortion=True,
                lens=arraw.RecordedLens(maker="Canon", model="Renamed", crop_factor=1.6),
            )
        )
    )
    with caplog.at_level(logging.INFO, logger="arraw"):
        lens = photo.lens_correction(lens_database)
    assert not lens.active
    assert caplog.records and caplog.records[-1].levelno >= logging.WARNING
    # The render goes ahead without it.
    assert np.array_equal(
        arraw.develop(photo, lens_database=lens_database).pixels,
        arraw.develop(photo.with_lens_corrections(distortion=False)).pixels,
    )


def test_a_photograph_that_names_no_lens_says_so(dng, lens_database, caplog):
    photo = arraw.open(dng, sidecar=False).with_lens_corrections(distortion=True, vignetting_amount=100)
    with caplog.at_level(logging.INFO, logger="arraw"):
        lens = photo.lens_correction(lens_database)
    assert not lens.active
    assert any("names no lens" in record.getMessage() for record in caplog.records)


def test_a_png_ignores_the_switches(png, lens_database, caplog):
    photo = arraw.open(png, sidecar=False)
    with caplog.at_level(logging.INFO, logger="arraw"):
        assert not photo.lens_correction(lens_database).active
    assert not caplog.records


# ---- sessions and history -----------------------------------------------------------------


def test_the_toggle_and_the_record_are_one_step_and_undo_together(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database=lens_database)
    session.set_lens_corrections(distortion=True)
    assert len(session.history) == 2
    assert session.photo.lens.distortion and session.photo.lens.lens is not None
    assert session.history[-1].lens == session.photo.lens
    change = arraw.describe_change(session.history[0], session.history[1])
    assert change.negative == ["lensDistortion", "lens"]
    assert change.lens == ["lensDistortion", "lens"]

    # Switching off is its own step and keeps the record.
    session.set_lens_corrections(distortion=False)
    assert len(session.history) == 3
    assert session.photo.lens.lens is not None
    assert arraw.describe_change(session.history[1], session.history[2]).lens == ["lensDistortion"]

    session.undo()
    assert session.photo.lens.distortion
    session.undo()
    assert session.photo.negative == session.history[0].negative
    assert session.photo.lens.lens is None
    session.redo()
    assert session.photo.lens.lens is not None


def test_choosing_and_resetting_the_lens(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database=lens_database)
    session.set_lens_corrections(vignetting_amount=100)
    session.choose_lens("Canon", "Canon EF 50mm f/1.8 STM", lens_database)
    assert session.photo.lens.lens.origin == arraw.LensOrigin.CHOSEN
    assert session.photo.lens.vignetting_amount == 100
    with pytest.raises(ValueError):
        session.choose_lens("Canon", "Nothing like it", lens_database)
    session.reset_lens()
    assert session.photo.lens.lens.model == KIT[1]
    assert session.photo.lens.lens.origin == arraw.LensOrigin.MATCHED
    assert not session.can_undo or session.history[-1].lens == session.photo.lens


def test_the_lens_survives_the_sidecar(raw, lens_database):
    session = arraw.EditSession(arraw.open(raw), lens_database=lens_database)
    session.set_lens_corrections(distortion=True, lateral_ca=True)
    session.save()
    reopened = arraw.open(raw)
    assert reopened.lens == session.photo.lens
    assert reopened.lens.lens.model == KIT[1]


# ---- the command line ---------------------------------------------------------------------


def export_with_cli(cli, source, outdir, *flags):
    outdir.mkdir()
    result = subprocess.run(
        [str(cli), "export", "--quiet", "--device", "cpu", "--format", "tiff", "--bit-depth", "16",
         "--no-sidecar", "-o", str(outdir), *flags, str(source)],
        capture_output=True, text=True, timeout=120,
    )
    assert result.returncode == 0, result.stderr
    (out,) = list(outdir.iterdir())
    return arraw.load(out).pixels.astype(np.int64)


@pytest.mark.parametrize(
    "flag, switches",
    [
        ("all", {"distortion": True, "vignetting_amount": 100, "lateral_ca": True}),
        ("distortion", {"distortion": True}),
        ("vignetting,ca", {"vignetting_amount": 100, "lateral_ca": True}),
        ("none", {}),
    ],
)
def test_python_matches_cli_export_with_lens_corrections(
    cli, lens_dng, lens_database, lens_database_dir, tmp_path, flag, switches
):
    photo = arraw.open(lens_dng, sidecar=False).with_lens_corrections(**switches)
    py_out = tmp_path / "py.tif"
    arraw.save(arraw.develop(photo, lens_database=lens_database), py_out, bit_depth=16)
    expected = arraw.load(py_out).pixels.astype(np.int64)
    got = export_with_cli(
        cli, lens_dng, tmp_path / "cli", "--lens-corrections", flag,
        "--lens-database", str(lens_database_dir),
    )
    assert np.array_equal(expected, got)


def test_python_matches_cli_export_with_a_chosen_lens(
    cli, lens_dng, lens_database, lens_database_dir, tmp_path
):
    photo = arraw.open(lens_dng, sidecar=False).with_lens_corrections(distortion=True)
    photo = photo.with_chosen_lens("Canon", "Canon EF 50mm f/1.8 STM", lens_database)
    py_out = tmp_path / "py.tif"
    arraw.save(arraw.develop(photo, lens_database=lens_database), py_out, bit_depth=16)
    got = export_with_cli(
        cli, lens_dng, tmp_path / "cli", "--lens-corrections", "distortion",
        "--lens", "Canon|Canon EF 50mm f/1.8 STM", "--lens-database", str(lens_database_dir),
    )
    assert np.array_equal(arraw.load(py_out).pixels.astype(np.int64), got)


# ---- the session records the match ----------------------------------------------------------


def test_the_session_holds_the_database_it_was_given(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database=lens_database)
    assert session.lens_database is lens_database
    with pytest.raises(AttributeError):
        session.lens_database = None


def test_the_database_outlives_its_last_reference(lens_dng, lens_database_dir):
    session = arraw.EditSession(
        arraw.open(lens_dng, sidecar=False),
        lens_database=arraw.LensDatabase.from_directory(lens_database_dir),
    )
    gc.collect()
    session.set_lens_corrections(distortion=True)
    assert session.photo.lens.lens.model == KIT[1]
    assert session.lens_database.loaded


def test_a_session_of_a_png_has_no_database(png):
    assert arraw.EditSession(arraw.open(png, sidecar=False)).lens_database is None


def test_a_raw_session_defaults_to_the_process_database(lens_dng):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False))
    assert session.lens_database is not None


def test_set_negative_records_the_match_in_its_step(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database=lens_database)
    negative = session.photo.negative
    session.set_negative(negative.replace(lens=arraw.LensSettings(lateral_ca=True)))
    assert len(session.history) == 2
    assert session.photo.lens.lens.model == KIT[1]
    session.undo()
    assert session.photo.lens.lens is None


def test_a_dragged_amount_records_the_match_at_the_commit(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database=lens_database)
    negative = session.photo.negative
    session.begin()
    session.update_negative(negative.replace(lens=arraw.LensSettings(vignetting_amount=60)))
    assert session.photo.lens.lens is None  # a move never matches
    session.commit()
    assert len(session.history) == 2
    assert session.photo.lens.lens.model == KIT[1]


def test_a_chosen_lens_defaults_to_the_sessions_database(lens_dng, lens_database):
    session = arraw.EditSession(arraw.open(lens_dng, sidecar=False), lens_database=lens_database)
    session.choose_lens("Canon", "Canon EF 50mm f/1.8 STM")
    assert session.photo.lens.lens.origin == arraw.LensOrigin.CHOSEN
