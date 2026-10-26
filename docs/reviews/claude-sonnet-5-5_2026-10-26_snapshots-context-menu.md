# Review: snapshot tree context menu (uncommitted working tree)

Reviewer: Claude Sonnet 5.5, 2026-10-26. Scope: the uncommitted diff of `src/app/ui/MainWindow.{h,cpp}`,
`tests/test_MainWindowSnapshots.cpp` and `docs/keybindings.md` that adds a right-click and menu-key
context menu to the snapshot tree, read against
[`snapshots-gui-plan.md`](../ideas/snapshots-gui-plan.md) and
[ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md).
Lens: the `contextSnapshot_` lend (leaks, stale targets, re-entrancy), preview and session left
alone by merely opening the menu, parity of enabled state with the existing actions, keyboard menu
positioning, whether the tests can fail, CLAUDE.md style.

I read the diff and the surrounding code (`updateSnapshotPanel`, `restoreSnapshot`,
`renameSelectedSnapshot`, `deleteSelectedSnapshot`, `refreshSnapshotModel`, the selection handler,
`EditSession::restoreSnapshot`). I did not build or run anything; "would still pass" below is by
reading, not by mutation. Qt behaviours cited from memory are marked as such.

**Counts:** medium 1, low 5, nit 3.

Summary: the design holds up. `exec()` always returns, so on the normal paths the lend is reset;
opening the menu starts no preview and touches no session state (`updateSnapshotPanel` only sets
action and widget enabled states); the actions are the window's own, so enabled state during a
preview or an in-flight write is identical to the "..." button menu; and the tests are mostly
discriminating (Rename and Restore would visibly fail without the lend). The real hole is that the
lend is a bare snapshot id that is not tied to the photograph, and ids restart at 1 per photograph.
The rest is robustness and test gaps.

---

## Medium

### M1. The lent id outlives the photograph it was lent for, and ids collide across photographs

`src/app/ui/MainWindow.cpp:1620` (set), `:1603` (read), `:1634` (reset); `include/Snapshots.h:93`
(`nextId = 1`).

`contextSnapshot_` is an `std::optional<SnapshotId>`, with no session serial. The menu is a nested
event loop, and the photograph can change under it with no user input: an async load finishing, a
folder-watch removal, a second instance handing over a file. When `open_` is replaced,
`updateHistoryDock` / `updateSnapshotPanel` run with the lend still set, so `selected` is true and
the three row actions are enabled for the new photograph. The open menu is not closed. Clicking
Restore then calls `restoreSnapshot(1)` on whatever snapshot 1 is in the new photograph (ids start
at 1 per photograph), with no confirmation. Rename and Delete resolve the id the same way; Delete
at least names the wrong snapshot in its confirmation. Scenario: arrow-key to the next photo, then
right-click a row before the load lands; the load lands while the menu is open; the user picks
Restore on a menu that was drawn for the previous photograph.

Fix options. (a) Store `{serial, id}` and have `selectedSnapshot()` return the lent id only while
`open_ && open_->serial == serial`, else fall through to the selection. (b) Keep a
`QPointer<QMenu>` member and close it from the photo-change path (same place `open_` is reset,
`:3906`). (a) is smaller and also covers the "row deleted while open" case if it additionally
requires `snapshots().find(id) != nullptr`. Either way the menu should not offer entries for a
photograph that is gone.

## Low

### L1. The lent id is not validated against the collection, so the enabled state can be wrong

`src/app/ui/MainWindow.cpp:1603`, `:1683` (`selected` in `updateSnapshotPanel`).

While the menu (or the dialog it launched) is up, a write result can remove the target: a conflict
adopting a fresh collection from another program, or a result from an earlier write.
`selectedSnapshot()` still returns the id, so Restore, Rename and Delete stay enabled for a row
that no longer exists. Triggering them is safe (`find` returns `nullptr` and `renameSelectedSnapshot`
and `deleteSelectedSnapshot` return quietly; `EditSession::restoreSnapshot` throws
`invalid_argument`, which `guarded` presumably reports), but the user gets a silent no-op or an
error box instead of a disabled entry. Same remedy as M1(a).

### L2. The reset after `exec()` is not exception-safe and not guarded against destruction

`src/app/ui/MainWindow.cpp:1633-1635`.

