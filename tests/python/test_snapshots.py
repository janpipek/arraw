import datetime
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import pytest

import arraw


@pytest.fixture
def photo_path(dng, tmp_path):
    path = tmp_path / "IMG_1.dng"
    path.write_bytes(dng.read_bytes())
    return path


def exposed(value):
    return arraw.DevelopState(settings=arraw.DevelopState().settings.with_(exposure=value))


@pytest.fixture
def made(photo_path):
    """A session over a sidecar of two snapshots: 1 "Warm" (+1 EV) and 2 "Dark" (-1 EV, child of 1)."""
    session = arraw.EditSession(arraw.open_document(photo_path))
    session.set_state(exposed(1.0))
    first = session.create_snapshot("Warm")
    session.set_state(exposed(-1.0))
    second = session.create_snapshot("Dark")
    session.save()
    return session, first, second


def test_snapshot_id_is_a_checked_value():
    assert arraw.SnapshotId(3).value == 3
    assert int(arraw.SnapshotId(3)) == 3
    assert arraw.SnapshotId(3) == arraw.SnapshotId(3)
    assert arraw.SnapshotId(2) < arraw.SnapshotId(3)
    assert len({arraw.SnapshotId(3), arraw.SnapshotId(3)}) == 1
    assert repr(arraw.SnapshotId(3)) == "SnapshotId(3)"
    assert str(arraw.SnapshotId(3)) == "3"
    assert arraw.SnapshotId(2**64 - 1).value == 2**64 - 1
    for bad in (0, -1, 2**64):
        with pytest.raises(ValueError):
            arraw.SnapshotId(bad)
    for bad in (True, 1.0, "1", None):
        with pytest.raises(TypeError):
            arraw.SnapshotId(bad)


def test_open_document_of_a_photograph_without_a_sidecar(photo_path):
    document = arraw.open_document(photo_path)
    assert document.photo == arraw.open(photo_path)
    assert len(document.snapshots) == 0
    assert document.snapshots.entries == []
    assert document.snapshots.revision == 0
    assert document.snapshots.next_id == 1
    assert document.saved_base is None
    assert document.unreadable_snapshots == 0


def test_open_document_lists_what_the_sidecar_holds(made, photo_path):
    session, first, second = made
    document = arraw.open_document(photo_path)
    assert document.snapshots == session.snapshots
    assert document.saved_base == second
    assert document.photo == arraw.open(photo_path)

    warm, dark = document.snapshots.entries
    assert (warm.id, warm.name, warm.parent) == (first, "Warm", None)
    assert (dark.id, dark.name, dark.parent) == (second, "Dark", first)
    assert warm.state.settings.tone.exposure == pytest.approx(1.0)
    assert dark.state.settings.tone.exposure == pytest.approx(-1.0)
    assert warm.negative == document.photo.negative
    assert not warm.has_thumbnail
    assert warm.created.tzinfo == datetime.timezone.utc
    assert abs(datetime.datetime.now(datetime.timezone.utc) - warm.created) < datetime.timedelta(minutes=5)
    assert warm.created.microsecond == 0

    assert len(document.snapshots) == 2
    assert [s.id for s in document.snapshots] == [first, second]
    assert first in document.snapshots
    assert arraw.SnapshotId(9) not in document.snapshots
    assert document.snapshots.find(second) == dark
    assert document.snapshots.find(arraw.SnapshotId(9)) is None
    assert document.snapshots.next_id == 3
    assert document.snapshots.revision == 2
    assert "Warm" in repr(warm)
    with pytest.raises(TypeError):
        hash(warm)


def test_session_from_a_document_carries_the_base(made, photo_path):
    _, first, second = made
    session = arraw.EditSession(arraw.open_document(photo_path))
    assert session.base_snapshot == second
    assert session.saved_base_snapshot == second
    assert session.history[0].base_snapshot == second
    assert [s.id for s in session.snapshots] == [first, second]
    assert session.unreadable_snapshots == 0
    assert not session.has_unsaved_changes


