# An edit is begun, updated and committed as one step

The [feature brief](../desired-features.md) asks for one undo stack, with a
slider drag counting as a single step rather than a hundred. A drag still has
to show every value as it moves, so the document changes many times while
history should change once. [ADR 021](021-a-photograph-is-developed-from-a-develop-state.md)
made `DevelopState` the unit history stores. This ADR records how
`EditSession` turns changes into steps.

## Decision

**An edit has three phases.** `begin()` remembers the state the edit starts
from. `update(state)` replaces the state provisionally, any number of times.
`commit()` closes the edit as one step, or `cancel()` restores the start.
`setState(state)` is all three at once, for changes that happen at once: a
reset, a choice from a list, a keyboard step.

**What to render is always `photo()`.** During an edit it already carries the
latest update, so the renderer never needs to know whether an edit is open.

**History is two stacks of whole `DevelopState` values.** Undo holds the state
before each step, and redo the state after each step undone. Rules:
- an edit that ends where it began leaves no step;
- a new step clears redo;
- there is no depth limit, since a state holds no pixels and, under ADR 021,
  its large payloads are shared.

**Steps carry no labels.** A history panel can name a step ("Exposure +0.50")
by comparing it with its neighbour through the descriptor table. A stored label
could disagree with the states it describes.

**Misuse is a caller's bug, and it throws `std::logic_error`.** This covers
`update`, `commit` or `cancel` with no edit open, and `undo` or `redo` with
nothing to take back. The GUI greys out its actions through `canUndo()` and
`canRedo()`.

**An invalid state changes nothing.** `update` and `setState` throw
`std::invalid_argument`, as `Photo` does. An edit that was open stays open with
its last valid state, and `setState` opens no edit at all.

**Interleaving resolves towards keeping work:**
- `begin()` while an edit is open commits that edit first.
- `undo()` during an edit commits it, then undoes it. Ctrl+Z in the middle of a
  drag therefore reverts the drag, and redo brings it back.
- `redo()` during an edit commits it first. If the edit changed something, that
  clears redo, so `canRedo()` already says no.

**Marks are not in history.** Only develop states are stored, so an undo never
changes a rating or label (ADR 021).

**The session sends no notifications.** It is a library type with no Qt in its
interface. The GUI calls it, then refreshes the panel from `photo()` and
renders.

**Python does not bind `EditSession` yet.** Scripts work on `Photo` values,
and no Python caller needs history. It will be bound when scripting an open GUI
or batch undo arrives, probably with a context-manager form for an edit.

## Alternatives

- **Commands instead of states.** Each step would hold a reversible operation.
  That pays off when states are large. They are not, and large payloads are
  shared, so snapshots are simpler and cannot drift from what they undo.
- **`QUndoStack`.** It is command-based, and Qt-specific in the public API. It
  would also keep history out of reach of the command line and Python.
- **`update()` beginning an edit implicitly.** More forgiving, but a forgotten
  `begin()` would merge unrelated edits silently, or leave one open forever.

## Consequences

- Every control in the GUI reports a start, changes and an end, even where the
  start and end coincide. That shape suits sliders, brush strokes and handle
  drags alike.
- Unsaved-change tracking, a baseline compared against `photo().state()`, fits
  on top without changing the protocol.
- Batch undo across several photographs, and what happens when a sidecar
  changes on disk during a session, are still open.
