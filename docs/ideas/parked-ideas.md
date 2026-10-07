# Parked ideas

Ideas the user has noted and wants kept in view, deliberately not part of any
plan yet (see `architecture-roadmap.md` for what is planned). Each entry gives
its date and anything already known that bears on it.

## Import settings from Lightroom, best effort (2026-10-07)

Read a photograph's Lightroom develop settings (`crs:` in its XMP sidecar, or
embedded in a DNG or JPEG) and turn whatever maps onto arraw's settings,
saying what did not map.

What bears on it:
- `docs/desired-features.md` already intends to map develop settings to
  Lightroom's `crs:` dialect, so that most of them survive the round trip.
  Import is the reading half of that, and could come first.
- The sidecar code already leaves other namespaces alone, so a `crs:` block
  next to arraw's is kept today.
- arraw's tone, colour and grading controls are not Lightroom's maths (for
  example the Oklab hues of ADR 034, and the perceptual tone curve of ADR 033),
  so values would be translated approximately, not copied. Hence "best effort".
- The library-owned edit operations of step (a) of the roadmap, which apply a
  partial state through the compound rules, are the natural place to apply an
  imported look.

## Brainstorm, not yet reviewed (2026-10-07)

Proposed by the assistant and parked on the user's word. None has been weighed
by the user yet beyond "keep them". The notes say how each fits what exists.
The assistant's own picks for value against effort: the clipping overlay,
before/after, export recipes and naming, perspective correction, and burst
grouping with a compare view.

### Seeing the photo better

- **Before/after:** a split view or a held key. The preview pipeline needs
  restructuring for it (the architecture review).
- **Clipping overlay (`J`) and highlight/shadow warnings:** one more step in
  the per-pixel processing, cheap.
- **Focus peaking and a sharpness score** for culling, from the pyramid level
  the thumbnails use.
- **Soft proofing:** preview through the export profile and flag the colours it
  cannot show.
- **A 1:1 loupe under the cursor** while culling.
- **False-colour exposure map**, as in video work; it suits the scene-referred
  maths.

### Culling faster

- **Group bursts and near-duplicates** by capture time and similarity, and pick
  the best of each.
- **Compare view:** two to four shots side by side, with zoom and pan kept in
  step.
- **Blink and out-of-focus flags.** The focus part needs only a sharpness
  measure, no AI.

### Colour and tone

- **DCP camera profiles**, for the camera maker's colour. This matters for the
  Lightroom import too.
- **Film emulation looks** built from the existing curves, grading and grain,
  shipped as presets.
- **Grey-card neutralise**, and white balance spread over several picked
  points.
- **Auto tone**, a best-effort starting point.
- **Match the look of a reference photograph**: grading and tone fitted from one
  frame to another.
- **Display-HDR export** (gain-map JPEG or AVIF). The pipeline is
  scene-referred, so the headroom above white survives until the output.

### Detail and repair

- **Demosaic choices, and noise reduction before demosaicing** for high-ISO
  work.
- **Chromatic aberration and fringe removal without a lens profile.**
- **Perspective correction** (keystone, and auto-upright from lines), extending
  the geometry plan.
- **Content-aware spot healing**, beyond cloning; still no layers.

### Workflow

- **Watch a folder**, tethered or a card, developing new shots with a preset as
  they land.
