import logging

import arraw


def arraw_records(caplog, level=logging.INFO):
    return [r for r in caplog.records if r.name == "arraw" and r.levelno >= level]


def test_missing_white_balance_logs_warning(nowb_dng, caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        arraw.develop(arraw.open(nowb_dng))
    warnings = [r for r in arraw_records(caplog) if r.levelno == logging.WARNING]
    assert warnings, "expected a WARNING on the 'arraw' logger"
    assert any("white balance" in r.getMessage().lower() for r in warnings)


def test_opened_photo_reports_once(nowb_dng, caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        photo = arraw.open(nowb_dng)
        photo.load()
        arraw.develop(photo)
    assert len([r for r in arraw_records(caplog) if r.levelno == logging.WARNING]) == 1


def test_load_by_path_reports(nowb_dng, caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        arraw.load(nowb_dng)
    assert any(r.levelno == logging.WARNING for r in arraw_records(caplog))


def test_neutral_fixture_logs_no_warning(dng, caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        arraw.develop(arraw.open(dng))
    assert not [r for r in arraw_records(caplog) if r.levelno >= logging.WARNING]


def test_png_logs_no_warning(png, caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        arraw.develop(arraw.open(png))
    assert not [r for r in arraw_records(caplog) if r.levelno >= logging.WARNING]


def test_severity_enum_members():
    assert {s.name for s in arraw.Severity} == {"INFO", "WARNING", "ERROR"}
