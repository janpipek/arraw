import json
import subprocess

import pytest

import arraw
from arraw import ExportPreset, ExportPresetError, ExportPresetProblem, ExportPresetStore, ExportSettings

BUILT_INS = ["Web 2048 px", "Social 1080 px", "Full size JPEG", "Print TIFF 16-bit"]


@pytest.fixture
def store(tmp_path):
    return ExportPresetStore(tmp_path / "config")


def names(store):
    return [preset.name for preset in store.presets()]


def problem_of(call, *args):
    with pytest.raises(ExportPresetError) as raised:
        call(*args)
    return raised.value.problem


def test_the_session_uses_its_own_configuration_directory(config_directory):
    assert arraw.config_directory() == config_directory
    assert ExportPresetStore().file == config_directory / "export-presets.json"


def test_save_then_presets_lists_it_and_load_returns_it(store):
    settings = ExportSettings(
        format=arraw.ImageFileFormat.PNG,
        encoding=arraw.NamedEncoding.DISPLAY_P3,
        resize=True,
        width=1500,
        height=1000,
        quality=77,
        sharpening=12,
        location=True,
    )
    store.save(ExportPreset("Client", settings))

    assert names(store) == [*BUILT_INS, "Client"]
    loaded = store.load("client")
    assert loaded.name == "Client"
    assert loaded.settings == settings.canonical()
    assert not loaded.builtin
    assert store.file.is_file()
    assert store.state() == arraw.ExportPresetFileState.READ


def test_nothing_is_created_by_reading(store):
    assert names(store) == BUILT_INS
    assert store.state() == arraw.ExportPresetFileState.MISSING
    assert not store.file.exists()
    assert store.unusable() == []


def test_saving_again_replaces_and_takes_the_new_spelling(store):
    store.save(ExportPreset("Web", ExportSettings(quality=70)))
    store.save(ExportPreset("web", ExportSettings(quality=60)))
    (mine,) = [p for p in store.presets() if not p.builtin]
    assert mine.name == "web"
    assert mine.settings.quality == 60


def test_rename(store):
    store.save(ExportPreset("One"))
    store.save(ExportPreset("Two"))
    store.rename("One", "Uno")
    assert names(store)[-2:] == ["Two", "Uno"]
    assert problem_of(store.rename, "Uno", "two") == ExportPresetProblem.NAME_TAKEN
    assert problem_of(store.rename, "Missing", "Three") == ExportPresetProblem.NOT_FOUND


def test_the_built_in_presets_are_listed_first_and_cannot_be_changed(store):
    store.save(ExportPreset("Mine"))
    listed = store.presets()
    assert [p.name for p in listed[:4]] == BUILT_INS
    assert all(p.builtin for p in listed[:4])
    assert not listed[4].builtin

    assert problem_of(store.save, ExportPreset("web 2048 PX")) == ExportPresetProblem.BUILT_IN
    assert problem_of(store.rename, "Mine", "Full size JPEG") == ExportPresetProblem.BUILT_IN
    assert problem_of(store.delete, "Social 1080 px") == ExportPresetProblem.BUILT_IN
    assert problem_of(store.rename, "Social 1080 px", "Other") == ExportPresetProblem.BUILT_IN

    web = store.load("web 2048 px")
    assert web.builtin
    assert web.settings.develop_size == (2048, 2048)
    assert store.load("Print TIFF 16-bit").settings.sixteen_bit
    assert not store.file.read_text().count("Web 2048")


def test_delete_removes_it_and_delete_of_a_missing_one_raises_not_found(store):
    store.save(ExportPreset("Gone"))
    store.delete("gone")
    assert names(store) == BUILT_INS
    assert problem_of(store.delete, "Gone") == ExportPresetProblem.NOT_FOUND
    assert problem_of(store.load, "Gone") == ExportPresetProblem.NOT_FOUND


def test_a_bad_name_or_setting_is_refused(store):
    assert problem_of(store.save, ExportPreset("")) == ExportPresetProblem.INVALID_NAME
    assert problem_of(store.save, ExportPreset("a\nb")) == ExportPresetProblem.INVALID_NAME
    with pytest.raises(ValueError):
        store.save(ExportPreset("Loud", ExportSettings(quality=101)))
    assert names(store) == BUILT_INS


def test_a_newer_file_raises_newer_file_on_save_and_is_left_alone(store):
    store.file.parent.mkdir(parents=True)
    text = json.dumps({"arraw": 99, "exportPresets": [], "future": True})
    store.file.write_text(text)

    assert store.state() == arraw.ExportPresetFileState.NEWER
    assert problem_of(store.save, ExportPreset("New")) == ExportPresetProblem.NEWER_FILE
    assert store.file.read_text() == text


def test_an_unreadable_file_is_never_written_over_but_can_be_set_aside(store):
    store.file.parent.mkdir(parents=True)
    store.file.write_text("this is not json")

    assert store.state() == arraw.ExportPresetFileState.UNREADABLE
    assert names(store) == BUILT_INS
    assert problem_of(store.save, ExportPreset("New")) == ExportPresetProblem.UNREADABLE_FILE
    assert store.file.read_text() == "this is not json"

    moved = store.set_aside()
    assert moved.read_text() == "this is not json"
    assert not store.file.exists()
    store.save(ExportPreset("New"))
    assert "New" in names(store)


def test_set_aside_refuses_a_newer_file_and_a_readable_one(store):
    store.file.parent.mkdir(parents=True)
    newer = json.dumps({"arraw": 99, "exportPresets": []})
    store.file.write_text(newer)
    assert problem_of(store.set_aside) == ExportPresetProblem.NEWER_FILE
    assert store.file.read_text() == newer

    store.file.unlink()
    store.save(ExportPreset("Good"))
    assert problem_of(store.set_aside) == ExportPresetProblem.NOT_UNREADABLE
    assert "Good" in names(store)


