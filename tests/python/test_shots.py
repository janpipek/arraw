from pathlib import Path

import pytest

import arraw
from arraw import ColorLabel, MarksFilter, PhotoMarks


def test_group_shots_pairs_a_raw_with_its_standard_images():
    shots = arraw.group_shots(
        ["/p/IMG_1.jpg", "/p/IMG_1.ARW", "/p/img_1.png", "/p/IMG_2.jpg"]
    )
    assert shots == [
        arraw.Shot(
            primary=Path("/p/IMG_1.ARW"),
            companions=[Path("/p/IMG_1.jpg"), Path("/p/img_1.png")],
        ),
        arraw.Shot(primary=Path("/p/IMG_2.jpg")),
    ]
    assert shots[0].format_label == "ARW+JPEG+PNG"
    assert shots[1].format_label == "JPEG"
    assert shots[0].primary == Path("/p/IMG_1.ARW")
    assert shots[1].companions == []


def test_group_shots_rules():
    # Two RAWs of one stem leave every file of it standing alone.
    assert len(arraw.group_shots(["/p/a.cr2", "/p/a.nef", "/p/a.jpg"])) == 3
    # Different folders never pair; unsupported files and sidecars are dropped.
    assert len(arraw.group_shots(["/a/x.cr2", "/b/x.jpg"])) == 2
    assert arraw.group_shots(["/p/a.xmp", "/p/a.txt"]) == []
    assert arraw.group_shots([]) == []


def test_group_shots_natural_order():
    shots = arraw.group_shots(["/p/IMG_10.jpg", "/p/img_2.jpg", "/p/IMG_1.jpg"])
    assert [s.primary.name for s in shots] == ["IMG_1.jpg", "img_2.jpg", "IMG_10.jpg"]


def test_list_shots(tmp_path):
    for name in [
        "IMG_2.ARW",
        "IMG_2.JPG",
        "IMG_10.jpg",
        "IMG_2.xmp",
        ".hidden.jpg",
        "n.txt",
    ]:
        (tmp_path / name).write_bytes(b"x")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "deep.jpg").write_bytes(b"x")
    shots = arraw.list_shots(tmp_path)
    assert shots == [
        arraw.Shot(primary=tmp_path / "IMG_2.ARW", companions=[tmp_path / "IMG_2.JPG"]),
        arraw.Shot(primary=tmp_path / "IMG_10.jpg"),
    ]
    assert shots[0].format_label == "ARW+JPEG"
    assert arraw.list_shots(str(tmp_path)) == shots


def test_list_shots_of_an_empty_or_unreadable_folder(tmp_path):
    assert arraw.list_shots(tmp_path) == []
    with pytest.raises(RuntimeError):
        arraw.list_shots(tmp_path / "missing")


def test_a_listed_shot_opens(tmp_path, dng):
    (tmp_path / "frame.dng").write_bytes(dng.read_bytes())
    (shot,) = arraw.list_shots(tmp_path)
    assert arraw.open(shot.primary).metadata.size.width == 32


def test_shot_is_a_frozen_value():
    shot = arraw.Shot(primary=Path("/p/a.arw"))
    assert hash(shot) == hash(arraw.Shot(primary=Path("/p/a.arw")))
    assert shot.replace(companions=[Path("/p/a.jpg")]).format_label == "ARW+JPEG"
    with pytest.raises(AttributeError):
        shot.primary = Path("/q")


def test_supported_extensions():
    assert "arw" in arraw.SUPPORTED_EXTENSIONS
    assert "jpeg" in arraw.SUPPORTED_EXTENSIONS
    assert all(e == e.lower() and "." not in e for e in arraw.SUPPORTED_EXTENSIONS)
    assert arraw.is_supported_image("x/IMG.CR3")
    assert not arraw.is_supported_image("x/IMG.xmp")
    for extension in arraw.SUPPORTED_EXTENSIONS:
        assert arraw.is_supported_image(f"a.{extension}")


def test_marks_filter_semantics():
    assert not MarksFilter().is_active
    assert MarksFilter().matches(PhotoMarks(rating=-1))

    stars = MarksFilter(min_rating=3)
    assert stars.is_active
    assert [stars.matches(PhotoMarks(rating=r)) for r in (-1, 0, 2, 3, 5)] == [
        False,
        False,
        False,
        True,
        True,
    ]

    rejects = MarksFilter(rejects_only=True)
    assert rejects.matches(PhotoMarks(rating=-1))
    assert not rejects.matches(PhotoMarks(rating=0))

    colours = MarksFilter(labels={ColorLabel.RED, ColorLabel.GREEN})
    assert colours.matches(PhotoMarks(label=ColorLabel.GREEN))
    assert not colours.matches(PhotoMarks(label=ColorLabel.BLUE))
    assert not colours.matches(PhotoMarks())

    both = MarksFilter(min_rating=3, labels={ColorLabel.RED})
    assert both.matches(PhotoMarks(rating=4, label=ColorLabel.RED))
    assert not both.matches(PhotoMarks(rating=2, label=ColorLabel.RED))
    assert not both.matches(PhotoMarks(rating=4, label=ColorLabel.BLUE))


def test_marks_filter_is_a_frozen_value():
    a = MarksFilter(min_rating=2, labels={ColorLabel.BLUE})
    assert a == MarksFilter(min_rating=2, labels={ColorLabel.BLUE})
    assert a != MarksFilter(min_rating=2)
    assert hash(a) == hash(MarksFilter(min_rating=2, labels={ColorLabel.BLUE}))
    assert a.labels == {ColorLabel.BLUE}
    assert a.replace(min_rating=4).min_rating == 4
    with pytest.raises(AttributeError):
        a.min_rating = 1


def test_marks_filter_refuses_a_contradiction():
    with pytest.raises(ValueError):
        MarksFilter(min_rating=2, rejects_only=True).matches(PhotoMarks())
    with pytest.raises(ValueError):
        MarksFilter(min_rating=9).matches(PhotoMarks())
