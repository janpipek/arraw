# Exports carry chosen metadata

An exported JPEG, PNG or TIFF has so far carried pixels and a colour profile
and nothing else: the camera, the exposure, the rating and the photographer's
name were all lost. ADR 028 chose exiv2 as the library that would later write
them; this is that decision. The behaviour follows `main`'s `ExportMetadata`
where it was right, and differs where it was not (it wrote the source's
whole EXIF, thumbnail and MakerNote aside, and gated it on an optional
dependency).

## Decision

**The caller names a source and the groups to carry.** In `ImageExport.h`:

```cpp
struct MetadataSelection { bool capture = true; bool location = false; bool descriptive = true; };
struct ExportMetadata { path source; PhotoMarks marks; MetadataSelection selection;
                        bool useSidecar = true; };
void exportImage(image, path, options, const std::optional<ExportMetadata>& = std::nullopt);
```

`std::optional` over a pointer: the value owns what it names, a caller cannot
hand over a dangling address, and `{}` reads as "none". Absent, or with no
group on, the export is *bit for bit* what it was: that path is the old code.
`useSidecar` exists so a caller that ignores sidecars (the command line's
`--no-sidecar`) also ignores them for metadata.

**Location is off by default.** A photograph's coordinates are the one group
that can reveal a home; every other group is what a photographer shows by
signing a print. It is opt-in, in the library as on the command line.

**Embedding happens before the destination is touched.** The image is encoded
to memory, exiv2 edits that buffer (it opens a memory image and writes it
back), and the result is written through the same `QSaveFile` as before. So a
failure of either step leaves no half-written file and an existing destination
intact, and there is one commit. A temporary file beside the destination was
the alternative: no memory cost, but a second atomic-replace mechanism next to
`QSaveFile`'s; the price here is one more copy of the encoded file in memory
(for a 16-bit TIFF, hundreds of MB at 60 MP) and only when metadata is asked
for. If that bites, a temp file is the follow-up.

**Reading metadata is best effort; the export never fails because of it.** A
source exiv2 cannot open (missing, or not an image it knows), a sidecar that is
not XMP, or a single tag that cannot be copied is left out, and a
`Notice::MetadataNotCarried` warning naming the source and what was left out
goes to the `DiagnosticLog` that `exportImage` takes (the command line prints
it like other per-file warnings with the exit code unaffected, Python sends it
to the `arraw` logger, the application shows "Exported <name> without some
metadata" with the notice as tooltip). Whatever can be carried still is: a bad
sidecar costs the sidecar's descriptive fields, not the capture group; an
unreadable source costs what only the source holds, while the marks and the
sidecar's descriptive fields, which arraw knows without it, are still carried. The reason is that metadata is an addition
to the pixels: an export that worked before it was asked to carry metadata
must keep working, and a photographer with a thousand-file batch should not
lose files to an odd tag in one of them. What stays an error is what makes
the *output* differ from what was asked: an exiv2 failure while writing into
the encoded buffer throws `std::runtime_error` (`Cannot write the metadata of
<source> into the export: ...`; the command line then suggests `--metadata
none`), and a rating outside -1 to 5 is `std::invalid_argument`, a caller's
bug whatever the source holds. A source that is readable but *has no metadata*
to carry is not even a warning: nothing is written, and the file is
bit-identical to an export without metadata (so not even `Software` is
stamped).

**What each group carries** is copied key by key with exiv2, never as a whole
IFD, so a MakerNote, an IFD1 thumbnail and anything that describes the
*source's* pixels (strip offsets, dimensions, compression, DNG tags, colour
matrices) cannot come along:

