# Thumbnails start from embedded previews and half-size decodes

The film strip's thumbnails (docs/ideas/filmstrip-plan.md, step 5) show the
camera's embedded JPEG first, then arraw's own rendering of the saved settings
(about 512 px). A full decode of a RAW costs a demosaic of every pixel for a
picture of a few hundred; the embedded preview costs a read. This ADR is the
library's part; the app's (worker, disk cache) adds to it.

## Decision

**`readEmbeddedPreview(path, maxEdge, log)`** returns the camera's own preview
as an `ImageBuffer`, or nothing.

- exiv2 finds it (`Exiv2::PreviewManager`), so it covers the embedded JPEGs of
  RAW files and the EXIF thumbnails of JPEG and TIFF alike, through the same
  dependency and the same initialisation as ADR 028 (now `Exiv2Support`).
- Of the previews a file holds, the smallest whose longer edge is at least
  `maxEdge` is used, else the largest; `maxEdge == 0` asks for the largest as
  it is. If the chosen one will not decode, the next best is tried.
- Qt decodes it (`QImageReader`), converts any embedded profile to sRGB and
  returns 8-bit RGBA in `NamedEncoding::Srgb` with no pending orientation. It
  is a camera's picture, not a development, so it is not put in the working
  encoding: 8 linear bits would crush its shadows, and nothing downstream
  develops it.
- It is turned upright by the file's `Exif.Image.Orientation`, because RAW
  previews are stored as the sensor was read. A preview whose own JPEG carries
  an orientation tag is trusted and turned by Qt alone, never twice. Main did
  not need this, as it showed a preview raw; the strip shows it beside
  developed ones, which are upright.
- Reduced to fit `maxEdge` (smooth, before turning, never enlarged).
- No preview: `nullopt`, no notice, as a PNG having none is no event. A file
  that cannot be read: `nullopt` and `Notice::PreviewUnreadable` (warning).
  Thread-safe, as `readExif` is.

**`DecodeOptions::halfSize`** (`loadImage(path, log, {.halfSize = true})`) makes
LibRaw combine each 2x2 sensor block into a pixel rather than demosaic. The
result is an ordinary decode, with the same encoding, orientation and applied
multipliers, at `size / 2` per side (rounded down, as LibRaw does), so
`develop()` takes it unchanged. Two consequences:

- **Plan against the buffer.** The size `readImageMetadata` declares is the
  full one. Crops are normalised, so geometry is the same shape; `planFor(Photo)`
  would not be, so a small render uses `planFor(buffer, ...)`.
- **Every RAW is halved, not only Bayer ones.** LibRaw halves only what it
  would demosaic, so a linear DNG (already demosaiced) comes back whole. The
  loader then averages 2x2 blocks itself, so the contract holds for any file
  and a caller need not look at the size to know. There is no speed-up in that
  case, as there was nothing to skip.
- Ignored for files that are not RAW. They decode in full; `halved` reduces
  them where wanted, and a different buffer format after the call would be a
  surprise.

In Python: `read_embedded_preview(path, max_edge)` and `load(path,
half_size=False)`.

**The strip's thumbnails are built on these two** (`src/app/ThumbnailWorker`,
`ThumbnailCache`):
- **One worker thread, at low OS priority and CPU only.** The preview owns the
  GPU, and a thumbnail must never slow a slider.
- **Order:** for each shot, the embedded preview first, then arraw's rendering of
  the *saved* state (half-size decode, pyramid, `FitInside{512, 512}`). The
  visible shots go first, all embedded previews come before any developed
  thumbnail, and a changed sidecar jumps the queue. A new folder cancels
  everything queued.
- **Disk cache:** JPEG files under the XDG cache directory. The key is
  SHA-256 of the path, size and modification time, plus the saved settings for
  a developed thumbnail. Writes are atomic, entries are pruned oldest-first
  beyond 512 MB at start-up, and a corrupt entry is replaced.
- **The open photograph's thumbnail follows its unsaved edits** from the
  preview, and is never cached. Saving re-renders the saved state through the
  worker; leaving the photograph returns the cell to the cached thumbnail.

## Consequences

- A half-size develop differs slightly from a full one, reduced: the tone chain
  is not linear and averaging happens before it rather than after, and the
  demosaic is not run. The tests pin it within 0.005 of full scale on the
  fixtures (measured 8.4e-4 on linear ramps; the Bayer fixture is flat, so 0).
  That is for a thumbnail; export and the preview never use it.
- The thumbnail the strip shows first and the one it shows after can differ in
  colour (the camera's processing against arraw's); this is inherent and
  accepted in the plan.
- There is no fixture with several previews, so the choice among them is
  specified here but not pinned by a test; fixtures carry at most one.

## Note, 2026-10-05

A half-size decode has a pixel scale of 2 (`ImageBuffer::pixelScale`), and its
halvings 4, 8 and so on, so noise reduction on a thumbnail shrinks its reach to
match ([ADR 039](039-noise-reduction-is-the-first-pass-and-reads-the-as-shot-luminance.md)). A photograph with no recorded develop state is developed, and
its thumbnail cached, with `defaultStateFor` its kind. The worker reads the
file's header to tell a RAW, which costs a header parse but no decode, also on
a cache hit.
