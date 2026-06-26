# Implementation plan — Info Panel editable User Metadata

Red-first plan for making descriptive metadata editable. Architecture and rationale
are in [ADR 0033](adr/0033-editable-dc-user-metadata-ownership.md) (amends
[ADR 0026](adr/0026-shared-xmp-property-ownership.md)); vocabulary in
[CONTEXT.md](../CONTEXT.md) (**User Metadata**, **Info Panel**, **Keywords**, **XMP
Property Ownership**). Designed via a grilling session; this file is the resume
point if work continues in a new session.

## Scope (this milestone)

Five editable descriptive fields in a renamed **Info Panel**, written to the develop
sidecar as Dublin Core:

| Field | Property | RDF container | Read fallback when `dc:` absent |
|---|---|---|---|
| Title | `dc:title` | `rdf:Alt` (`x-default`) | — |
| Caption | `dc:description` | `rdf:Alt` (`x-default`) | EXIF `other.desc` |
| Keywords | `dc:subject` | `rdf:Bag` | — |
| Creator | `dc:creator` | `rdf:Seq` | EXIF `other.artist` |
| Copyright | `dc:rights` | `rdf:Alt` (`x-default`) | — |

Plus a few **read-only** EXIF rows added to `extractMetadata()` from fields LibRaw
already exposes (validated, fragile maker-note fields skipped): FlashEC
(`makernotes.common.FlashEC`), ColorSpace (`common.ColorSpace`), firmware
(`common.firmware`), drive/focus/exposure mode (`shootinginfo.*`, small enum maps),
shot order (`other.shot_order`), internal body serial (`shootinginfo.InternalBodySerial`).

## Guiding constraints

- **One persistence model for User Metadata.** Descriptive fields join the existing
  Rating/Colour-Label contract: immediate save on field commit (`editingFinished`),
  **off** the develop undo stack, flush pending edits on image switch. Do not invent
  a second model. (See `MainWindow::applyCurrentUserMetadata`, `setCurrentRating`.)
- **One `dc:` extraction routine** reused by the sidecar reader and the embedded-XMP
  reader (SPOT). Read precedence: sidecar `dc:` → embedded XMP
  (`imgdata.idata.xmpdata` / `xmplen`) → EXIF.
- **Own named properties, not the namespace.** Save replaces only the five `dc:`
  properties; everything else in the packet (incl. `lr:hierarchicalSubject` and
  foreign `dc:`) survives semantically (ADR 0026/0033). Flat keywords only — never
  author a hierarchy.
- The metadata model is **headless-testable** with no GUI (Catch2 in `tests/`, like
  `test_XmpSidecar.cpp`); Jan runs the app for feel.

## Data model changes

- **`UserMetadata`** (`src/UserMetadata.h`): add `QString title, caption, creator,
  copyright;` and `QStringList keywords;`. Keep `operator==` defaulted (drives
  dirty-tracking / `markMetadataSaved`). The struct stays "bounded named fields,"
  as its own comment promises.
- **`XmpSidecar`** (`src/XmpSidecar.cpp`): extend the read path to parse the five
  `dc:` properties into `UserMetadata`; extend `saveMetadata` to write/replace them
  with correct RDF containers while preserving the rest of the packet. Factor a
  `dc:`-parsing helper usable on both the sidecar `QDomDocument` and the embedded
  blob string.
- **Embedded read**: parse `imgdata.idata.xmpdata` (len `xmplen`) through the shared
  helper during load; feed results into the resolve/pre-fill step.

## Red-first test sequence

1. **Round-trip + foreign preservation (pins ADR 0026/0033 at once).** Sidecar with
   `dc:subject` (bag), a foreign `dc:` property, and `lr:hierarchicalSubject` →
   parse → edit keywords → save → assert owned `dc:` replaced, foreign `dc:` and
   `lr:hierarchicalSubject` survive byte-for-meaning.
2. **RDF containers.** `dc:title`/`description`/`rights` round-trip as `rdf:Alt`
   `x-default`; `dc:subject` as `rdf:Bag`; `dc:creator` as `rdf:Seq`. Reading a
   foreign packet that used a different container shape still yields the value.
3. **Read precedence.** sidecar `dc:` wins over embedded XMP wins over EXIF;
   Caption/Creator pre-fill from EXIF only when no `dc:` value exists.
4. **Empty/absent.** No `dc:` and no fallback → empty field, and an empty field does
   not write an empty property (mirrors `add()` skipping empties / `xmp:Label`
   absence).
5. **Embedded blob parsing.** A RAW whose `idata.xmpdata` carries `dc:subject`
   surfaces keywords with no sidecar present.
6. **Read-only EXIF rows** added to `extractMetadata()` (extend `test` coverage as in
   the existing metadata tests).

## UI changes (after the model is green)

- Rename the tab "Exif" → "Info" (`MainWindow.cpp:944` area).
- `ExifPanel` → split into two sections: an editable User Metadata form (top) and the
  existing read-only `ImageMetadata` rows (below). Title/Caption/Copyright =
  single-line; Caption multi-line; Creator single-line; Keywords single-line
  comma-separated (chips are a later pure-UI swap, no model change).
- Wire `editingFinished` → a `MainWindow` slot that builds `UserMetadata` and calls
  the existing `applyCurrentUserMetadata` path. Flush pending edits in the
  image-switch path before `applyLoadResult` clears state.
- Keep the read-only section purely read-only (no edit affordance).

## Deferred (documented, not built here)

- **Embed metadata into exported JPEGs** (EXIF + XMP, maybe legacy IPTC). Qt's JPEG
  writer cannot write metadata; needs a writer for in-file segments — likely a new
  dependency (exiv2) to package across AppImage / Fedora RPM / vcpkg / brew. Decide
  payload (user-fields-only XMP vs full camera-EXIF passthrough) when picked up.
- **Edit metadata on standalone non-RAW files**, whose metadata is embedded rather
  than in a sidecar — same in-file-writer problem; pairs with the export work.
- Keyword **chips** UI; multi-value Creator UX beyond a single line.
