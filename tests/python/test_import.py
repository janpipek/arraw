import re
from pathlib import Path

import pytest

import arraw

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures"


def test_version_matches_cmake_project():
    text = (ROOT / "CMakeLists.txt").read_text()
    match = re.search(r"project\(arraw VERSION (\d+\.\d+\.\d+)", text)
    assert match
    assert arraw.__version__ == match.group(1)


def test_png_metadata():
    metadata = arraw.read_metadata(FIXTURES / "testcard-61x41-srgb8.png")
    assert metadata.size == arraw.ImageSize(61, 41)
    assert metadata.orientation == arraw.ImageOrientation.NORMAL
    assert "61" in repr(metadata.size)


def test_png_load_matches_metadata():
    path = FIXTURES / "testcard-61x41-srgb8.png"
    assert arraw.load(path).size == arraw.read_metadata(path).size


def test_dng_load_matches_metadata():
    path = FIXTURES / "linear-32x24-neutral.dng"
    metadata = arraw.read_metadata(path)
    assert metadata.size == arraw.ImageSize(32, 24)
    assert arraw.load(str(path)).size == metadata.size


def test_missing_file_raises():
    with pytest.raises(RuntimeError):
        arraw.load(FIXTURES / "no-such-file.dng")
    with pytest.raises(RuntimeError):
        arraw.read_metadata(FIXTURES / "no-such-file.png")


def test_image_size_is_read_only_and_hashable():
    size = arraw.ImageSize(3, 2)
    with pytest.raises(AttributeError):
        size.width = 5
    assert size == arraw.ImageSize(3, 2)
    assert hash(size) == hash(arraw.ImageSize(3, 2))
    assert hash(size) != hash(arraw.ImageSize(2, 3))
    assert len({size, arraw.ImageSize(3, 2)}) == 1
