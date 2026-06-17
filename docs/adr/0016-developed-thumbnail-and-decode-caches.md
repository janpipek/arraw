# Developed thumbnails and an in-session decode cache

`ThumbnailCache` was a single disk cache of **camera-embedded** JPEGs (≤512px from
LibRaw `unpack_thumb`), keyed `path|size|mtime`. That is changing in two ways, and a
second, in-memory cache is being added alongside it, to kill loading glitches and the
multi-second re-open delay.

**1. Thumbnails reflect develop edits.** The *current* image's filmstrip thumbnail is
regenerated through the actual develop pipeline — the offscreen render path with
`baseLook=true`, at ≤512px, read back as an output-referred sRGB JPEG that
**overwrites** the existing `ThumbnailCache` entry in place. It is debounced ~750ms
after the last param change, so the filmstrip updates live while you edit. The cache
key is unchanged (`path|size|mtime` of the *raw*): edits live in the `.xmp` sidecar,
not the raw, so the key is stable across edits and the latest develop state simply
overwrites the previous thumbnail (last-write-wins, eventually consistent). Develop
params are deliberately **not** part of the key. Only the open image is re-developed;
unopened files keep their camera-embedded thumbnail until opened. Whole-folder
preview generation (Lightroom's "Standard Previews") is explicitly out of scope.

**2. An in-memory decoded-result cache.** A session-scoped MRU keyed `path|size|mtime`
holds whole `LoadResult`s (fullRes + preview + metadata + defaultCrop), bounded by a
~1.5GB byte budget with LRU eviction and the current image pinned. `loadImage` looks
it up first; a hit skips the background decode entirely, so re-navigation is instant
*and* correct (it jumps straight to the demosaiced image, never the camera-baked
stages). It caches **pixels only** — the sidecar is always re-read, so external edits
are never masked.

**3. The load sequence reads the sidecar before the first paint.** The new image's
XMP is loaded synchronously at the top of `loadImage`, the previous image is left on
screen untouched, and pixels + params are swapped **atomically** on the first
new-image frame. This removes the glitch where a new image's first paint wore the
*previous* image's exposure/crop. The synchronous 512px thumbnail paint is dropped
from the open path (the embedded preview, up to 2048px, supersedes it), and the
embedded preview is exposure-normalised to the same 0.78 target as the demosaic so
the brightness no longer pops between stages.

## Considered Options

- **Keep camera-embedded thumbnails.** Simple and fast, but the filmstrip then lies
  about what the photo looks like after editing — the whole point of culling is to
  judge developed frames. Rejected.
- **Regenerate thumbnails on navigate-away / app close** instead of debounced.
  Fewer renders, but it forces capturing the *outgoing* image's buffer and params
  mid-switch — more error-prone — and gives no live update. Rejected; a 512px
  offscreen render is cheap enough to run debounced on the current image.
- **A disk cache of the half-res preview** (the original "keep the other resolutions"
  idea). Deferred, not rejected: the half-res preview is the buffer the shader
  pipeline *edits* at fit-zoom, so it cannot be stored lossy (8-bit clips the >1.0
  highlights and the wide gamut, and bands under exposure pushes). A disk version
  needs 16-bit half-float + `qCompress` (no new dependency) and real LRU eviction of
  `~/.arraw/cache`, which is more work than the in-memory cache that solves the
  within-session case. Start in memory.

## Consequences

- The `ThumbnailCache` comment ("Generates up-to-512px JPEGs from embedded RAW
  previews") no longer tells the whole story — thumbnails for opened images come from
  the develop pipeline via offscreen RHI, not `unpack_thumb`.
- A cache hit and a finished decode must converge on the **same** path: both feed an
  `applyLoadResult()` that sets pixels, re-reads the sidecar, and restores exif —
  keep them unified so they cannot drift.
- `~/.arraw/cache` still has **no eviction**. Developed thumbnails overwrite in place
  (~50KB, one per file), so growth stays modest; the unbounded-growth problem is left
  for the deferred disk-half-res work, where ~20MB blobs make it bite.
- These are pipeline/caching internals, not domain language: `CONTEXT.md` is
  untouched.
