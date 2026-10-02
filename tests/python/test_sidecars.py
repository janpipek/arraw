import logging
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

import arraw

CRS = "http://ns.adobe.com/camera-raw-settings/1.0/"


@pytest.fixture
def work(tmp_path, dng) -> Path:
    """A copy of the neutral DNG in a directory of its own."""
    target = tmp_path / "shot.dng"
    shutil.copy(dng, target)
    return target


def arraw_records(caplog, level=logging.WARNING):
    return [r for r in caplog.records if r.name == "arraw" and r.levelno >= level]


# ---- ColorLabel and PhotoMarks -------------------------------------------------------------


def test_color_label_members():
    assert [m.name for m in arraw.ColorLabel] == ["RED", "YELLOW", "GREEN", "BLUE", "PURPLE"]


def test_marks_defaults_and_keyword_only():
    marks = arraw.PhotoMarks()
    assert marks.rating == 0
    assert marks.label is None
    with pytest.raises(TypeError):
        arraw.PhotoMarks(3)


def test_marks_are_frozen_values():
    marks = arraw.PhotoMarks(rating=3, label=arraw.ColorLabel.BLUE)
    with pytest.raises(AttributeError):
        marks.rating = 4
    assert marks == arraw.PhotoMarks(rating=3, label=arraw.ColorLabel.BLUE)
    assert marks != arraw.PhotoMarks(rating=3)
    assert hash(marks) == hash(arraw.PhotoMarks(rating=3, label=arraw.ColorLabel.BLUE))
    assert {marks, arraw.PhotoMarks(rating=3, label=arraw.ColorLabel.BLUE)} == {marks}
    assert repr(marks) == "PhotoMarks(rating=3, label=ColorLabel.BLUE)"


def test_marks_replace():
    marks = arraw.PhotoMarks(rating=3, label=arraw.ColorLabel.BLUE)
    assert marks.replace(rating=5).label == arraw.ColorLabel.BLUE
    assert marks.replace(label=None) == arraw.PhotoMarks(rating=3)
    with pytest.raises(TypeError):
        marks.replace(colour=1)
    with pytest.raises(TypeError):
        marks.replace(rating=True)


@pytest.mark.parametrize("rating", [True, False, 3.0, "3", None])
def test_marks_rating_is_strict(rating):
    with pytest.raises(TypeError):
        arraw.PhotoMarks(rating=rating)


@pytest.mark.parametrize("label", ["Red", 0, True])
def test_marks_label_is_strict(label):
    with pytest.raises(TypeError):
        arraw.PhotoMarks(label=label)


# ---- Photo.marks and with_ -----------------------------------------------------------------


def test_photo_has_default_marks(dng):
    assert arraw.open(dng, sidecar=False).marks == arraw.PhotoMarks()


def test_with_rating_and_label(dng):
    photo = arraw.open(dng, sidecar=False)
    marked = photo.with_(rating=4, label=arraw.ColorLabel.GREEN)
    assert marked.marks == arraw.PhotoMarks(rating=4, label=arraw.ColorLabel.GREEN)
    assert marked.state == photo.state
    assert photo.marks == arraw.PhotoMarks()


def test_with_rating_alone_keeps_the_label(dng):
    photo = arraw.open(dng, sidecar=False).with_(rating=2, label=arraw.ColorLabel.RED)
    assert photo.with_(rating=-1).marks == arraw.PhotoMarks(rating=-1, label=arraw.ColorLabel.RED)


def test_with_label_none_clears(dng):
    photo = arraw.open(dng, sidecar=False).with_(rating=2, label=arraw.ColorLabel.RED)
    assert photo.with_(label=None).marks == arraw.PhotoMarks(rating=2)


def test_with_marks_and_settings_together(dng):
    photo = arraw.open(dng, sidecar=False).with_(exposure=0.5, rating=1)
    assert photo.state.settings.tone.exposure == pytest.approx(0.5)
    assert photo.marks.rating == 1


def test_with_state_keeps_marks(dng):
    photo = arraw.open(dng, sidecar=False).with_(rating=3)
    assert photo.with_(arraw.DevelopState()).marks.rating == 3
    assert photo.with_(exposure=1.0).marks.rating == 3


@pytest.mark.parametrize("rating", [6, -2, 100])
def test_out_of_range_rating_is_value_error(dng, rating):
    with pytest.raises(ValueError):
        arraw.open(dng, sidecar=False).with_(rating=rating)


@pytest.mark.parametrize("rating", [True, 2.0, "2"])
def test_with_rating_is_strict(dng, rating):
    with pytest.raises(TypeError, match="rating"):
        arraw.open(dng, sidecar=False).with_(rating=rating)


@pytest.mark.parametrize("label", ["Red", 1])
def test_with_label_is_strict(dng, label):
    with pytest.raises(TypeError, match="label"):
        arraw.open(dng, sidecar=False).with_(label=label)