def test_session_from_a_photo_knows_no_snapshots(photo_path):
    session = arraw.EditSession(arraw.open(photo_path))
    assert len(session.snapshots) == 0
    assert session.base_snapshot is None


def test_create_anchors_the_base_and_adds_no_step(photo_path):
    session = arraw.EditSession(arraw.open_document(photo_path))
    session.set_state(exposed(0.5))
    steps = len(session.history)
    created = session.create_snapshot("Half")
    assert created == arraw.SnapshotId(1)
    assert len(session.history) == steps
    assert session.base_snapshot == created
    assert session.saved_base_snapshot is None
    assert session.has_unsaved_changes  # the base changed
    assert session.snapshots.find(created).name == "Half"
    assert arraw.open_document(photo_path).snapshots.find(created).name == "Half"
    # Only the snapshot was written: the saved settings on disk are still the defaults.
    assert arraw.open(photo_path).state == arraw.default_state(arraw.open(photo_path).metadata)


def test_restore_is_one_undoable_step(made):
    session, first, second = made
    steps = len(session.history)
    session.restore_snapshot(first)
    assert len(session.history) == steps + 1
    assert session.history[-1].origin == arraw.EditOrigin.RESTORE
    assert session.history[-1].detail == "Warm"
    assert session.history[-1].base_snapshot == first
    assert session.base_snapshot == first
    assert session.photo.state == session.snapshots.find(first).state
    assert session.photo.negative == session.snapshots.find(first).negative
    session.undo()
    assert session.base_snapshot == second
    assert session.photo.state.settings.tone.exposure == pytest.approx(-1.0)
    with pytest.raises(ValueError):
        session.restore_snapshot(arraw.SnapshotId(99))


def test_rename_writes_the_sidecar_and_is_no_step(made, photo_path):
    session, first, _ = made
    steps = len(session.history)
    session.rename_snapshot(first, "Warmer")
    assert len(session.history) == steps
    assert session.snapshots.find(first).name == "Warmer"
    assert arraw.open_document(photo_path).snapshots.find(first).name == "Warmer"
    for bad in ("", "x" * 257):
        with pytest.raises(ValueError):
            session.rename_snapshot(first, bad)
    with pytest.raises(ValueError):
        session.rename_snapshot(arraw.SnapshotId(99), "Nope")


def test_delete_remaps_children_and_bases(made, photo_path):
    session, first, second = made
    session.delete_snapshot(second)
    assert session.snapshots.find(second) is None
    assert session.base_snapshot == first
    assert session.saved_base_snapshot == first
    assert [s.id for s in arraw.open_document(photo_path).snapshots] == [first]
    # An ID is never reused.
    assert session.create_snapshot("Again") == arraw.SnapshotId(3)
    session.delete_snapshot(first)
    assert session.snapshots.find(arraw.SnapshotId(3)).parent is None
    with pytest.raises(ValueError):
        session.delete_snapshot(first)


def test_delete_is_not_undone(made):
    session, _, second = made
    session.delete_snapshot(second)
    while session.can_undo:
        session.undo()
    assert session.snapshots.find(second) is None


def test_names_are_validated(photo_path):
    session = arraw.EditSession(arraw.open_document(photo_path))
    for bad in ("", "x" * 257):
        with pytest.raises(ValueError):
            session.create_snapshot(bad)
    assert len(session.snapshots) == 0


def test_the_list_is_bounded(photo_path):
    session = arraw.EditSession(arraw.open_document(photo_path))
    for index in range(32):
        session.create_snapshot(f"S{index}")
    with pytest.raises(ValueError):
        session.create_snapshot("one too many")
    assert len(session.snapshots) == 32


def test_save_keeps_the_base(made, photo_path):
    session, first, _ = made
    session.restore_snapshot(first)
    assert session.has_unsaved_changes
    session.save()
    assert arraw.open_document(photo_path).saved_base == first
    assert not session.has_unsaved_changes


