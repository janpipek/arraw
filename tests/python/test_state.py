import pytest

import arraw


def test_default_state_holds_default_settings():
    state = arraw.DevelopState()
    assert state.settings == arraw.DevelopSettings()
    assert state == arraw.DevelopState(settings=None)


def test_state_is_keyword_only():
    with pytest.raises(TypeError):
        arraw.DevelopState(arraw.DevelopSettings())


def test_state_equality_and_hash():
    brighter = arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=0.5))
    a = arraw.DevelopState(settings=brighter)
    b = arraw.DevelopState(settings=brighter)
    assert a == b
    assert hash(a) == hash(b)
    assert a != arraw.DevelopState()
    assert len({a, b}) == 1


def test_state_replace():
    brighter = arraw.DevelopSettings(tone=arraw.ToneSettings(exposure=0.5))
    assert arraw.DevelopState().replace(settings=brighter).settings == brighter
    with pytest.raises(TypeError):
        arraw.DevelopState().replace(bogus=1)


def test_state_repr_names_settings():
    assert repr(arraw.DevelopState()).startswith("DevelopState(settings=DevelopSettings(")


def test_state_is_read_only():
    with pytest.raises(AttributeError):
        arraw.DevelopState().settings = arraw.DevelopSettings()


def test_photo_state_follows_flat_keys(dng):
    photo = arraw.open(dng)
    assert photo.state == arraw.DevelopState()
    assert photo.with_(exposure=0.5).state.settings.tone.exposure == pytest.approx(0.5)
