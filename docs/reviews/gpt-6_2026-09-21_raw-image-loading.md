# Review: RAW image loading and its rationale

Commit: `8079f839cfc7fdfb16326d3918d9685f7b78fbc4` — “First load RAW”.
Comparison: `git diff HEAD^...HEAD`, parent `c29e990a65ef3614b33cd40e8f9dac13dc710de2`.
Reviewer: GPT-6. Date: 2026-09-21.

Reviewed implementation, fixtures, tests, build integration, and ADR 005 against
AGENTS.md, desired-features.md, ADR 003, and the reimplementation plan. Standards
and specification were reviewed independently using the review skill; the main
review added runtime probes. No production code was modified. The pre-existing
staged/deleted `src/core/Utils.h` change is outside scope.

## Standards

### S1 — P3: Three Doxygen briefs use the wrong grammatical form

`src/core/RawImport.cpp:38`, `tests/test_RawImport.cpp:39`, and
`tests/test_RawImport.cpp:44` contradict AGENTS.md’s requirement for noun-form
class briefs and verb-form function briefs. `ProcessedImageDeleter` uses
“Releases…”, while `rampAt` and `maxDifference` use noun phrases. Suggested
wording: “Deleter for images allocated by…”, “Returns the generator’s sample
value at a column”, and “Returns the largest absolute difference…”.

No substantive standards violations found. The private decoder module, public
`arraw` namespace, standard-library usage, and RAII ownership fit the repository.
`just format-check` passes.

## Spec

### F1 — P1: Successful TIFF preview decoding bypasses RAW decoding

Location: `src/core/ImageImport.cpp:79` (the fallback only runs when Qt returns
a null image); related claim: `docs/adr/005-raw-import-through-libraw.md:34`.

ADR 005 promises that a misnamed RAW still loads and that machines with and
without KDE’s RAW plugin produce “identical pixels.” This assumes Qt’s TIFF
reader will reject every RAW container. A RAW can also contain an ordinary
RGB preview that Qt successfully reads, preventing the LibRaw fallback.

Reproduced with one synthetic DNG containing an 8×6 RGB preview in its first IFD
and the existing 32×24 linear RAW in a SubIFD:

| Same file contents | Qt plugins available | `loadImage` result |
|---|---|---|
| `preview.dng` | Normal system plugins | 32×24 RAW |
| `preview.png` | Normal system plugins | 8×6 preview |
| `preview.mrw` | Normal system plugins, including KDE RAW | 32×24 RAW |
| `preview.mrw` | TIFF plugin only | 8×6 preview |

This recreates the deployment-dependent behavior the ADR intends to eliminate,
and silently substitutes a preview for the full photograph. LibRaw decoded
the full image in every case when invoked directly.

Recommendation: distinguish RAW containers from ordinary TIFFs before accepting
a Qt TIFF decode, using actual RAW/container metadata. Expanding the extension
set closes the known-extension gap but does not solve renamed files. Add a
preview-bearing fixture and test with and without the RAW plugin; the current
single-IFD fixtures cannot expose this failure.

### F2 — P2: Per-frame brightness normalization remains enabled

Location: `src/core/RawImport.cpp:70`–`73`.

ADR 005 rejects a “content-dependent brightness stretch” so develop settings
remain consistent between frames. `no_auto_bright = 1` disables histogram
brightening, but leaves `adjust_maximum_thr` at its default 0.75. That separate
setting adjusts the white level using the frame’s actual maximum.

Reproduced with 32×24 Bayer DNGs declaring WhiteLevel 65535, each containing the
same neutral 16000 patch and a second, brighter neutral patch:

| Brighter patch | Imported unchanged 16000 patch, RGB |
|---|---|
| 48000 | 16000, 16000, 16000 |
| 52000 | 20164, 20164, 20164 |
| 60000 | 17476, 17475, 17475 |

