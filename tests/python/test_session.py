import pytest

import arraw


@pytest.fixture
def session(dng, tmp_path):
    photo = arraw.open(dng, sidecar=False)
    return arraw.EditSession(photo)


def state_with(session, **kwargs):
    return session.photo.with_(**kwargs).state


def test_opens_with_one_opened_step(session):
    assert len(session.history) == 1
    assert session.history[0].origin == arraw.EditOrigin.OPENED
    assert session.history[0].detail == ""
    assert session.history[0].state == session.photo.state
    assert session.position == 0
    assert not session.editing
    assert not session.can_undo and not session.can_redo


def test_edit_cycle_adds_one_step(session):
    session.begin()
    assert session.editing
    for value in (0.1, 0.2, 0.3):
        session.update(state_with(session, exposure=value))
    session.commit()
    assert not session.editing
    assert len(session.history) == 2
    assert session.position == 1
    assert session.photo.state.settings.tone.exposure == pytest.approx(0.3)
    assert session.history[1].origin == arraw.EditOrigin.EDIT


def test_edit_calls_without_open_edit_are_runtime_errors(session):
    with pytest.raises(RuntimeError):
        session.update(session.photo.state)
    with pytest.raises(RuntimeError):
        session.commit()
    with pytest.raises(RuntimeError):
        session.cancel()


def test_cancel_restores(session):
    before = session.photo.state
    session.begin()
    session.update(state_with(session, exposure=0.5))
    session.cancel()
    assert session.photo.state == before
    assert len(session.history) == 1


def test_edit_that_changes_nothing_leaves_no_step(session):
    session.begin()
    session.commit()
    assert len(session.history) == 1


def test_set_state_with_origin_and_detail(session):
    session.set_state(state_with(session, exposure=0.4), arraw.EditOrigin.PRESET, "Warm")
    step = session.history[-1]
    assert step.origin == arraw.EditOrigin.PRESET
    assert step.detail == "Warm"
    session.set_state(state_with(session, exposure=0.9), origin=arraw.EditOrigin.PASTE)
    assert session.history[-1].origin == arraw.EditOrigin.PASTE
    assert session.history[-1].detail == ""


def test_commit_origin_and_detail(session):
    session.begin()
    session.update(state_with(session, exposure=0.2))
    session.commit(arraw.EditOrigin.CROP, "x")
    assert session.history[1].origin == arraw.EditOrigin.CROP
    assert session.history[1].detail == "x"


def test_history_step_is_read_only(session):
    step = session.history[0]
    with pytest.raises(AttributeError):
        step.detail = "nope"


def make_steps(session, count):
    for index in range(1, count + 1):
        session.set_state(state_with(session, exposure=index / 10))


def test_go_to_navigates_without_dropping(session):
    make_steps(session, 3)
    session.go_to(1)
    assert session.position == 1
    assert len(session.history) == 4
    assert session.photo.state == session.history[1].state
    session.go_to(3)
    assert session.position == 3
    assert session.photo.state == session.history[3].state


def test_edit_after_go_to_drops_later_steps(session):
    make_steps(session, 3)
    session.go_to(1)
    session.set_state(state_with(session, exposure=-0.5))
    assert len(session.history) == 3
    assert session.position == 2
    assert not session.can_redo


def test_go_to_out_of_range_is_index_error(session):
    with pytest.raises(IndexError):
        session.go_to(1)
    session.set_state(state_with(session, exposure=0.1))
    with pytest.raises(IndexError):
        session.go_to(2)


def test_go_to_commits_open_edit_first(session):
    make_steps(session, 2)
    session.go_to(1)
    session.begin()
    session.update(state_with(session, exposure=0.77))
    # the commit drops step 2, so history has 3 entries and index 2 is the new step
    session.go_to(2)
    assert len(session.history) == 3
    assert session.position == 2
    assert session.photo.state.settings.tone.exposure == pytest.approx(0.77)
    with pytest.raises(IndexError):
        session.go_to(3)


def test_undo_redo(session):
    make_steps(session, 2)
    assert session.can_undo and not session.can_redo
    session.undo()
    assert session.position == 1
    assert session.can_redo
    session.redo()
    assert session.position == 2
    session.go_to(0)
    with pytest.raises(RuntimeError):
        session.undo()
    session.go_to(2)
    with pytest.raises(RuntimeError):
        session.redo()