`contextSnapshot_.reset()` is the only thing that ends the lend. Normal and early closes (Esc,
click elsewhere, focus loss) all return from `exec()` and are fine. Two ways to skip it: an
exception unwinding out of `exec()` (the actions catch their own, so not today), and the window
being destroyed while the menu is open, after which the line writes to a dead `this`. The latter is
the same exposure the existing `QMessageBox::exec` paths have, so it is consistent with the code
base. A `QScopeGuard` (or a `QPointer<MainWindow>` check) makes both cases cheap to close.
Re-entrancy: a second `showSnapshotContextMenu` while one is open is not reachable (the popup grabs
the pointer), but if it ever were, the inner reset would clear the outer lend; an early
`if (contextSnapshot_ || menuOpen) return;` would document the assumption.

### L3. The keyboard path positions the menu from `visualRect` without scrolling to the row

`src/app/ui/MainWindow.cpp:416-422`.

`visualRect(rows.first())` is relative to the current scroll offset. The selected row is scrolled
into view when the selection is set by the window (`syncSnapshotSelection` calls `scrollTo`), but
the user can wheel-scroll afterwards, then press the menu key: the rectangle then lies above or
below the viewport, `mapToGlobal` puts the menu outside the panel, and Qt may clamp it to a screen
edge far from the tree. Call `snapshotTree_->scrollTo(index)` first, or clamp `where` into
`viewport()->rect()`. A second, cosmetic point: the anchor is the row centre (the row is the full
panel width), so the menu opens over the row it is about; the usual anchor is the row's bottom-left
(`visualRect(...).bottomLeft()`), which keeps the row and its thumbnail visible.

### L4. No guard that the panel is enabled before opening the menu

`src/app/ui/MainWindow.cpp:1613-1617`.

`showSnapshotContextMenu` checks only `open_`. During the crop and mask modes (and while there are
no pixels) `updateSnapshotPanel` disables the panel and the actions, and Qt does not deliver
`customContextMenuRequested` for a disabled tree, so the mouse path is safe. The keyboard path goes
through the event filter, which runs before `QWidget::event`'s disabled check; it is protected only
because a disabled widget cannot hold the focus. That is two implicit assumptions for a menu whose
whole content is destructive actions. `if (!open_ || !snapshotPanel_->isEnabled()) return;` states
it. The menu would have shown disabled entries anyway, so this is defence in depth.

### L5. The right-press swallow leaves the target row with no visible mark

`src/app/ui/MainWindow.cpp:405-411`, `docs/keybindings.md:67`.

Because the right-press selects nothing (the intended design: selecting previews), nothing shows
which row the menu is about, and with a different row previewed the highlight, the banner
("Previewing X") and the menu target (Y) disagree. Restore on Y then changes the look to Y while the
banner names X. The test `while another snapshot is previewed ...` pins this behaviour but not its
presentation. Options: paint the target with a transient `setCurrentIndex` that does not change the
selection (`selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate)` gives the
focus rectangle without selecting or previewing), or put the snapshot name in the menu entries
("Restore "Two""). Not a bug; a usability gap worth deciding on before it is documented as
"a right-click previews nothing".

## Nits

### N1. Documentation drift

- `docs/keybindings.md:67` says "all snapshot actions"; on the empty space only New (and Remove
  Unreadable Entries when there are any) is offered, and Remove Unreadable is hidden otherwise,
  unlike the "..." button menu where it is always listed and disabled. Say "the snapshot actions
  that fit the row".
- `snapshots-gui-plan.md:269` describes the header row and its menu button only; the context menu is
  an addition the plan and ADR 056 do not mention. A line in the plan's panel section would keep
  the next reader from treating it as undecided.
- The `Menu` key is documented; `Shift+F10` is the same event on most platforms and is not
  mentioned (harmless).

### N2. Test helpers can crash or hang instead of failing

`tests/test_MainWindowSnapshots.cpp:125-170`.

- `choosing()` ends in `FAIL(...)`, which throws from inside a `QTimer` slot. Qt does not allow
  exceptions out of slots, so a missing entry aborts the whole test binary rather than reporting
  one failed case. (`clicking()` above it has the same pattern, so this is consistent but not
  better.) Setting a flag the section can `REQUIRE` after `exec()` returns avoids it.
- `MenuProbe` polls forever for a popup. If the menu opens but is never a `QMenu` active popup (or
  never closes), `contextMenuAt` blocks in `exec()` with no timeout. A `QTimer::singleShot` safety
  close, or a deadline in the probe, turns that into a failure.
