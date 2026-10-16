"""Negative settings (ADR 049): the demosaic, beside the develop state."""

import shutil
import subprocess

import numpy as np
import pytest

import arraw

TOKENS = ["linear", "vng", "ppg", "ahd", "dcb", "dht", "aahd"]


@pytest.fixture
def detail(bayer_detail_dng, tmp_path):
    """A copy of the detail fixture, so that a sidecar can sit beside it."""
    path = tmp_path / "detail.dng"
    shutil.copy(bayer_detail_dng, path)
    return path


def vng():
    return arraw.NegativeSettings(demosaic=arraw.Demosaic.VNG)


# ---- the types -----------------------------------------------------------------------------


def test_demosaic_members_print_as_their_tokens():
    assert [str(member) for member in arraw.Demosaic] == TOKENS
    assert arraw.Demosaic.AHD.name == "AHD"


def test_negative_settings_default_is_ahd():
    assert arraw.NegativeSettings().demosaic == arraw.Demosaic.AHD


def test_negative_settings_are_frozen_values():
    negative = vng()
    assert negative == vng()
    assert negative != arraw.NegativeSettings()
    assert hash(negative) == hash(vng())
    assert negative.replace(demosaic=arraw.Demosaic.PPG).demosaic == arraw.Demosaic.PPG
    with pytest.raises(AttributeError):
        negative.demosaic = arraw.Demosaic.AHD
    with pytest.raises(TypeError):
        arraw.NegativeSettings(arraw.Demosaic.VNG)


# ---- Photo ---------------------------------------------------------------------------------


def test_photo_negative_defaults_and_with(dng):
    photo = arraw.open(dng, sidecar=False)
    assert photo.negative == arraw.NegativeSettings()
    other = photo.with_(negative=vng())
    assert other.negative == vng()
    assert other.state == photo.state
    assert photo.negative == arraw.NegativeSettings()  # the original is untouched


def test_with_state_keeps_the_negative_and_the_negative_keeps_the_state(dng):
    photo = arraw.open(dng, sidecar=False).with_(negative=vng())
    assert photo.with_(exposure=0.5).negative == vng()
    assert photo.with_(rating=3).negative == vng()
    assert photo.with_(state=arraw.DevelopState()).negative == vng()
    edited = photo.with_(exposure=0.5).with_(negative=arraw.NegativeSettings())
    assert edited.state.settings.tone.exposure == pytest.approx(0.5)


def test_every_derived_photo_keeps_the_negative(dng):
    photo = arraw.open(dng, sidecar=False).with_(negative=vng())
    assert photo.edited(exposure=0.5).negative == vng()
    mask = photo.add_radial_mask((0.5, 0.5), 0.3, 0.2, exposure=-0.4)
    assert mask.negative == vng()
    assert mask.without_local_adjustment(mask.state.local_adjustments[0].id).negative == vng()


def test_edited_takes_develop_keys_only(dng):
    photo = arraw.open(dng, sidecar=False)
    with pytest.raises(TypeError):
        photo.edited(demosaic=arraw.Demosaic.VNG)
    with pytest.raises(TypeError):
        photo.edited(negative=vng())


def test_the_negative_is_in_equality_and_repr(dng):
    photo = arraw.open(dng, sidecar=False)
    assert photo != photo.with_(negative=vng())
    assert "negative=NegativeSettings(demosaic=" in repr(photo)


def test_negative_survives_a_sidecar(detail):
    photo = arraw.open(detail, sidecar=False).with_(negative=vng())
    arraw.write_sidecar(photo)
    assert arraw.open(detail).negative == vng()
    assert arraw.read_sidecar(detail).negative == vng()
    assert arraw.open(detail, sidecar=False).negative == arraw.NegativeSettings()


# ---- decoding ------------------------------------------------------------------------------


def test_load_demosaic_changes_a_full_decode_only(detail):
    ahd = arraw.load(detail).pixels
    assert np.array_equal(arraw.load(detail, demosaic=arraw.Demosaic.AHD).pixels, ahd)
    assert not np.array_equal(arraw.load(detail, demosaic=arraw.Demosaic.VNG).pixels, ahd)
    half = arraw.load(detail, half_size=True).pixels
    assert np.array_equal(arraw.load(detail, half_size=True, demosaic=arraw.Demosaic.VNG).pixels, half)


def test_photo_load_and_develop_decode_with_the_photos_negative(detail):
    photo = arraw.open(detail, sidecar=False)
    other = photo.with_(negative=vng())
    assert np.array_equal(photo.load().pixels, arraw.load(detail).pixels)
    assert np.array_equal(
        other.load().pixels, arraw.load(detail, demosaic=arraw.Demosaic.VNG).pixels
    )
    assert not np.array_equal(arraw.develop(photo).pixels, arraw.develop(other).pixels)


