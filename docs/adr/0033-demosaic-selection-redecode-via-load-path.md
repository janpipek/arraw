# Demosaic selection re-decodes via the load path; LGPL built-ins only

Arraw decodes every RAW with libraw's default demosaic (AHD) and has never
exposed a choice. Issue #22 asks for **Demosaicing Selection** — "AMaZE for
detail, RCD or AHD for noise" — so a high-ISO frame and a high-detail landscape
can be decoded differently. This milestone ships the *selection*, but over a
deliberately narrower algorithm set than the issue names, and routes the change
through the image **load** path rather than the live shader, because demosaic is
the one develop control that is not a shader operation.

## Algorithm universe: LGPL built-ins only

The vcpkg `libraw` port is **0.22.1, LGPL, built with only the `dng-lossy` and
`openmp` features** — no GPL demosaic-pack. So the reachable algorithms are the
ones `dcraw_process()` produces from `imgdata.params.user_qual`:

| Label | `user_qual` | Character |
|---|---|---|
| **AHD** *(default)* | 3 | Balanced; today's behaviour |
| **VNG** | 1 | Smooth, noise-tolerant, softer |
| **PPG** | 2 | Fast, sharper than VNG, some maze artifacts |
| **DCB** | 4 | Detail-oriented, occasional artifacts |
| **DHT** | 11 | High detail, slower, noisier on high-ISO |
| **AAHD** | 12 | Aliasing-aware AHD variant |
| **Linear** | 0 | Bilinear baseline/diagnostic |

**AMaZE** lives in libraw's GPL3 demosaic-pack (not built here) and **RCD is not
in libraw at all** — both of the algorithms the issue names by example are
unreachable without either vendoring a GPL pack (a licensing obligation on the
distributed binary plus a custom overlay port) or porting a demosaicer
ourselves. We chose the built-in set: it delivers the issue's actual value
(different decode for detail vs noise) with **zero new dependencies and no
licence change**, and AMaZE/RCD remain a documented later extension if the
curated set proves insufficient. AHD stays the default, so existing edits and
the golden-image suite (`0005`) do not move.

The persisted value is a **stable string token** (`arraw:DemosaicAlgorithm`,
e.g. `"AHD"`), not libraw's integer — the string↔`user_qual` mapping lives in
one tested function, so the sidecar never silently changes meaning if libraw
renumbers or we drop an entry. An absent or unrecognised token falls back
**silently to AHD** (the crop-preset precedent in `0021`: re-derive, don't
error). It is `arraw:`-namespaced rather than `crs:` because Lightroom has no
equivalent property, so there is no round-trip to protect ([[XMP Property
Ownership]]). It travels in the **Detail** develop group alongside Sharpen and
Colour Noise Reduction.

## Why the load path, not the shader

Every other develop adjustment is a real-time shader uniform. Demosaic is the
**decode itself** — `RawProcessor::load` / `dcraw_process` — upstream of the
decoded `ImageBuffer` that lens correction (`0027`), spots (`0017`), and the
whole shader chain operate on. Changing it cannot be a uniform tweak; it must
**re-run the multi-second decode**.

So a demosaic change is routed through the existing async load machinery
(`QtConcurrent::run` + `loadWatcher`), keeping the current image on screen until
the new decode swaps in atomically — exactly how image-to-image loading already
behaves, minus the embedded-preview flash (a full image is already up). It is
still a normal develop edit: on the undo stack, marks the session dirty, writes
the token. Undo/redo of it re-decodes too.

To make this bearable — and to make A/B comparison pleasant — the **decode cache
key gains the algorithm token** (`path|size|mtime|algo`, was `path|size|mtime`).
Each algorithm's decode is cached independently, so only a *never-tried*
algorithm pays the decode cost; switching among tried algorithms (or undoing) is
instant. The LRU budget (`0024`) is unchanged; the current image's algorithm is
the pinned entry, others are evictable.

The demosaic token must be resolved from the sidecar **before** decode (it
parameterises the decode). This is free: `pendingPreviewParams` is already
resolved from the sidecar up front, before the decode is dispatched.

### Considered and rejected

- **Decode all algorithms eagerly** — wasteful (7× decode time and memory per
  image for choices the user will never make).
- **Apply the choice only at export** — breaks WYSIWYG; the preview would not
  show what you are choosing.
- **Vendor the GPL demosaic-pack for AMaZE** — licence obligation on the
  shipped binary and a custom overlay port, and still no RCD. Deferred.

## Non-Bayer sensors: disable, don't mislead

The seven algorithms are **Bayer** algorithms. `imgdata.idata.filters` is known
the moment the file opens and tells us the mosaic:

- **X-Trans** (`filters == 9`): libraw silently *reinterprets* `user_qual` as
  Markesteijn 1-pass/3-pass — picking "DCB" on a Fuji file does not run DCB.
- **Foveon / already-demosaiced / monochrome** (`filters == 0`): no mosaic to
  interpolate.
- **Standard-image companions** (JPEG/TIFF via `StandardImageLoader`): no
  demosaic stage exists at all.

On these the control is shown **disabled with a short explanation** ("this sensor
uses its own decode") rather than offering Bayer labels that would not apply —
honesty matters most precisely for the "high-end developer" this feature targets.
A proper **X-Trans (Markesteijn 1-pass/3-pass) menu** is a deferred follow-up:
real value for Fuji shooters, but a separate sensor-specific design.

## Consequences

- **A develop adjustment is coupled to the load subsystem** for the first time.
  `RawProcessor::load` and `decodeImage` gain an algorithm parameter; the change
  handler dispatches a re-decode instead of a re-render. This is the surprising
  bit a future reader will hit — it is deliberate, because demosaic genuinely is
  a decode-time choice, not a pixel operation.
- **Batch export and the headless CLI must thread the per-image token through**
  (`decodeImage` at the export call site), or an exported file would silently
  differ from the on-screen preview. The token is read from the sidecar like any
  other develop field.
- **Geometry is unaffected.** All algorithms output the same dimensions, so
  `defaultCrop`, orientation seeding, and the preview downsample are untouched —
  unlike Distortion (`0027`), a demosaic change never refits the crop.
- **Memory.** Caching multiple algorithms per image multiplies decoded-buffer
  footprint, but only for algorithms actually tried, and the existing LRU budget
  bounds it; the pinned current decode is never evicted.
- **Reset semantics.** Resetting (or pasting an unedited) Detail group returns
  demosaic to AHD, which triggers a re-decode back to the AHD buffer — cheap if
  that buffer is still cached.
- **Testable core is pure.** The string↔`user_qual` mapping, the cache-key
  composition, and the sidecar round-trip are unit-testable without a GPU or a
  RAW file; the visual difference between algorithms is verified by eye, not in
  the golden suite (where AHD-as-default keeps existing goldens stable).