- *capture*: `Exif.Image.Make`, `Model`, `DateTime`; `Exif.Photo.ExposureTime`,
  `FNumber`, `ExposureProgram`, `ISOSpeedRatings`, `SensitivityType`,
  `DateTimeOriginal`, `DateTimeDigitized`, `OffsetTime`, `OffsetTimeOriginal`,
  `OffsetTimeDigitized`, `SubSecTime`, `SubSecTimeOriginal`,
  `SubSecTimeDigitized`, `ExposureBiasValue`, `MaxApertureValue`,
  `MeteringMode`, `LightSource`, `Flash`, `FocalLength`,
  `FocalLengthIn35mmFilm`, `LensMake`, `LensModel`, `LensSpecification`,
  `WhiteBalance`, `ExposureMode`, `SceneCaptureType`. Where the source has a
  tag only as XMP, its `exif:`, `exifEX:` or `tiff:` property is copied
  instead, and `aux:Lens` and `aux:LensInfo` are carried. **Serial numbers**
  (`BodySerialNumber`, `LensSerialNumber`, `CameraOwnerName`) are left out: they
  identify a body, and nobody asked for that.
- *location*: every `Exif.GPSInfo.*` tag and every XMP `exif:GPS*` property.
- *descriptive*: `xmp:Rating` and `xmp:Label` from the **marks** (`-1` is
  written as `-1`, as Lightroom does; no stars and no label are absent
  properties, not `0` or empty); `dc:title`, `dc:description`, `dc:subject`,
  `dc:creator` and `dc:rights` from the source's own XMP and then from its
  sidecar, which wins per property (it is the photographer's latest word);
  `Exif.Image.Artist` and `Copyright`. The marks come from the caller, so the
  source's own `xmp:Rating` and the sidecar's are not copied: the culling state
  arraw holds is the truth. IPTC is not written; XMP carries the same fields
  and PNG has no IPTC.
- *always, when anything is written*: `Orientation = 1` (the pixels are
  upright), `Exif.Photo.PixelXDimension` and `PixelYDimension` of the output,
  `Exif.Photo.ColorSpace` (1 for an sRGB output, 65535 "uncalibrated" for
  Display P3 and Adobe RGB, whose profile says the rest), and
  `Exif.Image.Software = "arraw <version>"`. The version is the CMake project
  version, compiled in as `ARRAW_VERSION` (as the command line's `--version`).

**Formats.** JPEG gets Exif and XMP segments, TIFF the Exif and GPS IFDs and
an XMP tag, PNG an `eXIf` chunk and an iTXt XMP packet, all as exiv2 writes
them. Tests show for each format that the decoded pixels and the embedded ICC
profile are unchanged by embedding (8 and 16 bit for the lossless ones), which
is the check that exiv2 rewrites a libtiff-encoded TIFF safely.

**The command line** gets `--metadata LIST`: `all`, `none`, or a comma list of
`capture`, `location`, `descriptive`, case-insensitively; default
`capture,descriptive`. An unknown name, an empty item or an empty value is a
usage error (2) before any file is touched. The marks are those the photograph
opened with (its sidecar's, defaults under `--no-sidecar`).

**Python** gets a frozen `MetadataSelection(capture=True, location=False,
descriptive=True)` and `save(..., metadata_from: Photo | None = None,
metadata: MetadataSelection = MetadataSelection())`. A `set[str]` or comma
string would have been terser, but ADR 018 binds C++ values one for one, with
frozen keyword-only classes, and a typo in a string is a runtime error where
a misspelt keyword is a `TypeError` at once. `metadata_from` takes the `Photo`
the pixels came from, which is both the source path and the marks; with `None`
nothing is written, so existing scripts are unchanged.

## Consequences

- The desktop application's export is unchanged for now: it passes no
  metadata. Its export dialog gets the same three switches when the film strip
  work reaches it.
- Exporting a RAW that LibRaw reads but exiv2 does not succeeds, without
  metadata and with a warning; `--metadata none` silences it.
- The embedding code (`src/core/MetadataEmbedding.cpp`) is private; the public
  header names no exiv2 type.
- A MakerNote is never carried, so a viewer that wants lens corrections or
  proprietary settings from the export will not find them; that is the point.
