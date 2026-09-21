# RAW import through LibRaw, as a neutral development

`desired-features.md` opens with "a RAW editor for photographers", and
`loadImage` could not read one. LibRaw is the ingestion foundation the
reimplementation plan already names (§882), so the question was never *whether*
LibRaw, but what `loadImage` should hand back and who should call it.

Investigating turned up something that had to be decided first. On a machine
with `kf6-kimageformats` installed, **`loadImage` already decoded RAW files** —
KDE ships `kimg_raw.so`, a LibRaw-backed plugin, into Qt's imageformats
directory, and `QImageReader` picks it up. It decoded a test DNG at full
resolution, 16-bit, with no code of ours. On a machine without that package it
would fail. The same file, two outcomes, decided by an unrelated distribution
package: a developer's machine and CI would disagree, and nothing in the build
would say so.

## Decision

**LibRaw is linked directly and is a required dependency**, via
`pkg_check_modules(... libraw_r)` — the reentrant variant, because decoding
belongs on a background thread. Not optional behind a flag: a build flag would
reintroduce exactly the divergence above, only as our own.

**`loadImage` dispatches by extension, then by content.** A RAW extension (the
ten in `desired-features.md`) goes to LibRaw. Anything else is content-detected
by Qt as before, and a file Qt cannot read is offered to LibRaw as a last
resort, so a misnamed RAW still loads — more slowly than a correctly named one.
Extension chooses the *decoder*, never the format a decoder then reads.

Extension-first, not content-first, because **LibRaw opens ordinary TIFFs as
happily as camera files**; asking it first would capture arraw's own TIFF
exports.

**A detected format of `raw` is treated as a decline**, sending the file to our
LibRaw path instead. The plugin claims files by *extension*, not signature, and
it claims eleven RAW extensions beyond the ten arraw routes to LibRaw itself —
`.mrw .srf .x3f .kdc .mos .raw .3fr .iiq .erf .nrw .crw`. Those are the files
the decline is for: with the plugin present they are skipped and reach LibRaw;
without it, Qt detects them as TIFF, fails, and the content fallback reaches
LibRaw anyway. Both machines then produce identical pixels. Measured: left to
the plugin, `holiday.mrw` decodes 24×32 rather than 32×24, because it honours
the orientation tag.

**A RAW arrives as a neutral development, not sensor data.** LibRaw demosaics
(AHD), applies the camera's as-shot white balance, and converts through the
camera's colour matrix to linear Rec.2020 (`output_color = 8`), landing exactly
on `workingEncoding` (ADR 003). Every parameter is set explicitly; two are
forced by promises arraw has already made:

- `no_auto_bright = 1` — LibRaw otherwise applies a content-dependent brightness
  stretch, so one develop setting would render differently frame to frame.
- `user_flip = 0` — LibRaw defaults to honouring the camera's orientation.
  `loadImage` promises rotation stays a develop setting, and that promise must
  not depend on the file format. It also keeps the buffer in sensor geometry,
  which is the frame lens-correction profiles are described in.

With `gamm = {1, 1}`, `output_bps = 16`, `use_camera_wb = 1` and
`highlight = 0` (clip; a 16-bit integer output has no headroom to carry
unclipped values anyway).

## Consequences

- **White balance and demosaic are baked in.** Both are develop settings in
  `desired-features.md`, and `ImageBuffer` cannot carry the multipliers that
  were used. A future WB control can therefore only work *relative* to as-shot,
  and changing demosaic means re-decoding — which is where `main` independently
  arrived (its ADR 0036). A later `RawLoadOptions` reopens this additively: the
  colour matrix can be overridden on the LibRaw handle (`cam_xyz`), through an
  ICC profile (`camera_profile`; the linked `libraw.so` does have lcms2), or by
  dropping to `output_color = 0`. None of those change the output contract, so
  no caller moves — but they do change rendered pixels, so the choice is cheaper
  to revisit before sidecars exist than after (plan §470).
- **LibRaw validates nothing.** `output_color = 9` is silently accepted and
  falls back to sRGB rather than erroring. Values reaching these fields must
  never come from an unchecked integer.
- **Lens corrections are not part of import.** They operate on the decoded,
  demosaiced, linear buffer, before crop and rotation — a separate apply-once
  stage, as `main` concluded in its ADR 0032. Lensfun matches a profile from
  camera, lens, focal length, aperture and focus distance, none of which an
  `ImageBuffer` carries, so that stage waits on a `readMetadata(path)` that
  reads headers without decoding pixels. Deliberately deferred: the film strip
  and `arraw-cli info` need metadata *without* pixels, so metadata-by-path has
  to exist regardless, and it should be designed against two callers rather than
  guessed at from one.
- **Licensing is not a constraint here.** `main` is GPL-3; LibRaw (LGPL-2.1/
  CDDL) and lensfun (LGPL-3) are compatible, and LGPL's relinking obligation is
  already satisfied by a GPL-3 work shipping complete source. This branch has no
  `LICENSE` file yet, which should be restored deliberately rather than by
  default. It also means ADR 0036's licensing argument against the GPL demosaic
  pack does not apply to us — what is left against AMaZE is packaging effort.
- **A first build now needs LibRaw** (`LibRaw-devel`, or vcpkg on Windows),
  ahead of Windows packaging existing. Accepted: a RAW editor that can be built
  without RAW support is the stranger artefact.