def test_revision_conflict_reads_again_and_names_both_revisions(made, photo_path):
    session, first, second = made
    other = arraw.EditSession(arraw.open_document(photo_path))
    added = other.create_snapshot("Theirs")

    with pytest.raises(arraw.SnapshotRevisionConflict) as caught:
        session.create_snapshot("Mine")
    assert isinstance(caught.value, RuntimeError)
    assert caught.value.expected == 2
    assert caught.value.actual == 3
    # The session read the collection again; nothing of its own was written.
    assert session.snapshots.find(added).name == "Theirs"
    assert not any(s.name == "Mine" for s in session.snapshots)

    # The retry is the caller's.
    assert session.create_snapshot("Mine").value == added.value + 1


def test_every_writer_meets_a_revision_conflict(made, photo_path):
    _, first, _ = made
    for operation in (
        lambda s: s.rename_snapshot(first, "x"),
        lambda s: s.delete_snapshot(first),
        lambda s: s.create_snapshot("x"),
    ):
        stale = arraw.EditSession(arraw.open_document(photo_path))
        arraw.EditSession(arraw.open_document(photo_path)).create_snapshot("bump")
        with pytest.raises(arraw.SnapshotRevisionConflict):
            operation(stale)


def test_save_remaps_a_base_another_process_deleted(made, photo_path):
    session, first, second = made
    arraw.EditSession(arraw.open_document(photo_path)).delete_snapshot(second)
    session.restore_snapshot(first)
    session.restore_snapshot(second)  # still known to this session
    session.save()  # reads the list again and falls back to the nearest survivor
    assert session.base_snapshot == first
    assert arraw.open_document(photo_path).saved_base == first


def test_write_sidecar_records_a_base_or_refuses_a_missing_one(made, photo_path):
    _, first, _ = made
    photo = arraw.open(photo_path)
    arraw.write_sidecar(photo, first)
    assert arraw.open_document(photo_path).saved_base == first
    arraw.write_sidecar(photo, None)
    assert arraw.open_document(photo_path).saved_base is None

    sidecar = Path(arraw.sidecar_path(photo_path))
    before = sidecar.read_bytes()
    with pytest.raises(arraw.SnapshotBaseMissing) as caught:
        arraw.write_sidecar(photo, arraw.SnapshotId(99))
    assert isinstance(caught.value, ValueError)
    assert caught.value.base == arraw.SnapshotId(99)
    assert sidecar.read_bytes() == before


def damage(photo_path):
    sidecar = Path(arraw.sidecar_path(photo_path))
    text = sidecar.read_text(encoding="utf-8")
    end = text.index("</arraw:snapshots>")
    seq = text.rindex("</rdf:Seq>", 0, end)
    bad = (
        '<rdf:li rdf:parseType="Resource"><arraw:id>9</arraw:id><arraw:name>Bad</arraw:name>'
        "<arraw:created>2026-10-23T12:00:00Z</arraw:created><arraw:state>{ not json</arraw:state>"
        "<arraw:negative>{}</arraw:negative></rdf:li>"
    )
    sidecar.write_text(text[:seq] + bad + text[seq:], encoding="utf-8")


def test_unreadable_entries_are_counted_and_discarded(made, photo_path):
    _, first, second = made
    damage(photo_path)
    document = arraw.open_document(photo_path)
    assert document.unreadable_snapshots == 1
    assert [s.id for s in document.snapshots] == [first, second]

    session = arraw.EditSession(document)
    assert session.unreadable_snapshots == 1
    session.discard_unreadable_snapshots()
    assert session.unreadable_snapshots == 0
    assert [s.id for s in session.snapshots] == [first, second]
    after = arraw.open_document(photo_path)
    assert after.unreadable_snapshots == 0
    assert [s.id for s in after.snapshots] == [first, second]

    # Nothing unusable: nothing is written.
    sidecar = Path(arraw.sidecar_path(photo_path))
    before = sidecar.read_bytes()
    session.discard_unreadable_snapshots()
    assert sidecar.read_bytes() == before