def test_marks_take_part_in_photo_equality_and_repr(dng):
    photo = arraw.open(dng, sidecar=False)
    assert photo.with_(rating=3) != photo
    assert photo.with_(rating=3) == photo.with_(rating=3)
    assert "marks=PhotoMarks(rating=3" in repr(photo.with_(rating=3))


def test_out_of_range_marks_are_values_until_a_photo_takes_them(dng):
    marks = arraw.PhotoMarks(rating=9)  # a value; only a Photo refuses it
    assert marks.rating == 9
    with pytest.raises(ValueError):
        arraw.open(dng, sidecar=False).with_(marks=marks)


def test_with_marks_replaces_them_and_flat_keywords_apply_on_top(dng):
    photo = arraw.open(dng, sidecar=False).with_(rating=1, label=arraw.ColorLabel.RED)
    marks = arraw.PhotoMarks(rating=4, label=arraw.ColorLabel.BLUE)
    assert photo.with_(marks=marks).marks == marks
    assert photo.with_(marks=marks, rating=2).marks == marks.replace(rating=2)
    assert photo.with_(marks=arraw.PhotoMarks()).marks == arraw.PhotoMarks()


def test_numpy_integers_are_ratings(dng):
    np = pytest.importorskip("numpy")
    assert arraw.PhotoMarks(rating=np.int64(3)).rating == 3
    assert arraw.open(dng, sidecar=False).with_(rating=np.int64(3)).marks.rating == 3
    with pytest.raises(TypeError):
        arraw.PhotoMarks(rating=np.bool_(True))


# ---- strictness of the frozen classes ------------------------------------------------------

STRICT_CASES = [
    (arraw.PhotoMarks, "rating", True),
    (arraw.PhotoMarks, "rating", 2.0),
    (arraw.PhotoMarks, "label", 0),
    (arraw.GeometrySettings, "rotation", 1),
    (arraw.GeometrySettings, "straighten", True),
    (arraw.GeometrySettings, "flip_horizontal", 1.5),
    (arraw.ColorSettings, "white_balance", 0),
    (arraw.ColorSettings, "temperature", True),
    (arraw.ToneSettings, "exposure", True),
    (arraw.ToneSettings, "exposure", "1"),
]


@pytest.mark.parametrize(("cls", "name", "value"), STRICT_CASES)
def test_constructor_and_replace_refuse_the_same_values(cls, name, value):
    with pytest.raises(TypeError, match=name):
        cls(**{name: value})
    with pytest.raises(TypeError, match=name):
        cls().replace(**{name: value})


# ---- sidecar_path --------------------------------------------------------------------------


def test_sidecar_path_is_a_pathlib_path(work):
    result = arraw.sidecar_path(work)
    assert isinstance(result, Path)
    assert result == work.with_suffix(".xmp")
    assert arraw.sidecar_path(str(work)) == result
    assert not result.exists()


def test_sidecar_path_pair_rule(work, png):
    jpeg_like = work.with_suffix(".png")
    shutil.copy(png, jpeg_like)
    # The RAW keeps <stem>.xmp; the other image of the pair gets <name>.<ext>.xmp.
    assert arraw.sidecar_path(work) == work.with_suffix(".xmp")
    assert arraw.sidecar_path(jpeg_like) == work.parent / "shot.png.xmp"


def test_sidecar_path_alone_is_the_stem(tmp_path, png):
    lone = tmp_path / "lone.png"
    shutil.copy(png, lone)
    assert arraw.sidecar_path(lone) == tmp_path / "lone.xmp"


# ---- read_sidecar / write_sidecar ----------------------------------------------------------


def test_read_sidecar_without_one_is_none(work):
    assert arraw.read_sidecar(work) is None


def test_round_trip_through_write_and_read(work):
    photo = arraw.open(work).with_(
        exposure=0.75, contrast=12, rating=4, label=arraw.ColorLabel.PURPLE
    )
    arraw.write_sidecar(photo)
    assert arraw.sidecar_path(work).is_file()
    contents = arraw.read_sidecar(work)
    assert isinstance(contents, arraw.SidecarContents)
    assert contents.marks == photo.marks
    assert contents.state == photo.state
    with pytest.raises(AttributeError):
        contents.marks = arraw.PhotoMarks()
    assert contents == arraw.read_sidecar(work)
    assert hash(contents) == hash(arraw.read_sidecar(work))


def test_read_sidecar_reports_other_tools(work):
    shutil.copy(Path(__file__).parents[1] / "fixtures" / "sidecar-foreign.xmp", arraw.sidecar_path(work))
    contents = arraw.read_sidecar(work)
    assert contents.creator_tool == "Adobe Lightroom Classic 13.0 (Macintosh)"
    assert [(o.prefix, o.properties) for o in contents.others] == [("crs", 4), ("acme", 2), ("dc", 1)]
    assert arraw.xmp_namespace_owner(contents.others[0].uri) == "Adobe Camera Raw / Lightroom develop settings"
    assert arraw.xmp_namespace_owner(contents.others[1].uri) is None
    assert hash(contents) == hash(arraw.read_sidecar(work))


