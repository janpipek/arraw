# Review: Reset All Settings

- Date: 2026-11-18
- Plan: [docs/ideas/reset-all-plan.md](../ideas/reset-all-plan.md)
- Implementation: Sonnet. Review: two Opus reviewers, one on correctness and one on quality and
  tests. Fixes: Sonnet. Orchestration and final check: Claude Opus 5.5.
- Scope: the uncommitted change on `main` that follows `7680cd8`, 11 files, about 290 lines.

## Summary

Reset All follows the plan and the `pasteSettings` idiom. It is one undoable
`EditOrigin::Reset` step that carries the detail `resetAllDetail` ("All"). The state becomes
`resetState()`, which gives `defaultStateFor(encoding)` but keeps `nextLocalAdjustmentId`. The
negative becomes `defaultNegativeFor(metadata)`, keeping the recorded lens. A confirmation dialog
always asks first and names the masks and lens corrections it removes. The action
(Ctrl+Shift+R) and the "Reset All" button above the develop tabs share one enabled state, which
`updatePasteAction()` refreshes.

The reviewers found no high-severity problems. All 12 findings were addressed, two of them by
decisions that need the user's confirmation (see "Open").

## Findings and outcomes

| # | Sev. | Finding | Outcome |
|---|---|---|---|
| 1 | medium | On macOS, Ctrl+Shift+R is Shift+Cmd+R, which is also one of the `QKeySequence::Refresh` keys bound to Refresh Folder. An ambiguous shortcut runs neither action. | Fixed: no shortcut on macOS (`#ifndef Q_OS_MACOS`), recorded in `docs/keybindings.md`. **Needs the user's confirmation.** |
| 2 | medium | A recorded lens left behind after turning the corrections off kept Reset All enabled, and resetting dropped it silently. | Fixed: `resetNegative()` keeps the recorded lens (`withCorrectionsOf`), as paste does. Corrections and demosaic still reset. **Needs the user's confirmation.** |
| 3 | low | Cancel, after the shortcut was pressed during a mask stroke, left the session edit open. | Fixed: an open edit is committed before the dialog. No test was added for it. |
| 4 | low | The extra `isEnabled()` guard ignored a pending edit. | Fixed: the guard was dropped, and the check is repeated after `finishPendingEdit`. |
| 5 | low | The window test did not check the global settings after reset, undo and redo. | Fixed. |
| 6 | low | The lens clause was untested because the test assumed it needed lensfun. | Fixed: the sidecar sets `distortion = true`, and the test asserts `lensOn`. |
| 7 | nit | The magic string "All" was repeated in two files. | Fixed: `resetAllDetail` in `HistoryModel.h`. |
| 8 | nit | The dialog sentence was assembled from fragments. | Fixed: whole sentences with `%n` plurals. |
| 9 | nit | The enabled-state test did not leave crop mode again. | Partly fixed: leaving crop mode is covered, but the button is not checked at the defaults. |
| 10 | nit | The plan's line references were stale. | Fixed. |

Two fixes made in the final pass: the tooltip now reads "Puts every setting of the photograph
back to its default.", matching the verb style of the other tooltips, and a duplicated `Photo&`
reference in `resetAllSettings` is merged.

## Remaining notes

- No window test covers the kept recorded lens or Cancel during a stroke.
- `resetState` is public in `include/DevelopState.h`, so Python could bind it later.

## Open (for the user)

1. On macOS, should there be no shortcut (as now), a different key, or should Refresh Folder
   give up Shift+Cmd+R?
2. Is keeping the recorded lens (the corrections still turn off) right for "reset everything"?

## Verification

Run in the container (`ARRAW_BUILD_PREFIX=container-`, offscreen):

- `arraw-tests`: all passed, 1935 test cases.
- `arraw-widget-tests`: all passed, 341 test cases.
- `just format-check`: clean.