def hold_lock(photo_path):
    """Holds the sidecar's lock file as a running process would, until the process is stopped."""
    holder = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    lock = Path(str(arraw.sidecar_path(photo_path)) + ".lock")
    name = Path(sys.executable).resolve().name
    boot = Path("/proc/sys/kernel/random/boot_id")
    machine = Path("/etc/machine-id")
    fields = [
        str(holder.pid),
        name,
        socket.gethostname(),
        machine.read_text().strip() if machine.exists() else "",
        boot.read_text().strip() if boot.exists() else "",
    ]
    lock.write_text("\n".join(fields) + "\n")
    return holder, lock


@pytest.mark.skipif(sys.platform != "linux", reason="forges a Qt lock file as Linux writes it")
def test_a_held_lock_times_out_with_its_own_error(made, photo_path):
    session, first, _ = made
    sidecar = Path(arraw.sidecar_path(photo_path))
    before = sidecar.read_bytes()
    holder, lock = hold_lock(photo_path)
    try:
        started = time.monotonic()
        with pytest.raises(arraw.SidecarLockTimeout) as caught:
            session.rename_snapshot(first, "Blocked")
        waited = time.monotonic() - started
        assert 1.5 <= waited < 30  # it waited for the lock, not refused at once
        assert isinstance(caught.value, RuntimeError)
        assert str(sidecar.name) in str(caught.value)
        assert sidecar.read_bytes() == before
        assert session.snapshots.find(first).name == "Warm"
    finally:
        holder.kill()
        holder.wait()
        lock.unlink(missing_ok=True)
    session.rename_snapshot(first, "Warm again")
    assert session.snapshots.find(first).name == "Warm again"


@pytest.mark.skipif(sys.platform != "linux", reason="forges a Qt lock file as Linux writes it")
def test_a_held_lock_times_out_every_write(made, photo_path):
    session, first, _ = made
    holder, lock = hold_lock(photo_path)
    try:
        with pytest.raises(arraw.SidecarLockTimeout):
            session.create_snapshot("Blocked")
        with pytest.raises(arraw.SidecarLockTimeout):
            session.delete_snapshot(first)
    finally:
        holder.kill()
        holder.wait()
        lock.unlink(missing_ok=True)
    assert len(session.snapshots) == 2


def test_snapshot_ids_are_not_ints_to_the_api(made):
    session, first, _ = made
    assert arraw.SnapshotId(2) <= arraw.SnapshotId(2) <= arraw.SnapshotId(3)
    assert arraw.SnapshotId(3) >= arraw.SnapshotId(2)
    assert first in session.snapshots
    assert 1 not in session.snapshots
    assert "1" not in session.snapshots
    assert None not in session.snapshots
    with pytest.raises(TypeError):
        session.snapshots.find(1)
    with pytest.raises(TypeError):
        session.restore_snapshot(1)
    with pytest.raises(TypeError):
        session.rename_snapshot(1, "x")
    with pytest.raises(TypeError):
        session.delete_snapshot(1)
    assert [int(s.id) for s in session.snapshots] == [1, 2]


def test_a_session_from_a_photo_refreshes_before_changing_snapshots(made, photo_path):
    stale = arraw.EditSession(arraw.open(photo_path))
    assert len(stale.snapshots) == 0
    with pytest.raises(arraw.SnapshotRevisionConflict):
        stale.create_snapshot("Late")
    stale.refresh_snapshots()
    assert len(stale.snapshots) == 2
    third = stale.create_snapshot("Late")
    assert stale.snapshots.find(third).name == "Late"


def test_with_snapshot_copies_the_recipe_exactly(made, photo_path):
    session, first, _ = made
    photo = arraw.open(photo_path).with_(rating=3)
    snapshot = session.snapshots.find(first)
    made_photo = photo.with_snapshot(snapshot)
    assert made_photo.path == photo.path
    assert made_photo.marks == photo.marks
    assert made_photo.state == snapshot.state
    assert made_photo.negative == snapshot.negative
    assert photo.state != snapshot.state  # the original is left alone


