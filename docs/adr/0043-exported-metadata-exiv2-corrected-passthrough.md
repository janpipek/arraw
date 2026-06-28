# Exported files carry corrected-passthrough metadata, written by exiv2

arraw embeds [[Exported Metadata]] into every exported JPEG, TIFF, and PNG: the
original capture EXIF copied through from the source RAW, plus arraw's
[[User Metadata]] written as XMP. This builds the feature ADR 0037 deliberately
**deferred** ("Embedding metadata into exported JPEGs … needs a metadata *writer*
for in-file segments, which Qt does not provide … Tracked in the plan, not built
here"). The governing constraint is unchanged from the rest of the codebase: the
metadata *resolution* logic exists once ([[spot-for-algorithms]]) — the same
sidecar `dc:` → embedded XMP → EXIF resolver the [[Info Panel]] uses (ADR 0037)
supplies the User Metadata an export writes.

## Payload: corrected EXIF passthrough + arraw XMP

A delivered photo is expected to carry its capture data (camera, lens, exposure,
date) the way Lightroom/Capture One deliverables do, so the export copies the
source EXIF rather than writing only arraw's own fields. But the developed pixels
are cropped, rotated upright, resized, and colour-converted, so parts of the
source EXIF now *describe the RAW, not the export*. The passthrough is therefore
**corrected**, not blind: EXIF Orientation is forced to upright (1), the stale
dimension tags and the RAW's embedded thumbnail (which shows the undeveloped
image) are dropped, `Software` is set to arraw, and `DateTimeOriginal` and the
remaining tags survive. arraw's own descriptive fields are written as standard
Dublin Core XMP.

## The user chooses what travels, in three groups

Deliverables are emailed and uploaded, so the export must not silently leak more
than the photographer intends — GPS in particular. The export dialog exposes three
independent toggles, plumbed through a new `ExportOptions` field (and the matching
flags on the future headless [[CLI]], ADR 0022):

| Group | Contents | Default |
|---|---|---|
| Camera & capture info | corrected EXIF passthrough (make/model, lens, exposure, date) | **on** |
| Location | GPS tags | **off** |
| Descriptive metadata | [[User Metadata]] — Title, Caption, [[Keywords]], Creator, Copyright + [[Rating]]/[[Colour Label]] — as XMP | **on** |

GPS defaults off because the cost of an accidental leak is high and hard to
reverse once a file is shared; the other two default on because their absence
would surprise.

## exiv2, optional like lensfun

Qt's image writers cannot write EXIF/XMP/IPTC; `libexif` does EXIF but not XMP.
**exiv2** writes EXIF + XMP + IPTC across JPEG/TIFF/PNG and copies EXIF
source→dest natively (which *is* the passthrough), so the writer is largely
format-shared. It is wired exactly like lensfun (ADR 0032): pkg-config-first,
`ARRAW_WITH_EXIV2=AUTO|ON|OFF`, an `ARRAW_HAS_EXIV2` compile define guarding one
isolated module. Dev builds without exiv2 still compile and simply export plain
images; **release/packaged builds set `ON`** so the shipped app always embeds.
This adds exiv2 to all four packaging targets (AppImage / Fedora RPM / vcpkg /
brew).

## Architecture: one engine module, best-effort, image-first

A new headless module in `arraw_core` (`ExportMetadata`) exposes a single entry
point — `embedExportMetadata(outputPath, sourcePath, UserMetadata, selection)` —
called right after the encode by every export path (single export, batch, and the
future CLI), so the three cannot drift. exiv2 reads the source EXIF from
`sourcePath` and writes the cleaned result plus the XMP into `outputPath`. The
module is Catch2-testable with no GUI, like `test_XmpSidecar` (Jan runs the app
for feel).

The pixels are the deliverable, so embedding is **image-first and best-effort**:
`QImage::save` writes the file, then exiv2 injects metadata as a second step. If
injection fails (a quirky maker note, a locked file) or the build has no exiv2,
the rendered image is still written and still counts as exported; the failure is
surfaced non-fatally. Batch never aborts on a metadata hiccup.

## exiv2 authors the export XMP

ADR 0037 pins exact RDF container shapes for the `dc:` fields, and `XmpSidecar`
already serializes them. For the *exported* packet we hand the values to exiv2 and
let it author standards-compliant `dc:` XMP, rather than injecting arraw's own
serialized packet. The exported file is a write-once leaf — never round-tripped
back into arraw — so it does not need arraw's sidecar serializer, and this does
not duplicate the *algorithm* (exiv2 is a standard XMP writer, not a second copy
of arraw's container logic). The sidecar serializer remains the single source for
the *interop* packets arraw reads back.

## Considered Options

- **arraw User Metadata only (no EXIF passthrough).** Simplest, no
  passthrough-correctness traps, but a delivered photo would carry no
  camera/lens/exposure/date — surprising for a RAW editor. Rejected.
- **Full XMP packet passthrough (sidecar/embedded) + EXIF.** Maximal fidelity, but
  drags foreign and stale develop metadata (`crs:`, `lr:`) into a deliverable,
  which is usually unwanted in an output file. Rejected.
- **Blind EXIF passthrough.** Minimal code, but ships a wrong Orientation
  (double-rotation in viewers), stale dimensions, and an undeveloped thumbnail —
  effectively a bug. Rejected for corrected passthrough.
- **Hard-required exiv2.** No `#ifdef` branches, but breaks the "builds with
  Qt + libraw + stdlib" floor and forces exiv2 onto every dev/CI machine. Rejected
  for the lensfun-style optional pattern (required only for release).
- **Hand-write an XMP APP1 segment, no new dependency.** Reuses arraw's QDom
  serializer, but makes EXIF passthrough (the chosen payload) impractical — copying
  and correcting the source EXIF segment by hand is exactly what exiv2 exists to do.
  Rejected.
- **Treat metadata failure as export failure.** Guarantees every delivered file has
  metadata, but lets one quirky maker note discard a perfectly good rendered image.
  Rejected for image-first best-effort.
- **JPEG only.** Matches the feature's title, but a TIFF export — often the
  higher-quality deliverable — would silently carry nothing. Rejected; embed wherever
  exiv2 can (JPEG/TIFF/PNG).

## Consequences

- A new `ExportOptions` field carries the three-group selection; it must reach all
  export paths and map onto CLI flags (defaults: capture on, location off,
  descriptive on).
- exiv2 joins the build as an optional (release-required) dependency across all four
  packaging targets; the AppImage/RPM/vcpkg/brew recipes gain it.
- A test must assert exiv2 does **not** strip the ICC profile Qt embeds for the
  [[Output transform]] — colour tag and metadata coexist.
- Batch export resolves each non-active file's User Metadata through the existing
  `XmpSidecar` resolver (sidecar `dc:` → embedded XMP → EXIF) — no new read path.
- Editing metadata on standalone non-RAW files (the other half of ADR 0037's
  deferral) remains deferred; it needs the same in-file writer but is a separate
  edit surface.
