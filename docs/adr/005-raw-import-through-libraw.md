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

**LibRaw is linked directly and is a required dependency**, via pkg-config. Prefer
`libraw_r` when the distribution ships that module; accept `libraw` where it is
the only pkg-config module available. Not optional behind a flag: a build flag
would reintroduce exactly the divergence above, only as our own.

**`loadImage` dispatches by extension, then by content, and only then asks
Qt.** A RAW extension (the ten in `desired-features.md`) goes to LibRaw; so
does anything else LibRaw recognises, which costs a header parse and no pixels.
What LibRaw declines goes to Qt. Extension chooses the *decoder*, never the
format a decoder then reads — and it is a fast path in front of the content
check now, not the thing deciding.

Content **before** Qt rather than after it, because **a RAW container is
usually a TIFF whose first IFD is an ordinary RGB preview**. Ask Qt first and
nothing fails: its TIFF reader decodes that preview perfectly well, and the
caller gets a thumbnail where it asked for the photograph. Nothing in the
result says so. This is the whole of the deployment-dependence this ADR set out
to remove, arriving by a second door — for the eleven RAW extensions `loadImage`
does not list, and for every renamed file.

The first version of this decision put Qt first, on the grounds that **LibRaw
opens ordinary TIFFs as happily as camera files**, so asking it first would
capture arraw's own TIFF exports. Measured against LibRaw 0.22.2, that is not
so: it declines every multi-channel TIFF offered to it — arraw's own 8- and
16-bit exports, uncompressed and LZW RGB from Pillow, and an RGB TIFF carrying
`Make`/`Model` tags. The one ordinary file it does claim is a *single-plane*
16-bit TIFF with camera tags, which is what a camera's own greyscale TIFF looks
like, and is not a shape arraw writes. `test_RawImport.cpp` pins the
assumption: an exported TIFF must come back through Qt, so a greedier future
LibRaw fails a test rather than quietly re-routing a user's own exports.

**A detected format of `raw` is treated as a decline**, sending the file to our
LibRaw path instead. The plugin claims files by *extension*, not signature, and
it claims eleven RAW extensions beyond the ten arraw routes to LibRaw itself —
`.mrw .srf .x3f .kdc .mos .raw .3fr .iiq .erf .nrw .crw`. Those are the files
the decline was for: with the plugin present they were skipped and reached
LibRaw; without it, Qt detected them as TIFF and — it was assumed — failed.
That second half is what the preview case disproves, and why content now
decides before Qt is asked at all. The plugin no longer gets offered a file
LibRaw can read, so the decline is a second line of defence rather than the
mechanism; it stays for the day the plugin's LibRaw recognises something ours
does not. Measured: left to the plugin, `holiday.mrw` decodes 24×32 rather than
32×24, because it honours the orientation tag.

**A RAW arrives as a neutral development, not sensor data.** LibRaw demosaics
(AHD), applies the camera's as-shot white balance, and converts through the
camera's colour matrix to linear Rec.2020 (`output_color = 8`), landing exactly
on `workingEncoding` (ADR 003). Every parameter is set explicitly; two are
forced by promises arraw has already made:

- `no_auto_bright = 1` **and `adjust_maximum_thr = 0`** — two halves of one
  promise. The first disables the histogram stretch; the second disables a
  second, quieter one, where LibRaw lowers the white level to the frame's own
  brightest sample whenever that sample lands within 0.75 of the declared
  white level. Either alone leaves a develop setting rendering differently
  frame to frame: with the default threshold, a 16000 patch under a declared
  white level of 65535 imports as 16000 or as 20164 depending on what else is
  in the frame.
- `user_flip = 0` — LibRaw defaults to honouring the camera's orientation.
  `loadImage` promises rotation stays a develop setting, and that promise must
  not depend on the file format. It also keeps the buffer in sensor geometry,
  which is the frame lens-correction profiles are described in.

With `gamm = {1, 1}`, `output_bps = 16` and `highlight = 0` (clip; a 16-bit
integer output has no headroom to carry unclipped values anyway).

`use_camera_wb = 1` **only when there is a camera white balance to use.**
The flag does not mean what its name suggests for a file that declares no
as-shot neutral: LibRaw then computes one from the frame, which is the
histogram guess `use_auto_wb = 0` was meant to refuse. arraw checks `cam_mul`
after opening and, when it is absent, switches the flag off instead — leaving
the daylight multipliers the camera's colour matrix implies. A fixed wrong
white balance beats a plausible one that moves with the scene, because only the
fixed one can be corrected once for a whole shoot. Measured on a flat
(48000, 32000, 16000) frame: automatic gives (47999, 48000, 48000), a neutral
grey the sensor never saw; daylight gives (41346, 32922, 17934), and gives the
same answer on a frame of different content.

That substitution should be **said out loud**, and currently is not: the frame
will not look as its camera intended, and nothing tells anyone. It becomes a
warning on the import path as soon as there is somewhere to put one — which is
the same missing channel the "unsupported file" and "clipped highlights" cases
will want.

## Consequences

- **White balance, demosaic and highlight handling are baked in.** All three
  are develop settings in `desired-features.md`, and `ImageBuffer` cannot carry
  the multipliers or the white level that were used. A future WB control can
  therefore only work *relative* to as-shot, and changing demosaic **or
  highlight handling** means re-decoding — which is where `main` independently
  arrived (its ADR 0036). A later `RawLoadOptions` reopens this additively: the
  colour matrix can be overridden on the LibRaw handle (`cam_xyz`) or through
  an ICC profile (`camera_profile`; the linked `libraw.so` does have lcms2),
  and neither moves a caller. Dropping to `output_color = 0` is a different
  kind of change: it hands back *camera space*, not Rec.2020, so it holds the
  output contract only if arraw then does the camera-to-working-space
  conversion itself. That is a stage to build, not a parameter to flip. All of
  them change rendered pixels, so the choice is cheaper to revisit before
  sidecars exist than after (plan §470).

- **Clipping at import loses what a highlight recovery would need.**
  `highlight = 0` is honest about the pixel format, but "recovery is a develop
  decision" overstates what the decision will have left to work with: white
  balance scaling and the colour matrix both clip on the way to 16-bit
  integers, and promoting an already-clipped buffer to float later restores
  nothing. Accepted for now, with the re-decode above as the escape: highlight
  handling changes the decode, it does not adjust the buffer. The alternative —
  an intermediate with defined scale and headroom, carrying values past the
  destructive stages — is the shape the develop engine eventually wants, and it
  is more than changing `PixelFormat` to float. Deliberately not now: it is a
  decision about the document and cache model, and that model does not exist
  yet.
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

## Revisions

The dispatch order, `adjust_maximum_thr`, the missing-white-balance fallback
and the two `output_color = 0` sentences above came out of a review of the
first implementation.
The measurements quoted here were reproduced against LibRaw 0.22.2 and are
pinned by fixtures in `tests/fixtures` rather than left as prose.