def test_an_unusable_preset_is_listed_apart_and_cannot_be_replaced_or_loaded(store):
    store.file.parent.mkdir(parents=True)
    entry = {"name": "Odd", "format": "webp"}
    store.file.write_text(json.dumps({"arraw": 1, "exportPresets": [entry]}))

    (odd,) = store.unusable()
    assert odd.name == "Odd"
    assert "webp" in odd.reason
    assert "Odd" not in names(store)
    assert problem_of(store.load, "odd") == ExportPresetProblem.UNUSABLE
    assert problem_of(store.save, ExportPreset("Odd")) == ExportPresetProblem.UNUSABLE
    store.delete("Odd")
    assert store.unusable() == []


def test_a_preset_shadowing_a_built_in_is_flagged_and_the_built_in_wins(store):
    store.file.parent.mkdir(parents=True)
    entry = ExportPreset("Web 2048 px", ExportSettings(quality=10)).to_json()
    store.file.write_text('{"arraw": 1, "exportPresets": [' + entry + "]}")

    listed = [p for p in store.presets() if p.name == "Web 2048 px"]
    assert [p.builtin for p in listed] == [True, False]
    assert [p.shadowed for p in listed] == [False, True]
    assert store.load("web 2048 px").builtin
    store.rename("Web 2048 px", "Mine")  # the way out of the shadowing
    assert [p.name for p in store.presets() if p.shadowed] == []


def test_the_exception_is_a_runtime_error_with_a_problem(store):
    with pytest.raises(RuntimeError, match="built-in"):
        store.delete("Web 2048 px")
    assert issubclass(ExportPresetError, RuntimeError)


def test_settings_are_frozen_values():
    settings = ExportSettings(quality=50)
    assert settings.replace(quality=60).quality == 60
    assert settings == ExportSettings(quality=50)
    assert hash(settings) == hash(ExportSettings(quality=50))
    with pytest.raises(AttributeError):
        settings.quality = 1
    with pytest.raises(TypeError):
        ExportSettings(True)
    assert ExportSettings().develop_size is None
    assert ExportSettings(resize=True, width=800, height=600).develop_size == (800, 600)
    assert ExportSettings(sixteen_bit=True).canonical() == ExportSettings()
    with pytest.raises(ValueError):
        ExportSettings(sharpening=200).validate()


def test_the_store_sees_a_preset_the_cli_prints(cli, store, tmp_path):
    settings = ExportSettings(
        format=arraw.ImageFileFormat.TIFF,
        encoding=arraw.NamedEncoding.ADOBE_RGB,
        sixteen_bit=True,
        resize=True,
        width=3000,
        height=2000,
        allow_enlarging=True,
        sharpening=40,
        descriptive=False,
    )
    store.save(ExportPreset("Print", settings))

    shown = subprocess.run(
        [str(cli), "export-preset", "show", "print", "--config-dir", str(store.file.parent)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert shown.returncode == 0, shown.stderr
    assert shown.stdout == store.load("Print").to_json()

    listed = subprocess.run(
        [str(cli), "export-preset", "list", "--config-dir", str(store.file.parent)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert listed.stdout.splitlines() == [f"{name} (built-in)" for name in BUILT_INS] + ["Print"]


def test_the_cli_exports_with_a_preset_the_store_saved(cli, store, png, tmp_path):
    store.save(ExportPreset("Tiny", ExportSettings(resize=True, width=20, height=20)))
    out = tmp_path / "out"
    out.mkdir()
    done = subprocess.run(
        [str(cli), "export", "-q", "--config-dir", str(store.file.parent), "--export-preset",
         "tiny", "-o", str(out), str(png)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert done.returncode == 0, done.stderr
    (written,) = out.iterdir()
    assert arraw.read_metadata(written).size.width == 20


@pytest.fixture(scope="module")
def developed(dng):
    return arraw.develop(arraw.open(dng).with_(exposure=0.3))


def test_a_preset_drives_develop_and_save(dng, tmp_path):
    settings = ExportPresetStore(tmp_path / "config").load("Web 2048 px").settings
    photo = arraw.open(dng)
    image = arraw.develop(photo, size=settings.develop_size)
    out = tmp_path / "web.jpg"
    arraw.save(image, out, format=settings.format, quality=settings.quality,
               sharpening=settings.sharpening)
    assert out.is_file()
    size = arraw.read_metadata(out).size
    assert max(size.width, size.height) <= 2048
    assert settings.develop_size == (2048, 2048)


def test_save_has_no_settings_argument(developed, tmp_path):
    with pytest.raises(TypeError):
        arraw.save(developed, tmp_path / "x.jpg", settings=ExportSettings())


def test_save_keeps_its_defaults(developed, tmp_path):
    arraw.save(developed, tmp_path / "plain.png")
    arraw.save(developed, tmp_path / "explicit.png", encoding=arraw.NamedEncoding.SRGB,
               bit_depth=8, quality=90, sharpening=0, metadata=arraw.MetadataSelection())
    assert (tmp_path / "plain.png").read_bytes() == (tmp_path / "explicit.png").read_bytes()


def test_the_stub_shows_the_real_signature_of_save():
    from conftest import ROOT

    stub = (ROOT / "src" / "python" / "arraw" / "_arraw.pyi").read_text()
    (line,) = [l for l in stub.splitlines() if l.startswith("def save(")]
    assert "bit_depth: int = 8" in line
    assert "quality: int = 90" in line
    assert "settings" not in line
    assert "Optional" not in line
    assert "class ExportPresetError(RuntimeError)" in stub
