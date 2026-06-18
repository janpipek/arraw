# Filmstrip EXIF tooltips cached as JSON sidecars

The filmstrip tooltip is growing from just the filename to capture date/time,
camera + lens, the exposure triad (ISO / shutter / aperture / focal length), and
pixel dimensions. That data comes from `extractMetadata`, which needs only
LibRaw `open_file()` — a cheap header parse, **not** the `unpack()` + demosaic
stages. `ThumbnailCache::decodeEmbeddedThumb` already calls `open_file()` on
every file, so we **piggyback**: capture the metadata on that same open and
persist it as a small JSON **sidecar** at `~/.arraw/cache/<hash>.json`, beside
the thumbnail's `<hash>.jpg` and under the identical `SHA256(path|size|mtime)`
key. Same key ⇒ same automatic invalidation (a modified file misses and
regenerates). The tooltip reads the sidecar, so it is instant and opens no files
on hover for any already-thumbnailed frame. This extends the file-per-entry
cache of [ADR 0016](0016-developed-thumbnail-and-decode-caches.md).

## Considered Options

- **A single SQLite cache db** (the "little db" idea). One queryable file instead
  of thousands of sidecars, but it adds Qt SQL + sqlite as a dependency and would
  be the app's first database — edging toward the catalogue/DAM that `CONTEXT.md`
  explicitly says arraw is **not** ("there is no index or database"). Rejected:
  too heavy for a hover-tooltip cache, and against the no-database stance.
- **In-memory only** (a session `QHash`). Simplest, but once a thumbnail is
  disk-cached the RAW is never re-opened (`loadFromDisk` reads the JPEG), so the
  metadata would never be recaptured after a restart and tooltips would be empty.
  Defeats piggybacking across sessions. Rejected.

## Consequences

- **Lazy backfill is required, not optional.** Thumbnails cached by earlier
  versions have a `<hash>.jpg` but no `<hash>.json`, and piggybacking alone never
  re-opens them. So a missing sidecar on tooltip request triggers one cheap async
  `open_file` + `extractMetadata` that writes the sidecar; every later hover is a
  cache hit. Hover can therefore cause at most one (cheap, async, once-ever) file
  open per frame.
- **Non-RAW files (JPEG/PNG/TIFF) are deferred.** They take the `QImage(path)`
  fallback, which exposes no EXIF, and LibRaw will not open them. Their tooltip
  shows only filename + pixel dimensions. RAW-grade EXIF for these would need a
  separate reader (e.g. exiv2) and is left as future work.
- `~/.arraw/cache` still has **no eviction**; a JSON sidecar is tiny (well under a
  KB) and one-per-file, so it does not change the growth story ADR 0016 already
  left for the deferred disk-half-res work.
- These are caching/pipeline internals, not domain language: `CONTEXT.md` is
  untouched.