- **Export recipes:** named export settings ("web 2048 px sRGB", "print TIFF
  16-bit"), shared by the GUI, the CLI and Python through the services layer
  (roadmap step (b)).
- **Export naming templates** (`{date}_{seq}_{rating}`), with batch export.
- **A contact sheet or proof PDF** of a selection.
- **A sidecar diff and merge** between two photographs' edits, cheap once the
  library's edit operations and the history view exist.
- **Python notebooks:** Jupyter display of renders and an interactive slider
  widget, on top of Python's GPU access.

### Interoperability

- **DNG export**, with the edits as embedded settings or baked into a linear
  DNG.
- Writing `crs:` and editing in GIMP or Krita moved to the interoperability
  section below.

## Interoperability with editors and asset managers (2026-10-07)

The user wants arraw to work well with external editors (GIMP, Krita, Affinity
Photo, open source or not) and with open-source asset managers. Guidance and
ideas from the assistant, parked as a whole.

**Ground rule: files are the interface.** Other programs never call our API,
so working with them means following their file conventions, never surprising
them, and offering a few ways in.

### Principles

- **Never write to a RAW.** Exports and derived files get names of their own.
- **Standard metadata first.** Marks, keywords, titles and GPS go in the
  standard `xmp:`, `dc:`, `photoshop:` and `lr:` fields; arraw's own
  namespace is only for develop settings.
- **Read leniently, write conservatively, keep everything else.** The sidecar
  already keeps other namespaces. Extend that to the order and formatting of
  other programs' blocks, so a darktable or digiKam sidecar comes out unchanged
  but for arraw's block.
- **An "interop contract" ADR**, with real sidecars from Lightroom, digiKam,
  darktable and Bridge as test fixtures, checked both ways.

### Asset managers

This covers digiKam, darktable's library, Shotwell, XnView MP, gThumb,
PhotoPrism and Immich, and Lightroom and Bridge too.

- **Sidecar naming.**
  - Adobe uses `IMG_1234.xmp`, which is ADR 019's choice.
  - darktable, and digiKam optionally, use `IMG_1234.ARW.xmp`.

  Read both, write one as a setting, and never leave two sidecars that
  disagree.
- **Mark mappings differ; translate them.**
  - Colour labels: `xmp:Label` text for Adobe, `digiKam:ColorLabel`,
    `darktable:colorlabels`.
  - Reject: rating −1 in Lightroom, a flag elsewhere.
  - Pick: several conventions.
  - Keywords: `dc:subject` flat, plus `lr:hierarchicalSubject` and
    `digiKam:TagsList` for hierarchies.
- **Notice other programs' changes.** A DAM may change marks or keywords while
  arraw is open. Reload marks quietly through the folder watcher, and ask when
  a develop edit is unsaved.
- **Developed previews are the gap:** DAMs show the camera's embedded JPEG,
  never arraw's edits. Options:
  - write `crs:` settings, the other half of the Lightroom import above; a few
    DAMs half-understand them;
  - an optional preview JPEG beside the RAW;
  - **`arraw-cli` as a drop-in RAW converter.** PhotoPrism shells out to
    `darktable-cli` or `rawtherapee-cli` through a configurable command, so a
    compatible invocation would let it render arraw's edits.
- **Group derivatives with their RAW.** Write `xmpMM:DerivedFrom`,
  `xmpMM:DocumentID` and `InstanceID` into exports, so digiKam's versioning
  and other DAMs keep them with the source.

### External editors

This covers GIMP, Krita, Affinity Photo and Photoshop.

- **An "Edit in…" round trip:**
  - render the current state to a TIFF beside the RAW (`IMG_1234-edit.tif`),
    with an embedded ICC profile and the metadata;
  - launch the editor through a configurable command template (`{file}`),
    with built-in entries detected per platform:
    - Flatpak: `flatpak run org.gimp.GIMP`;
    - Windows: the registry;
    - macOS: `open -a`;
  - watch the file, and show it in the strip as a companion of the shot once
    saved;
  - record in its XMP which RAW and which develop state it came from, so arraw
    can flag the derivative as **stale** after the RAW's edits change.
- **Output per editor:**
  - 16-bit TIFF in a wide gamut (ProPhoto or Rec.2020) by default, which is
    safe everywhere;
  - 32-bit float linear TIFF, or EXR, for Krita and GIMP 3, which are
    float-native. It suits the scene-referred pipeline: the headroom above
    white survives the trip.
- **Bringing results back:**
  - Flattened TIFF, PNG and JPEG already open, with their own sidecar.
  - OpenRaster (`.ora`, Krita and GIMP) contains a flattened
    `mergedimage.png` that is easy to read.
  - PSD, KRA and `.afphoto` are shown as companions with "open in editor"
    only.
- **arraw as GIMP's RAW loader.** GIMP 3 opens RAWs through a plug-in that
  calls an external converter (darktable and RawTherapee ship one). An arraw
  plug-in calling `arraw-cli` would put arraw's pipeline and sidecar behind
  "open RAW in GIMP". Krita uses libraw directly, so this mostly helps GIMP.

### Desktop integration

- **"Open with":**
  - Linux: a `.desktop` file listing the RAW MIME types;
  - Windows: file associations;
  - macOS: `CFBundleDocumentTypes`.
- **Single instance:** a second file opened from a DAM or a file manager goes
  to the running window (`QLocalServer`), not to a second app.
- **A documented command line:** `arraw-ui path/to/file` already opens the file
  and shows its folder, so a DAM's "edit with" setting just works.

### Where it would fit

- **Sidecar naming, the mark mappings and the interop ADR** belong with the
  sidecar work: roadmap step (c)'s structured sidecar format, or a small step
  of their own before it.
- **"Edit in…" and derivative grouping** sit on step (b)'s export batch and
  naming.
- **The `arraw-cli` converter mode and the GIMP plug-in** need a stable CLI,
  which step (b)'s cleanup gives.