Setting `adjust_maximum_thr = 0` in the diagnostic decoder retained 16000 in
all three cases. Explicitly disable this adjustment for the stated contract,
or adopt and document a different normalization policy. Add a fixture whose
maximum crosses the threshold; the current full-range ramp and the Bayer
fixture’s maximum of 48000 miss it. LibRaw documents the separate behavior in
its [processing notes](https://github.com/LibRaw/LibRaw/blob/master/doc/API-notes.html).

### F3 — P2: Missing camera white balance silently enables automatic WB

Location: `src/core/RawImport.cpp:82`–`84`.

The comment promises “never LibRaw’s guess,” but `use_camera_wb = 1` falls back
to automatic white balance when camera WB is absent, even with
`use_auto_wb = 0`. That changes colours according to scene contents.

Reproduced by removing AsShotNeutral from the synthetic coloured Bayer fixture:
the red/green/blue field becomes approximately neutral `(47999,48000,48000)`.
Selecting LibRaw’s explicit daylight fallback before opening the same file
produces `(41346,32922,17934)` instead.

Choose a fallback policy: daylight, a reported failure, or deliberately documented
automatic WB. For the existing no-guessing promise, use daylight or fail, and add
a missing-WB fixture. The behavior and fallback flag are documented in
[LibRaw’s data structures reference](https://github.com/LibRaw/LibRaw/blob/master/doc/API-datastruct.html).

### F4 — P3: The proposed camera-space escape hatch needs a conversion stage

Location: `docs/adr/005-raw-import-through-libraw.md:67`–`71`.

The ADR proposes `output_color = 0` and then says “None of those change the
output contract.” Zero produces camera-space values, whereas eight requests
Rec.2020. Preserving the contract requires an explicit camera-to-working-space
transform; simply changing this option and retaining `workingEncoding` would
mislabel the pixels. State that additional responsibility in the ADR.
See [LibRaw’s output colour definitions](https://github.com/LibRaw/LibRaw/blob/master/doc/API-datastruct.html).

## Assessment of the reasoning

Direct, required LibRaw linkage is a sound choice for a RAW editor. The small
private wrapper isolates decoding policy, keeps LibRaw out of the public API,
and owns resources correctly. Explicit working-space conversion and orientation
policy are useful contracts. Deferring metadata and lens correction is reasonable
for this first slice; they are expressly deferred, not missing implementation.

The weakest decision is the justification for clipping at import
(`src/core/RawImport.cpp:93`, ADR 005:57). Saying recovery is a later develop
decision does not preserve the information needed to make that decision.
White-balance scaling and colour conversion can clip valid sensor values before
the caller receives the buffer. Promoting that already-clipped buffer to float
later cannot restore them. LibRaw’s
[processing implementation](https://github.com/LibRaw/LibRaw/blob/0.22.0/src/postprocessing/postprocessing_utils.cpp)
clips during scaling and RGB conversion.

This is an acknowledged design limitation rather than an implementation mismatch,
but the argument that U16 necessarily requires `highlight = 0` is too strong.
With the committed warm-WB fixture, a diagnostic decode using `highlight = 1`
and the same 16-bit output retains an unclipped, differently scaled result.
That is not a complete replacement without defining the scale and remaining
gamut limits.

Two coherent paths are available:

- Keep the current image as a default rendering, and explicitly make WB,
  highlight handling, and demosaic changes trigger re-decoding. Retain the
  source and decode policy in the future document/cache model.
- Build an intermediate with defined scale and headroom, preserving values
  before destructive WB/colour clipping. This fits the eventual development
  engine better but requires more than changing `PixelFormat` to float.

The first is a reasonable incremental approach; the ADR should name highlight
handling alongside WB and demosaic as a reason to re-decode. Relative WB applied
to a clipped RGB buffer is not equivalent to changing RAW white balance.

## Verification and limits

- `just test`: 37/37 passed, including all eight RAW cases.
- `just format-check`: passed.
- Additional temporary probes linked the actual `build/debug/libarraw.a` and
  installed LibRaw 0.22.2. Their sources and generated inputs are in
  `/tmp/arraw-raw-review-uJAGNK/`; they are diagnostic artifacts, not committed tests.
- Plugin isolation used a temporary Qt plugin directory containing only the
  installed TIFF plugin; no installed files were modified.
- Probes reproduced F1–F3; F4 is documented API behavior. Existing tests passing
  does not cover these cases.
- Windows/macOS builds, real-camera format coverage, and licensing conclusions
  were not independently validated.

Standards: 1 minor finding (brief grammar); Spec: 4 findings, worst P1 (preview substituted for RAW).

---

## Resolution — 2026-09-21, Claude Opus 5

Every finding was reproduced before being fixed, each by a committed fixture
and a failing test rather than by a temporary probe. Measured with LibRaw
0.22.2 on Fedora 44, `kf6-kimageformats` installed.

| Finding | Resolution |
|---|---|
| F1 — preview bypasses RAW decoding | Fixed. `loadImage` now offers every file to LibRaw *before* Qt, and the post-Qt fallback is gone with nothing left to fall back to. New `preview-32x24.dng`, a preview IFD over a sensor sub-IFD, is loaded under `.png`, `.mrw` and `.tif`. |
| F2 — per-frame brightness normalization | Fixed. `adjust_maximum_thr = 0`. New `linear-32x24-highmax.dng` reproduced 16000 → 20164 exactly as reported. |
| F3 — missing camera WB enables auto WB | Fixed as daylight. `use_camera_wb` is switched off when `cam_mul` is absent, leaving the matrix's daylight multipliers: (41346, 32922, 17934), your figure. |
| F4 — `output_color = 0` escape hatch | Fixed in ADR 005: it returns camera space, so it holds the contract only with a conversion stage of arraw's own. |
| S1 — three brief forms | Fixed. |
| Clipping at import | ADR 005 now names highlight handling alongside WB and demosaic as a reason to re-decode, and records what clipping costs a later recovery, rather than claiming the information survives. |

Two things the report assumed that measurement did not support, both recorded
in ADR 005:

- **LibRaw does not open ordinary TIFFs.** 0.22.2 declined every multi-channel
  TIFF offered to it — arraw's own 8- and 16-bit exports, uncompressed and LZW
  RGB from Pillow, and an RGB TIFF carrying `Make`/`Model`. Only a *single-plane*
  16-bit TIFF with camera tags was claimed. That is what makes content-before-Qt
  safe, and it is now pinned by a test: an exported TIFF must reload through Qt.
- **Widening the extension set was therefore dropped.** With content deciding
  before Qt, the extension list covers nothing the content check does not, and
  `.raw` — a generic extension — would start reporting a RAW error for files Qt
  can read. The ten documented extensions stay as a fast path.

Not done, deliberately: the missing white balance is still substituted
silently. It wants a warning, and there is no channel to carry one yet; ADR 005
records it alongside the other cases that will want the same channel.
