from pathlib import Path

import pytest

import arraw

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures"
DNG = FIXTURES / "linear-32x24-neutral.dng"


def test_flat_keys_and_errors():
    photo = arraw.open(DNG)
    brighter = photo.with_(exposure=0.5, crop_aspect=arraw.CropRatio(1.5))
    assert brighter.state.settings.tone.exposure == pytest.approx(0.5)
    assert brighter.state.settings.geometry.crop.aspect == arraw.CropRatio(1.5)
    assert photo.state.settings.tone.exposure == 0.0
    with pytest.raises(TypeError, match="bogus"):
        photo.with_(bogus=1)
    with pytest.raises(ValueError):
        photo.with_(exposure=99)


def test_develop_and_save(tmp_path):
    photo = arraw.open(DNG).with_(exposure=0.3)
    developed = arraw.develop(photo)
    assert developed.pixels.shape == (24, 32, 4)
    for name in ("out.jpg", "out.png", "out.tif"):
        arraw.save(developed, tmp_path / name)
        assert arraw.read_metadata(tmp_path / name).size == arraw.ImageSize(32, 24)
    with pytest.raises(ValueError):
        arraw.save(developed, tmp_path / "bad.jpg", bit_depth=16)


def test_descriptors_cover_the_table():
    names = {d.name for d in arraw.setting_descriptors()}
    assert {"exposure", "filmic_highlights", "crop_rectangle"} <= names
