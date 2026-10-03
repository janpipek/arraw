import subprocess

import numpy as np
import pytest

import arraw

CASES = {
    "exposure": {"exposure": 0.5},
    "contrast": {"contrast": 20},
    "colour": {"saturation": 30, "vibrance": 20, "hue_red": 40, "luminance_green": -20},
    "curves": {
        "tone_curve_luma": [(0.0, 0.0), (0.25, 0.2), (0.75, 0.82), (1.0, 1.0)],
        "tone_curve_red": [(0.0, 0.0), (0.5, 0.6), (1.0, 1.0)],
        "tone_curve_blue": [(0.0, 0.05), (1.0, 0.9)],
    },
    "combined": {"exposure": 0.5, "contrast": 20, "shadows": 15, "filmic_highlights": 0},
}


def cli_args(settings):
    args = []
    for key, value in settings.items():
        if isinstance(value, list):
            value = ";".join(f"{x},{y}" for x, y in value)
        args += ["--" + key.replace("_", "-"), str(value)]
    return args


@pytest.mark.parametrize("fmt, ext", [("tiff", ".tif"), ("png", ".png")])
@pytest.mark.parametrize("case", CASES)
@pytest.mark.parametrize("fixture", ["dng", "bayer_dng"])
def test_python_matches_cli_export(request, cli, tmp_path, fixture, case, fmt, ext):
    source = request.getfixturevalue(fixture)
    settings = CASES[case]

    photo = arraw.open(source).with_(**settings)
    py_out = tmp_path / f"py{ext}"
    arraw.save(arraw.develop(photo), py_out, bit_depth=16)

    outdir = tmp_path / "cli"
    outdir.mkdir()
    result = subprocess.run(
        [str(cli), "export", "--quiet", "--device", "cpu", "--format", fmt, "--bit-depth", "16",
         "-o", str(outdir), *cli_args(settings), str(source)],
        capture_output=True, text=True, timeout=120,
    )
    assert result.returncode == 0, result.stderr
    (cli_out,) = list(outdir.iterdir())

    py_px = arraw.load(py_out).pixels.astype(np.int64)
    cli_px = arraw.load(cli_out).pixels.astype(np.int64)
    assert py_px.shape == cli_px.shape
    # Both sides run the same CPU pipeline on float32 settings: bit-exact.
    assert np.array_equal(py_px, cli_px)