- The helper structs are documented as "Watches ...", "Triggers ...": verb briefs on a struct, where
  CLAUDE.md asks for noun forms for classes ("Watcher of the next popup menu ..."). `Answer` above
  has the same form, so it follows the file.

### N3. Style check

Clean otherwise. Include groups and order are right (`QContextMenuEvent`, `QMouseEvent`, `QMap`,
`QStringList` are in alphabetical position within the Qt group), comments use `///`, the new method
brief is a verb form, the new field brief is a noun form. The `eventFilter` branch evaluates
`snapshotTree_->viewport()` for every event of every watched object before looking at the event
type; testing `event->type()` first is cheaper and avoids touching `snapshotTree_` while it is being
torn down. The `where(8, 8)` literal deserves a name or comment ("near the top-left of an empty
tree").

---

## Test assessment

What would fail if the behaviour broke:

- **Lend removed.** `Rename renames the clicked row, not the selected one` (One previewed, Two
  clicked, expects "Two" renamed) fails; `Restore makes the clicked look ...` fails
  (`restoreSnapshotAction_` would be disabled with no selection, so `trigger()` does nothing, so
  `steps` stays); `Delete ... deletes the clicked row` fails. These are not vacuous.
- **Lend not reset.** Covered only partially: after the menu on a row with an empty selection, the
  test checks Restore and Rename are disabled again, which catches a missing reset. It does not
  cover the reset after an entry was *triggered* (the early-return/dialog paths), where the
  assertion would be "after Rename of Two with One selected, Rename targets One again". Add one
  check after the trigger sections.
- **Right-press swallowed.** `a right-press selects nothing ...` fails without the filter, assuming
  `QTest::mouseClick` goes through `QApplication::notify` and so through event filters (I believe it
  does for widgets). It does not prove a real right-press does not preview on a real platform, where
  the press is delivered through `QWidgetWindow` and the context-menu event is generated from it. If
  swallowing the press also suppressed generation of the `QContextMenuEvent` on platforms that
  produce it on press, the menu would never open; the tests build the `QContextMenuEvent` by hand
  and so cannot see that. A smoke check on X11/Wayland by hand is needed (not testable
  headlessly).
- **Menu key.** The test sends a hand-built `QContextMenuEvent::Keyboard` to the tree. It does not
  send `Qt::Key_Menu` (so it cannot see the tree or `QAbstractItemView` consuming the key), and it
  never asserts where the menu opens (L3 is invisible to it). `QTest::keyClick(&tree, Qt::Key_Menu)`
  would cover the first; a scrolled-away selection plus a check that the popup's geometry is inside
  the tree's global rect would cover the second.
- **Parity during in-flight writes and modes.** No test opens the menu while `structuralTicket` is
  set (Rename, Delete, New should be disabled, Restore enabled) or in the crop/mask modes. The
  preview case is covered (New off, rest on); the busy case is the one the parity claim most
  depends on.
- **Stale or replaced photograph (M1).** Not covered; a test would open the menu, switch the photo
  from the probe's callback, and assert the row entries are disabled or gone.

Weak but acceptable: `CHECK(w.tree().contextMenuPolicy() == Qt::CustomContextMenu)` pins an
implementation detail; `the menu entries are the window's own actions` is a good guard against
copying the actions (identity plus the Ctrl+N shortcut).

## Findings list

| # | Severity | Location | Scenario |
|---|----------|----------|----------|
| M1 | medium | `MainWindow.cpp:1603,1620,1634` | Photo changes while the menu is open; lent id 1 now names another photograph's snapshot 1 and Restore (no confirmation) acts on it |
| L1 | low | `MainWindow.cpp:1603,1683` | Target row removed (conflict adoption) while the menu is open; entries stay enabled |
| L2 | low | `MainWindow.cpp:1633-1635` | Exception or window destruction during `exec()` skips the reset or writes to a dead `this` |
| L3 | low | `MainWindow.cpp:416-422` | Menu key after wheel-scrolling the selected row away: the menu opens outside the panel |
| L4 | low | `MainWindow.cpp:1613-1617` | No explicit `panel enabled` check; the keyboard path relies on focus semantics |
| L5 | low | `MainWindow.cpp:405-411` | A right-clicked row is unmarked; with another row previewed the banner and the menu disagree |
| N1-N3 | nit | docs, tests | See above |
