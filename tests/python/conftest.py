import os
import shutil
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures"


@pytest.fixture(scope="session")
def fixtures() -> Path:
    return FIXTURES


@pytest.fixture(scope="session")
def dng() -> Path:
    """Linear 32x24 DNG with as-shot white balance and no orientation."""
    return FIXTURES / "linear-32x24-neutral.dng"


@pytest.fixture(scope="session")
def bayer_dng() -> Path:
    return FIXTURES / "bayer-32x24.dng"


@pytest.fixture(scope="session")
def rotated_dng() -> Path:
    return FIXTURES / "linear-32x24-rotated.dng"


@pytest.fixture(scope="session")
def nowb_dng() -> Path:
    """DNG without an as-shot neutral; the importer substitutes daylight."""
    return FIXTURES / "linear-32x24-nowb.dng"


@pytest.fixture(scope="session")
def png() -> Path:
    return FIXTURES / "testcard-61x41-srgb8.png"


@pytest.fixture(scope="session")
def cli() -> Path:
    prefix = os.environ.get("ARRAW_BUILD_PREFIX", "")
    candidate = ROOT / "build" / f"{prefix}debug" / "arraw-cli"
    if candidate.is_file():
        return candidate
    found = shutil.which("arraw-cli")
    if found:
        return Path(found)
    message = f"arraw-cli not built ({candidate} is absent); run `just py-test` or `just build`"
    if os.environ.get("ARRAW_REQUIRE_CLI"):
        pytest.fail(message)
    pytest.skip(f"arraw-cli not built ({candidate} is absent); run `just build`")
