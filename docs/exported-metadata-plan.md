# Implementation plan — Exported Metadata (embed into exported files)

Red-first plan for embedding [[Exported Metadata]] into exported JPEG/TIFF/PNG.
Architecture and rationale are in [ADR 0043](adr/0043-exported-metadata-exiv2-corrected-passthrough.md)
(builds the deferral in [ADR 0037](adr/0037-editable-dc-user-metadata-ownership.md));
vocabulary in [CONTEXT.md](../CONTEXT.md) (**Exported Metadata**, **User Metadata**).
Designed via a grilling session; this file is the resume point.

## Scope (this milestone)

A new headless `ExportMetadata` module writes capture EXIF (corrected passthrough) +
arraw's User Metadata XMP into the encoded output file, gated by a three-group
selection on the export dialog.

| Group | Contents | Default |
|---|---|---|
| Camera & capture info | corrected EXIF passthrough from source RAW | on |
| Location | GPS tags | off |
| Descriptive metadata | User Metadata (Title/Caption/Keywords/Creator/Copyright + Rating/Label) as XMP | on |

## Guiding constraints

- **One embed routine, every export path.** `embedExportMetadata(outputPath,
  sourcePath, UserMetadata, selection)` in `arraw_core`, called right after the
  encode by single export, batch, and the future CLI (ADR 0022). Do not inline
  exiv2 at each call site.
- **Reuse the existing metadata resolver.** Batch resolves each file's User Metadata
  via `XmpSidecar` (sidecar `dc:` → embedded XMP → EXIF), same as the Info Panel.
- **Image-first, best-effort.** `QImage::save` first; exiv2 second. Embed failure or
  a build with no exiv2 never fails the export (batch never aborts).
- **exiv2 optional like lensfun.** pkg-config-first, `ARRAW_WITH_EXIV2=AUTO|ON|OFF`,
  `ARRAW_HAS_EXIV2`; `ON` for release. exiv2 isolated to the one module.
- **Headless-testable** (Catch2 in `tests/`, like `test_XmpSidecar`); Jan runs the
  app for feel.

## Data model changes

- **`ExportOptions`** (`src/ExportOptions.h`): add the three-group selection (e.g.
  `bool includeCaptureInfo = true; bool includeLocation = false; bool
  includeDescriptive = true;`).
- **`ExportMetadata`** (`src/ExportMetadata.{h,cpp}`, new): the embed entry point,
  exiv2-guarded. Reads source EXIF, applies the corrected-passthrough rules, writes
  the selected XMP.
- **Export call sites** (`MainWindow::exportImage`, `MainWindow::exportBatch`): pass
  `sourcePath` + resolved `UserMetadata` into the embed step after `saveExportImage`.
- **`ExportDialog`**: three checkboxes wired into `ExportOptions`.

## Red-first test sequence

All in a new `tests/test_ExportMetadata.cpp` unless noted. Each writes a small
image, embeds, then reads back with exiv2 to assert. Guard the suite on
`ARRAW_HAS_EXIV2`.

1. **Descriptive XMP round-trips.** Embed UserMetadata (title/caption/keywords/
   creator/copyright) → read back → assert the `dc:` values, with keywords as a bag.
2. **Corrected Orientation.** Source EXIF says Orientation=6; embed into an
   already-upright export → read back Orientation==1 (no double-rotation).
3. **Capture passthrough.** Source make/model/lens/exposure/DateTimeOriginal survive
   into the output when *Camera & capture info* is on.
4. **Selection gating.** Capture off → no EXIF capture tags; Location off (default) →
   no GPS even when the source has GPS; Descriptive off → no `dc:` XMP. Each group
   independent.
5. **Thumbnail & stale tags dropped.** The RAW's embedded thumbnail and the stale
   dimension tags are not present in the output; `Software` reads as arraw.
6. **ICC coexists.** Export with a non-sRGB output profile → after embed, the ICC
   profile Qt wrote is still present *and* the metadata is present.
7. **Best-effort.** Embedding into a path exiv2 rejects returns a non-fatal failure
   and leaves the already-written image intact (caller still counts it exported).
8. **Format coverage.** Tests 1–3 parametrised over JPEG, TIFF, PNG.

## Build changes

- `CMakeLists.txt`: `ARRAW_WITH_EXIV2` option + pkg-config-first detect, mirroring
  the lensfun block; define `ARRAW_HAS_EXIV2`; link the module.
- Packaging recipes (AppImage CI, `package_fedora.sh`, Windows/vcpkg, brew) gain
  exiv2; release configs set `ARRAW_WITH_EXIV2=ON`.

## UI changes (after the model is green)

- `ExportDialog`: a "Metadata" group with the three checkboxes (capture on, location
  off, descriptive on); read into `ExportOptions::options()`.
- Wire `sourcePath` + resolved `UserMetadata` into both `MainWindow` export paths.

## Deferred (documented, not built here)

- **Edit metadata on standalone non-RAW files** (ADR 0037's other deferred half) —
  same in-file writer, separate edit surface.
- **Person/region info** stripping; **per-field** metadata selection.
- Headless **CLI** flags land when `arraw-cli` itself is built (ADR 0022 not yet
  implemented).
