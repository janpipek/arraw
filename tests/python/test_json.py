import json
import logging

import pytest

import arraw
from test_photo import find, same, sample


def arraw_records(caplog, level=logging.WARNING):
    return [r for r in caplog.records if r.name == "arraw" and r.levelno >= level]


def all_leaves():
    return {d.name: sample(d) for d in arraw.setting_descriptors()}


def build(**flat):
    return arraw.DevelopSettings().with_(**flat)


def test_default_document_shape():
    document = json.loads(arraw.DevelopSettings().to_json())
    assert document["arraw"] == 1
    keys = [d.key for d in arraw.setting_descriptors()]
    assert list(document["settings"]) == keys


@pytest.mark.parametrize("descriptor", arraw.setting_descriptors(), ids=lambda d: d.name)
def test_round_trip_of_every_descriptor(descriptor):
    settings = build(**{descriptor.name: sample(descriptor)})
    assert settings != arraw.DevelopSettings()
    back = arraw.DevelopSettings.from_json(settings.to_json())
    for d in arraw.setting_descriptors():
        assert same(find(back, d.name), find(settings, d.name)), d.name


def test_round_trip_of_all_descriptors_together():
    settings = build(**all_leaves())
    back = arraw.DevelopSettings.from_json(settings.to_json())
    for d in arraw.setting_descriptors():
        assert same(find(back, d.name), find(settings, d.name)), d.name


def test_defaults_round_trip_exactly():
    text = arraw.DevelopSettings().to_json()
    assert arraw.DevelopSettings.from_json(text) == arraw.DevelopSettings()


def test_partial_document_applies_onto_base():
    base = build(exposure=1.0, contrast=20.0)
    result = arraw.DevelopSettings.from_json('{"arraw": 1, "settings": {"contrast": 5}}', base)
    assert result.tone.exposure == pytest.approx(1.0)
    assert result.tone.contrast == pytest.approx(5.0)


def test_partial_document_applies_onto_defaults():
    result = arraw.DevelopSettings.from_json('{"arraw": 1, "settings": {"exposure": 0.5}}')
    assert result == arraw.DevelopSettings().with_(exposure=0.5)


def test_null_unsets_an_optional():
    base = build(white_balance=arraw.WhiteBalanceMode.CUSTOM, temperature=5200)
    result = arraw.DevelopSettings.from_json(
        '{"arraw": 1, "settings": {"temperature": null}}', base
    )
    assert result.color.temperature is None
    assert result.color.white_balance == arraw.WhiteBalanceMode.CUSTOM


def test_clamped_value_warns_on_the_logger(caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        result = arraw.DevelopSettings.from_json('{"arraw": 1, "settings": {"exposure": 99}}')
    assert result.tone.exposure == pytest.approx(5.0)
    records = arraw_records(caplog)
    assert records
    assert any("exposure" in r.getMessage() for r in records)


def test_unknown_key_warns_and_is_skipped(caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        result = arraw.DevelopSettings.from_json('{"arraw": 1, "settings": {"bogusKnob": 3}}')
    assert result == arraw.DevelopSettings()
    assert any("bogusKnob" in r.getMessage() for r in arraw_records(caplog))


def test_malformed_value_warns_and_keeps_the_base(caplog):
    base = build(exposure=1.0)
    with caplog.at_level(logging.INFO, logger="arraw"):
        result = arraw.DevelopSettings.from_json(
            '{"arraw": 1, "settings": {"exposure": "bright"}}', base
        )
    assert result.tone.exposure == pytest.approx(1.0)
    assert arraw_records(caplog)


def test_clean_document_logs_nothing(caplog):
    with caplog.at_level(logging.INFO, logger="arraw"):
        arraw.DevelopSettings.from_json(build(**all_leaves()).to_json())
    assert not arraw_records(caplog, logging.INFO)


@pytest.mark.parametrize(
    "text",
    [
        "",
        "not json",
        "[]",
        "{}",
        '{"arraw": 1}',
        '{"arraw": "one", "settings": {}}',
        '{"arraw": 0, "settings": {}}',
        '{"arraw": 1, "settings": []}',
        '{"arraw": 1, "settings": {"exposure": 1e999}}',
    ],
)
def test_broken_document_is_value_error(text):
    with pytest.raises(ValueError):
        arraw.DevelopSettings.from_json(text)


def test_out_of_range_result_never_escapes_as_invalid():
    # A clamped read is valid, so the result can go straight into a photograph.
    result = arraw.DevelopSettings.from_json('{"arraw": 1, "settings": {"tint": 9999}}')
    assert result.color.tint == pytest.approx(150.0)


def test_to_json_of_out_of_range_settings_is_value_error():
    bad = arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=99.0))
    with pytest.raises(ValueError):
        bad.to_json()


def test_base_must_be_settings():
    with pytest.raises(TypeError):
        arraw.DevelopSettings.from_json("{}", base=3)
