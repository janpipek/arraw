# Film strip, shots, culling and EXIF: execution plan

Status: implemented on branch `develop-state`, 2026-10-03, in five commits:
EXIF (ADR 028), shots (ADR 029), saving (ADR 030), the strip, and thumbnails
(ADR 031). The lasting decisions are in those ADRs; this plan records the
questions and answers behind them.

## Goal

This rebuilds main's film strip on this branch's architecture, as the
[feature brief](../desired-features.md) describes: open, cull, develop.
- **Opening:** a folder is the session. Opening one photo fills the strip with
  that photo's folder.
- **Culling:** star ratings, a reject mark and colour labels are written to the
  sidecar at once, and the strip can be filtered by them.
- **Pairing:** a RAW and a JPEG of the same shot appear as one frame.
- **Shared code:** folder discovery and shot pairing live in the library, so
  the command line and Python use them too.

Main's structure is not to be copied. Its widget wrote sidecars itself
([reimplementation plan](reimplementation-plan.md), "FilmStrip performs culling
sidecar writes inside a widget"). Here the strip is a view, and mark writes go
through the library.

## Decisions

| Question | Answer |
|---|---|
| Where shots live | **The library.** A public `arraw::Shot` holds the primary file and its companions, and `listShots(folder)` returns them in order. Python binds them, and the CLI uses them. |
| Pairing | **As main.** Shots pair when they are in the same folder with the same file stem, compared case-insensitively. The RAW is the primary, and JPEG/TIFF/PNG files with that stem are its companions. A file with no RAW partner stands alone. The format label reads like `ARW+JPEG`. Sidecars (`.xmp`) are never listed. |
| Order | By file name, in natural order (IMG_2 before IMG_10). |
| Opening | File > Open Folder…, or the strip's folder button. File > Open… of one photo also fills the strip with its folder and selects that photo. |
| Leaving a changed photo | **As main.** Ctrl+S is "Save Adjustments". Moving to another photo, opening another folder or closing the window asks Save / Discard / Cancel. "Changed" means the develop state differs from what the sidecar holds, and Discard reverts to the sidecar. Undo history belongs to one visit and is dropped on leaving. |
| Marks | Ratings and labels are written to the sidecar at once, against their own baseline (ADR 021: marks are not in develop history). Keys apply to **all selected** shots. |
| Keys | **Main's keys:** 0–5 for stars (0 clears), X to reject, R/Y/G/B/P to toggle a colour label. |
| Arrow keys | **←/→ always move between photos.** Sliders never take keyboard focus. Spin boxes take it only while typing, and Enter or Esc hands it back. Ctrl+←/→ also moves between photos while typing. |
| Selection | Ctrl/Shift multi-select. The develop view always shows the active shot, which is the last one clicked. Batch export and paste will use the same selection later. |
| Filter | **As main:** at least N stars, or rejects only; and any of the chosen colours. Colours combine with OR, and the two dimensions with AND. It lasts for the session and is not saved. If the active shot stops matching, the view jumps to the nearest shot that does. |
| Look | **As main.** The strip is a bottom dock with square cells. Each cell shows the thumbnail, stars, a colour-label swatch and the format label. The active shot is outlined, and selected shots are dimmer. The filter and a folder button sit in the strip's title bar. The strip's height can change, and the cells scale with it. |
| Thumbnails | The camera's embedded JPEG shows first. It is replaced by arraw's developed rendering of the **saved** settings, rendered in the background on its own worker, never ahead of the preview. Thumbnails are about 512 px and cached on disk under the XDG cache directory, keyed by file, modification time and settings. The **current** photo's thumbnail follows edits live, from the preview. |
| Changes on disk | **Watch the folder.** New or removed files update the strip. A sidecar changed by another program refreshes that shot's marks and thumbnail. If it belongs to the open photo and there are no unsaved edits, the photo reloads; with unsaved edits, the user is told and nothing is overwritten. |
| Tooltips | File name, format label, size, and the EXIF capture information below. |
| Context menu | Rate, label, and reveal in the file manager. |
| EXIF | **exiv2, as a required dependency**, read for every format. |
| EXIF model | **A typed struct with EXIF names:** `make`, `model`, `lensModel`, `dateTimeOriginal` with its offset, `exposureTime` and `fNumber` as rationals, `photographicSensitivity`, `focalLength`, `focalLengthIn35mmFilm`, `exposureBiasValue`, `flash`, the GPS position, `artist` and `copyright`. Every field is optional. It is read alongside `ImageMetadata`, bound to Python, and shown by `arraw-cli info`. |
| Python | `arraw.list_shots(folder)` returns `Shot`s, each with a primary, companions and a format label, to be opened with `arraw.open(shot.primary)`. |
| CLI | `arraw-cli info shoot/` lists the shots with their marks and EXIF. `arraw-cli export shoot/ --min-rating 3 --label red` exports a filtered selection of shots. |

## Steps

1. **EXIF** (library): add exiv2 to CMake as `REQUIRED`, define the EXIF struct
   and its reader, extend `info`, bind it to Python, and add tests on the
   committed fixtures. Add exiv2 to the README's requirements and licence table.
   Write an ADR.
2. **Shots** (library): `Shot` and `listShots` (pairing, natural order, format
   label), Python `list_shots`, `info` on a folder, and export filtered by
   rating and label. Write an ADR covering the shot rules and the CLI shape.
3. **Saving in the GUI:** Save Adjustments, the leave guard, and separate
   baselines for the develop state and the marks.
4. **The strip:**
   - a model over the shots, with a filter proxy and a cell delegate;
   - the dock, with the filter and folder button in its title bar;
   - selection, keys, the context menu and tooltips;
   - the arrow-key focus policy;
   - a folder watcher.
5. **Thumbnails:** a background worker and the disk cache. Embedded first, then
   the developed rendering of the saved settings; the current photo live.

Each step is reviewed and committed before the next. Export metadata, which
was deferred on 2026-10-02 (see memory), can follow step 1 on the same exiv2
base.