def test_develop_with_a_state_still_uses_the_photos_negative(detail):
    other = arraw.open(detail, sidecar=False).with_(negative=vng())
    plain = arraw.develop(other)
    again = arraw.develop(other, other.state)
    assert np.array_equal(plain.pixels, again.pixels)


def test_python_matches_cli_export_with_a_demosaic(cli, detail, tmp_path):
    photo = arraw.open(detail, sidecar=False).with_(negative=vng())
    py_out = tmp_path / "py.tif"
    arraw.save(arraw.develop(photo), py_out, bit_depth=16)

    outdir = tmp_path / "cli"
    outdir.mkdir()
    result = subprocess.run(
        [str(cli), "export", "--quiet", "--device", "cpu", "--format", "tiff", "--bit-depth", "16",
         "--no-sidecar", "--demosaic", "vng", "-o", str(outdir), str(detail)],
        capture_output=True, text=True, timeout=120,
    )
    assert result.returncode == 0, result.stderr
    (cli_out,) = list(outdir.iterdir())
    assert np.array_equal(
        arraw.load(py_out).pixels.astype(np.int64), arraw.load(cli_out).pixels.astype(np.int64)
    )


# ---- EditSession ---------------------------------------------------------------------------


@pytest.fixture
def session(dng):
    return arraw.EditSession(arraw.open(dng, sidecar=False))


def test_set_negative_makes_one_named_step(session):
    session.set_negative(vng(), arraw.EditOrigin.PRESET, "Warm")
    assert session.photo.negative == vng()
    assert len(session.history) == 2
    step = session.history[-1]
    assert step.negative == vng()
    assert step.origin == arraw.EditOrigin.PRESET and step.detail == "Warm"
    assert session.history[0].negative == arraw.NegativeSettings()
    assert "negative=" in repr(step)


def test_an_equal_negative_leaves_no_step(session):
    session.set_negative(arraw.NegativeSettings())
    assert len(session.history) == 1


def test_undo_and_redo_restore_both_parts(session):
    session.set_state(
        session.photo.with_(exposure=0.4).state, arraw.EditOrigin.PASTE, negative=vng()
    )
    assert len(session.history) == 2
    assert session.photo.negative == vng()
    assert session.photo.state.settings.tone.exposure == pytest.approx(0.4)
    session.undo()
    assert session.photo.negative == arraw.NegativeSettings()
    assert session.photo.state.settings.tone.exposure == 0
    session.redo()
    assert session.photo.negative == vng()
    assert session.photo.state.settings.tone.exposure == pytest.approx(0.4)


def test_set_state_without_a_negative_keeps_it(session):
    session.set_negative(vng())
    session.set_state(session.photo.with_(exposure=0.4).state)
    assert session.photo.negative == vng()
    assert session.history[-1].negative == vng()


def test_an_edit_cannot_change_the_negative(session):
    session.set_negative(vng())
    with session.edit():
        session.update(session.photo.with_(exposure=0.3).state)
    assert session.photo.negative == vng()
    assert session.history[-1].negative == vng()


def test_set_negative_commits_an_open_edit_first(session):
    session.begin()
    session.update(session.photo.with_(exposure=0.3).state)
    session.set_negative(vng())
    assert not session.editing
    assert [step.negative for step in session.history] == [
        arraw.NegativeSettings(),
        arraw.NegativeSettings(),
        vng(),
    ]


def test_a_negative_only_change_is_unsaved_and_discard_restores_it(detail):
    session = arraw.EditSession(arraw.open(detail))
    assert not session.has_unsaved_changes
    session.set_negative(vng())
    assert session.has_unsaved_changes
    session.discard_changes()
    assert session.photo.negative == arraw.NegativeSettings()
    assert not session.has_unsaved_changes
    session.set_negative(vng())
    session.save()
    assert not session.has_unsaved_changes
    assert arraw.open(detail).negative == vng()


def test_describe_change_names_the_negative(session):
    session.set_negative(vng())
    before, after = session.history
    change = arraw.describe_change(before, after)
    assert change.negative == ["demosaic"]
    assert change.keys == []
    assert arraw.describe_change(after, after).negative == []
    assert arraw.describe_change(before.state, after.state).negative == []
    session.set_state(session.photo.with_(exposure=0.4).state)
    assert arraw.describe_change(after, session.history[-1]).negative == []
    assert arraw.describe_change(after, session.history[-1]).keys == ["exposure"]