def export_with_cli(cli, source, out_dir, *flags):
    out_dir.mkdir(exist_ok=True)
    result = subprocess.run(
        [str(cli), "export", "--quiet", "--device", "cpu", "--format", "png", "--bit-depth", "16",
         "-o", str(out_dir), *flags, str(source)],
        capture_output=True, text=True, timeout=120,
    )
    return result, out_dir / (Path(source).stem + ".png")


@pytest.mark.parametrize("which", [1, 2])
def test_cli_snapshot_export_equals_python_rendering(made, cli, photo_path, tmp_path, which):
    session, _, _ = made
    snapshot = session.snapshots.find(arraw.SnapshotId(which))
    rendered = arraw.develop(arraw.open(photo_path).with_snapshot(snapshot))
    py_out = tmp_path / "py.png"
    arraw.save(rendered, py_out, bit_depth=16)

    result, cli_out = export_with_cli(cli, photo_path, tmp_path / "cli", "--snapshot", str(which))
    assert result.returncode == 0, result.stderr
    py_px = arraw.load(py_out).pixels.astype(np.int64)
    cli_px = arraw.load(cli_out).pixels.astype(np.int64)
    assert py_px.shape == cli_px.shape
    assert np.array_equal(py_px, cli_px)


def test_cli_snapshots_differ_from_each_other(made, cli, photo_path, tmp_path):
    first, one = export_with_cli(cli, photo_path, tmp_path / "a", "--snapshot", "1")
    second, two = export_with_cli(cli, photo_path, tmp_path / "b", "--snapshot", "2")
    assert first.returncode == second.returncode == 0
    assert not np.array_equal(arraw.load(one).pixels, arraw.load(two).pixels)


def test_cli_info_agrees_with_python(made, cli, photo_path):
    import json

    session, _, second = made
    result = subprocess.run([str(cli), "info", "--json", str(photo_path)], capture_output=True,
                            text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    (info,) = json.loads(result.stdout)["files"]
    assert info["savedBase"] == str(second)
    assert info["unreadableSnapshots"] == 0
    listed = info["snapshots"]
    assert len(listed) == len(session.snapshots)
    for entry, snapshot in zip(listed, session.snapshots):
        assert entry["id"] == str(snapshot.id)
        assert entry["parent"] == (None if snapshot.parent is None else str(snapshot.parent))
        assert entry["name"] == snapshot.name
        assert entry["created"] == snapshot.created.strftime("%Y-%m-%dT%H:%M:%SZ")


def test_cli_snapshot_export_keeps_the_snapshot_negative(bayer_detail_dng, cli, tmp_path):
    path = tmp_path / "IMG_1.dng"
    path.write_bytes(bayer_detail_dng.read_bytes())
    photo = arraw.open(path)
    vng = photo.negative.replace(demosaic=arraw.Demosaic.VNG)
    session = arraw.EditSession(arraw.open_document(path))
    session.set_negative(vng)
    first = session.create_snapshot("Vng")
    session.set_negative(photo.negative)
    session.save()
    snapshot = session.snapshots.find(first)
    assert snapshot.negative == vng and snapshot.negative != session.photo.negative

    rendered = arraw.develop(arraw.open(path).with_snapshot(snapshot))
    py_out = tmp_path / "py.png"
    arraw.save(rendered, py_out, bit_depth=16)
    result, cli_out = export_with_cli(cli, path, tmp_path / "cli", "--snapshot", "1")
    assert result.returncode == 0, result.stderr
    assert np.array_equal(arraw.load(py_out).pixels, arraw.load(cli_out).pixels)
    saved, saved_out = export_with_cli(cli, path, tmp_path / "saved")
    assert not np.array_equal(arraw.load(saved_out).pixels, arraw.load(cli_out).pixels)