def test_undo_in_open_edit_reverts_it(session):
    session.set_state(state_with(session, exposure=0.1))
    session.begin()
    session.update(state_with(session, exposure=0.6))
    assert session.can_undo
    session.undo()
    assert session.photo.state.settings.tone.exposure == pytest.approx(0.1)
    assert session.can_redo


def test_context_manager_commits(session):
    with session.edit():
        assert session.editing
        session.update(state_with(session, exposure=0.3))
    assert not session.editing
    assert len(session.history) == 2
    assert session.history[1].origin == arraw.EditOrigin.EDIT


def test_context_manager_origin_and_detail(session):
    with session.edit(origin=arraw.EditOrigin.RESET, detail="d"):
        session.update(state_with(session, exposure=0.3))
    assert session.history[1].origin == arraw.EditOrigin.RESET
    assert session.history[1].detail == "d"


def test_context_manager_cancels_on_exception(session):
    before = session.photo.state
    with pytest.raises(KeyError):
        with session.edit():
            session.update(state_with(session, exposure=0.3))
            raise KeyError("boom")
    assert not session.editing
    assert session.photo.state == before
    assert len(session.history) == 1


def test_context_manager_tolerates_closed_edit(session):
    with session.edit():
        session.update(state_with(session, exposure=0.3))
        session.commit()
    assert len(session.history) == 2


def test_context_manager_refuses_to_nest(session):
    with session.edit():
        session.update(state_with(session, exposure=0.3))
        with pytest.raises(RuntimeError):
            with session.edit():
                pass  # pragma: no cover
        # The outer edit is still open, with its change.
        assert session.editing
    assert len(session.history) == 2


def test_context_manager_exit_after_edit_closed_does_nothing(session):
    with session.edit(origin=arraw.EditOrigin.RESET):
        session.update(state_with(session, exposure=0.3))
        session.commit()
        session.begin()
        session.update(state_with(session, exposure=0.4))
        session.cancel()
    assert len(session.history) == 2
    assert session.history[1].origin == arraw.EditOrigin.EDIT


def test_describe_change(session):
    base = session.photo.state
    assert arraw.describe_change(base, base).keys == []
    assert arraw.describe_change(base, base).group is None
    one = arraw.describe_change(base, state_with(session, exposure=0.5))
    assert one.keys == ["exposure"]
    assert one.group == arraw.SettingGroup.TONE
    two = arraw.describe_change(base, state_with(session, exposure=0.5, contrast=0.2))
    assert len(two.keys) == 2
    assert two.group == arraw.SettingGroup.TONE
    mixed = arraw.describe_change(base, state_with(session, exposure=0.5, saturation=0.2))
    assert mixed.group is None
    assert len(mixed.keys) == 2


def test_save_and_unsaved_changes(dng, tmp_path):
    copy = tmp_path / dng.name
    copy.write_bytes(dng.read_bytes())
    session = arraw.EditSession(arraw.open(copy))
    assert not session.has_unsaved_changes
    session.set_state(state_with(session, exposure=0.8))
    assert session.has_unsaved_changes
    assert session.saved.state != session.photo.state
    session.save()
    assert not session.has_unsaved_changes
    assert len(session.history) == 2
    assert arraw.sidecar_path(copy).exists()
    assert arraw.open(copy).state.settings.tone.exposure == pytest.approx(0.8)


def test_set_marks_writes_at_once(dng, tmp_path):
    copy = tmp_path / dng.name
    copy.write_bytes(dng.read_bytes())
    session = arraw.EditSession(arraw.open(copy))
    session.set_marks(arraw.PhotoMarks(rating=3))
    assert session.photo.marks.rating == 3
    assert arraw.open(copy).marks.rating == 3
    assert len(session.history) == 1
    with pytest.raises(ValueError):
        session.set_marks(arraw.PhotoMarks(rating=9))


def test_discard_changes(session):
    make_steps(session, 2)
    saved = session.saved.state
    session.discard_changes()
    assert session.photo.state == saved
    assert len(session.history) == 1
    assert session.history[0].origin == arraw.EditOrigin.OPENED
    assert session.position == 0
