import shutil

import pytest

import arraw


@pytest.fixture(scope="module")
def developed(dng):
    return arraw.develop(arraw.open(dng).with_(exposure=0.3))


@pytest.mark.parametrize("name", ["out.jpg", "out.jpeg", "out.png", "out.tif", "out.tiff"])
def test_save_infers_format_from_extension(developed, tmp_path, name):
    target = tmp_path / name
    arraw.save(developed, target)
    assert target.stat().st_size > 0
    assert arraw.read_metadata(target).size == arraw.ImageSize(32, 24)
    assert arraw.load(target).size == arraw.ImageSize(32, 24)


def test_file_signature_matches_extension(developed, tmp_path):
    magic = {"a.jpg": b"\xff\xd8", "a.png": b"\x89PNG", "a.tif": (b"II*\x00", b"MM\x00*")}
    for name, expected in magic.items():
        arraw.save(developed, tmp_path / name)
        head = (tmp_path / name).read_bytes()[:4]
        assert head.startswith(expected)


def test_explicit_format_overrides_extension(developed, tmp_path):
    target = tmp_path / "actually.dat"
    arraw.save(developed, target, format=arraw.ImageFileFormat.PNG)
    assert target.read_bytes()[:4] == b"\x89PNG"


@pytest.mark.parametrize("name", ["a.png", "a.tif"])
def test_sixteen_bit(developed, tmp_path, name):
    arraw.save(developed, tmp_path / name, bit_depth=16)
    back = arraw.load(tmp_path / name)
    assert back.size == arraw.ImageSize(32, 24)
    assert back.pixels.dtype.itemsize >= 2


def test_sixteen_bit_jpeg_is_value_error(developed, tmp_path):
    with pytest.raises(ValueError):
        arraw.save(developed, tmp_path / "bad.jpg", bit_depth=16)
    assert not (tmp_path / "bad.jpg").exists()


def test_jpeg_quality_changes_size(developed, tmp_path):
    arraw.save(developed, tmp_path / "lo.jpg", quality=10)
    arraw.save(developed, tmp_path / "hi.jpg", quality=100)
    assert (tmp_path / "lo.jpg").stat().st_size < (tmp_path / "hi.jpg").stat().st_size


@pytest.mark.parametrize("encoding", list(arraw.NamedEncoding))
def test_encodings_write(developed, tmp_path, encoding):
    if encoding in (arraw.NamedEncoding.LINEAR_REC2020, arraw.NamedEncoding.REC2020_GAMMA22):
        pytest.skip("the working and perceptual encodings are internal, not output ones")
    arraw.save(developed, tmp_path / "e.png", encoding=encoding)
    assert (tmp_path / "e.png").stat().st_size > 0


def test_tiff_display_p3_sixteen_bit(developed, tmp_path):
    arraw.save(developed, tmp_path / "p3.tif", encoding=arraw.NamedEncoding.DISPLAY_P3,
               bit_depth=16)
    assert arraw.load(tmp_path / "p3.tif").size == arraw.ImageSize(32, 24)


def test_embed_profile_false_is_smaller(developed, tmp_path):
    arraw.save(developed, tmp_path / "with.png")
    arraw.save(developed, tmp_path / "without.png", embed_profile=False)
    assert (tmp_path / "without.png").stat().st_size <= (tmp_path / "with.png").stat().st_size


def test_save_accepts_str_path(developed, tmp_path):
    arraw.save(developed, str(tmp_path / "s.png"))
    assert (tmp_path / "s.png").exists()


def test_save_to_missing_directory_raises(developed, tmp_path):
    with pytest.raises(RuntimeError):
        arraw.save(developed, tmp_path / "nope" / "x.png")


def test_unknown_extension_raises(developed, tmp_path):
    with pytest.raises(ValueError):
        arraw.save(developed, tmp_path / "x.xyz")


def test_sharpening_round_trip(developed, tmp_path):
    arraw.save(developed, tmp_path / "off.png")
    arraw.save(developed, tmp_path / "zero.png", sharpening=0)
    arraw.save(developed, tmp_path / "sharp.png", sharpening=100)
    assert (tmp_path / "off.png").read_bytes() == (tmp_path / "zero.png").read_bytes()
    assert (tmp_path / "sharp.png").read_bytes() != (tmp_path / "off.png").read_bytes()


@pytest.mark.parametrize("amount", [-1, 101])
def test_sharpening_out_of_range_is_value_error(developed, tmp_path, amount):
    with pytest.raises(ValueError):
        arraw.save(developed, tmp_path / "bad.png", sharpening=amount)
    assert not (tmp_path / "bad.png").exists()


@pytest.fixture(scope="module")
def exif_photo(fixtures):
    return arraw.open(fixtures / "exif-32x24.dng")


def test_save_without_metadata_from_writes_no_metadata(exif_photo, tmp_path):
    arraw.save(arraw.develop(exif_photo), tmp_path / "plain.jpg")
    assert arraw.read_exif(tmp_path / "plain.jpg") == arraw.ExifInfo()


def test_save_carries_capture_but_not_location_by_default(exif_photo, tmp_path):
    arraw.save(arraw.develop(exif_photo), tmp_path / "a.jpg", metadata_from=exif_photo)
    info = arraw.read_exif(tmp_path / "a.jpg")
    assert info.make == "Arraw"
    assert info.f_number == arraw.URational(28, 10)
    assert info.gps is None


def test_save_metadata_selection(exif_photo, tmp_path):
    selection = arraw.MetadataSelection(capture=False, location=True, descriptive=False)
    arraw.save(arraw.develop(exif_photo), tmp_path / "b.png", metadata_from=exif_photo,
               metadata=selection)
    info = arraw.read_exif(tmp_path / "b.png")
    assert info.gps is not None
    assert info.make is None


def test_metadata_selection_defaults_and_value_semantics():
    default = arraw.MetadataSelection()
    assert (default.capture, default.location, default.descriptive) == (True, False, True)
    assert default.replace(location=True).location is True
    assert default == arraw.MetadataSelection()


def test_save_metadata_from_missing_source_still_saves_and_warns(
    exif_photo, fixtures, tmp_path, caplog
):
    copy = tmp_path / "gone.dng"
    shutil.copy(fixtures / "exif-32x24.dng", copy)
    ghost = arraw.open(copy)
    copy.unlink()
    with caplog.at_level("WARNING", logger="arraw"):
        arraw.save(arraw.develop(exif_photo), tmp_path / "c.jpg", metadata_from=ghost)
    assert (tmp_path / "c.jpg").exists()
    assert arraw.read_exif(tmp_path / "c.jpg") == arraw.ExifInfo()
    assert any("without some metadata" in r.getMessage() for r in caplog.records)