def test_open_reads_the_sidecar(work):
    photo = arraw.open(work, sidecar=False).with_(
        exposure=-1.5, tint=20, rating=-1, label=arraw.ColorLabel.YELLOW
    )
    arraw.write_sidecar(photo)
    opened = arraw.open(work)
    assert opened == photo
    assert opened.marks == arraw.PhotoMarks(rating=-1, label=arraw.ColorLabel.YELLOW)
    assert opened.state.settings.tone.exposure == pytest.approx(-1.5)


def test_open_without_sidecar_ignores_it(work):
    arraw.write_sidecar(arraw.open(work).with_(exposure=1.0, rating=5))
    bare = arraw.open(work, sidecar=False)
    assert bare.state == arraw.DevelopState()
    assert bare.marks == arraw.PhotoMarks()
    assert bare.metadata == arraw.open(work).metadata
    assert arraw.open(work).marks.rating == 5


def test_open_sidecar_is_keyword_only(work):
    with pytest.raises(TypeError):
        arraw.open(work, False)


def test_open_without_sidecar_does_not_read_a_broken_one(work, caplog):
    arraw.sidecar_path(work).write_text("<<< not xml")
    with caplog.at_level(logging.INFO, logger="arraw"):
        photo = arraw.open(work, sidecar=False)
    assert photo.state == arraw.DevelopState()
    assert not arraw_records(caplog, logging.ERROR)


def test_clearing_the_label_is_written(work):
    marked = arraw.open(work).with_(label=arraw.ColorLabel.RED)
    arraw.write_sidecar(marked)
    assert arraw.open(work).marks.label == arraw.ColorLabel.RED
    arraw.write_sidecar(marked.with_(label=None))
    assert arraw.open(work).marks.label is None


def test_write_preserves_foreign_attributes(work, fixtures):
    sidecar = work.with_suffix(".xmp")
    shutil.copy(fixtures / "sidecar-foreign.xmp", sidecar)
    photo = arraw.open(work)
    assert photo.marks == arraw.PhotoMarks(rating=3, label=arraw.ColorLabel.BLUE)
    assert photo.state.settings.tone.exposure == pytest.approx(1.25)
    arraw.write_sidecar(photo.with_(exposure=2.0, rating=1))

    root = ET.parse(sidecar).getroot()
    text = sidecar.read_text(encoding="utf-8")
    crs = {
        el_key.removeprefix("{" + CRS + "}"): value
        for el in root.iter()
        for el_key, value in el.attrib.items()
        if el_key.startswith("{" + CRS + "}")
    }
    assert crs == {
        "Version": "15.0",
        "Exposure2012": "+0.50",
        "Contrast2012": "+10",
        "HasCrop": "False",
    }
    assert "futureKnob" in text
    assert "harbour" in text and "A. Photographer" in text and "someone@example.com" in text

    again = arraw.open(work)
    assert again.marks.rating == 1
    assert again.marks.label == arraw.ColorLabel.BLUE
    assert again.state.settings.tone.exposure == pytest.approx(2.0)
    assert again.state.settings.tone.contrast == pytest.approx(0.5)


def test_write_to_an_unwritable_place_is_runtime_error(work):
    photo = arraw.open(work)
    sidecar = arraw.sidecar_path(work)
    sidecar.mkdir()  # a directory where the file should go
    with pytest.raises(RuntimeError):
        arraw.write_sidecar(photo)


def test_write_over_a_non_xmp_sidecar_is_runtime_error(work):
    photo = arraw.open(work)
    arraw.sidecar_path(work).write_text("plain text, not XMP")
    with pytest.raises(RuntimeError):
        arraw.write_sidecar(photo)
    assert arraw.sidecar_path(work).read_text() == "plain text, not XMP"


def test_read_unreadable_sidecar_is_runtime_error(work):
    arraw.sidecar_path(work).write_text("<<< not xml")
    with pytest.raises(RuntimeError):
        arraw.read_sidecar(work)


def test_open_logs_error_for_an_unreadable_sidecar_and_returns_defaults(work, caplog):
    arraw.sidecar_path(work).write_text("<<< not xml")
    with caplog.at_level(logging.INFO, logger="arraw"):
        photo = arraw.open(work)
    assert photo.state == arraw.DevelopState()
    assert photo.marks == arraw.PhotoMarks()
    errors = arraw_records(caplog, logging.ERROR)
    assert errors and all(r.levelno == logging.ERROR for r in errors)
    assert any(arraw.sidecar_path(work).name in r.getMessage() for r in errors)


def test_sidecar_clamping_on_read_logs_warning(work, caplog):
    arraw.write_sidecar(arraw.open(work))
    sidecar = arraw.sidecar_path(work)
    text = sidecar.read_text(encoding="utf-8")
    assert 'arraw:exposure="0"' in text
    sidecar.write_text(text.replace('arraw:exposure="0"', 'arraw:exposure="99"'), encoding="utf-8")
    with caplog.at_level(logging.INFO, logger="arraw"):
        photo = arraw.open(work)
    assert photo.state.settings.tone.exposure == pytest.approx(5.0)
    assert arraw_records(caplog)
