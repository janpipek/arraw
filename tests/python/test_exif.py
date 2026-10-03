import dataclasses
import math

import pytest

import arraw


@pytest.fixture(scope="session")
def exif_dng(fixtures):
    return fixtures / "exif-32x24.dng"


def test_read_exif_attributes(exif_dng):
    info = arraw.read_exif(exif_dng)
    assert info.make == "Arraw"
    assert info.model == "Fixture One"
    assert info.lens_model == "Fixture 35mm F2.8"
    assert info.date_time_original == "2024:05:01 10:00:00"
    assert info.offset_time_original == "+02:00"
    assert info.exposure_time == arraw.URational(1, 250)
    assert info.f_number == arraw.URational(28, 10)
    assert info.photographic_sensitivity == 400
    assert info.focal_length == arraw.URational(35, 1)
    assert info.focal_length_in_35mm_film == 52
    assert info.exposure_bias_value == arraw.SRational(-1, 3)
    assert info.flash == 16
    assert info.artist == "Ada Lovelace"
    assert info.copyright == "(c) 2024 Ada Lovelace"
    assert info.gps.latitude == pytest.approx(50.0877083)
    assert info.gps.longitude == pytest.approx(14.4216667)
    assert info.gps.altitude == pytest.approx(235.5)


def test_rationals(exif_dng):
    info = arraw.read_exif(exif_dng)
    assert info.exposure_time.numerator == 1
    assert info.exposure_time.denominator == 250
    assert info.exposure_time.value() == pytest.approx(0.004)
    assert info.exposure_bias_value.value() == pytest.approx(-1 / 3)
    assert math.isnan(arraw.URational(1, 0).value())
    assert arraw.SRational(-1, 2) != arraw.SRational(1, 2)
    assert hash(arraw.URational(1, 2)) == hash(arraw.URational(1, 2))


def test_values_are_frozen(exif_dng):
    info = arraw.read_exif(exif_dng)
    with pytest.raises(AttributeError):
        info.make = "other"
    with pytest.raises(AttributeError):
        info.exposure_time.numerator = 2
    assert info == arraw.read_exif(exif_dng)
    assert "make='Arraw'" in repr(info)
    assert not dataclasses.is_dataclass(info)


def test_file_without_exif_is_empty(png):
    info = arraw.read_exif(png)
    assert info == arraw.ExifInfo()
    assert info.make is None
    assert info.gps is None
    assert info.exposure_time is None


def test_missing_file_raises(tmp_path):
    with pytest.raises(RuntimeError):
        arraw.read_exif(tmp_path / "absent.dng")


def test_values_can_be_built():
    info = arraw.ExifInfo(make="X", gps=arraw.GpsPosition(latitude=-1.5, longitude=2.5))
    assert info.gps.altitude is None
    assert info.gps.latitude == -1.5
    assert info.model is None
