# Reset all settings of a photograph — plan

Status: implemented, 2026-11-18.

Deviations: the library helper is the public `resetState(current, encoding)` in `DevelopState.h`.
The seam is `MainWindow::ResetPrompts::confirm(file, maskCount, lensOn)`. The Reset All action
and button update from `updatePasteAction()`, so they follow the same events as Paste. The
history wording is keyed on the detail `resetAllDetail` ("All", `HistoryModel.h`). The seam's lens
flag is tested; the real dialog's text is not. The recorded lens is kept by the reset (as the
corrections being off keeps it); only the corrections go back to off. Ctrl+Shift+R is not bound on
macOS, where Shift+Cmd+R is Refresh Folder (the plan's "free" held on Linux and Windows only).

## Decisions (user, 2026-11-18)

| Question | Answer |
|---|---|
| What "all" covers | Everything: the global settings, the local adjustments (masks) and the negative (demosaic, lens corrections) |
| What it resets to | The defaults for the photograph's kind: `defaultStateFor(encoding)` and `defaultNegativeFor(metadata)` |
| Where | A menu action with a shortcut, and a button in the develop panel |
| Scope | The open photograph only; no multi-selection, no CLI |
| Confirmation | Yes, a dialog before resetting |

## What the code does today (verified)

- Single settings, the curve channel and the geometry already reset, recorded as
  `EditOrigin::Reset` (`DevelopPanel::applyGeometryReset`, `finishResetEdit`,
  `src/app/ui/DevelopPanel.cpp:523`). `HistoryModel` names such a step "Reset <group>" when one
  group changed, and plain "Reset" otherwise (`HistoryModel.cpp`).
- `defaultStateFor` (`include/DevelopState.h`) is the one place defaults depend on the kind of
  photograph (ADR 039): colour noise reduction on for a RAW, neutral otherwise.
- `defaultNegativeFor` (`include/NegativeSettings.h:170`) gives AHD and no lens correction.
- `MainWindow::pasteSettings` is the model to follow. It
  finishes a pending edit, abandons a mask drag, refuses in crop mode, and writes state and
  negative as one step through `EditSession::setState(negative, state, origin)`.
  `refreshPanel()` re-decodes when the negative changed.
- The Edit menu has Undo, Redo, then Copy Settings… (Ctrl+Shift+C) and Paste Settings
  (Ctrl+Shift+V). Ctrl+Shift+R is free.

## Behaviour

- **Edit ▸ Reset All Settings…**, Ctrl+Shift+R, below Paste Settings. It is enabled when a photograph
  is editable, crop mode is off, and the photograph differs from its defaults.
- **A "Reset All" button** at the top of the develop dock, above the Adjustments/Masks tabs, so
  it shows on both tabs (ADR 048). It triggers the same action and is enabled under the same
  conditions.
- **Confirmation:** "Reset all settings of <file name>? This also removes its N masks and turns
  off lens corrections." Only the clauses that apply are shown. The buttons are Reset (default)
  and Cancel. The step is undoable, and the dialog says so.
- **The result** is one history step with `EditOrigin::Reset` and the detail "All", named
  "Reset All" in History. The state becomes `defaultStateFor(metadata.encoding)` and the negative
  becomes `defaultNegativeFor(metadata)`.
- **Untouched:** culling marks (`PhotoMarks`), snapshots (ADR 056) and the history before the
  step.
- **`nextLocalAdjustmentId` is kept, not reset to 1.** Ids never go down (`DevelopState.h`), so an
  undo followed by a redo, or a snapshot, never sees two masks with one id.

## Steps

1. **Library:** add `DevelopState resetState(const DevelopState& current, const ColorEncoding&)`
   or an equivalent private helper that keeps `nextLocalAdjustmentId`. Test it alongside
   `defaultStateFor`.
2. **App:** add `MainWindow::resetAllSettings()`, modelled on `pasteSettings`, and a
   `ResetPrompts`-style seam for the dialog, as `ExportPrompts` has, so tests can answer it.
3. **App:** add the menu action and shortcut, the develop-dock button and the enabled state. The
   enabled state is updated wherever Paste's is.
4. **History:** make `HistoryModel` name a Reset step that has the detail "All" as "Reset All".
5. **Tests:** cover undo and redo of all three parts, a re-decode when the negative changed,
   nothing happening on Cancel, the action being disabled at defaults and in crop mode, and the
   id counter being kept.
6. Update the docs: `desired-features.md` and the shortcut list, if one exists.

## Settled (user, 2026-11-18)

- The dialog always asks, whatever would change.
- No "Don't ask again" checkbox for now (perhaps later).
- The button reads "Reset All".
