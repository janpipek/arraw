# Plan — Square filmstrip thumbnails + EXIF tooltips

Branch: `square-thumbnails`. See
[ADR 0019](../adr/0019-filmstrip-exif-tooltip-cache.md) for the caching decision.
TDD: each step is red → green → refactor.

## Part A — Square geometry

1. **`filmstrip::cellWidth` → always square.**
   - *Red:* rewrite `tests/test_FilmStripLayout.cpp` to assert `cellWidth(h, any)
     == h` (drop the aspect-ratio cases; keep the `h <= 0 → 0` guard). Possibly
     rename to `cellSide`/inline it, since width no longer depends on image size.
   - *Green:* `cellWidth` returns `contentHeight` (square cell).
   - The delegate's `KeepAspectRatio` fit into `inner` is already correct — a
     landscape image letterboxes inside the square automatically. No paint change
     needed for fit.

2. **Widen the inset.** `kCellPad` 4 → 6 in `FilmStrip.cpp`. Neighbours land ~12px
   apart. Confirm the selected border fits inside the inset without touching the
   image.

3. **`setUniformItemSizes(true)`** in the `FilmStrip` ctor — all cells are now
   identical, so this is valid and a perf win. Remove the "cell width varies with
   aspect" comment.

## Part B — Selection border

4. **Widen + keep opacity tiers.** `kBorderWidth` 2 → ~3–4. Active = full-opacity
   highlight; batch-selected = same width/colour at reduced alpha (the as-merged
   scheme, just bolder). Unselected stays transparent. Largely a constant bump in
   the existing `paint()` border block (FilmStrip.cpp:130–145).
   - Verification is visual (Jan runs the app); no headless test for the look.

## Part C — EXIF tooltip

5. **Metadata serialisation.** Add `ImageMetadata` ⇄ JSON (the existing
   `QVector<QPair<QString,QString>> rows` maps cleanly to a JSON object/array).
   - *Red:* `test_ImageMetadata` round-trips a populated `ImageMetadata` through
     JSON and back, byte-stable.
   - *Green:* `toJson` / `fromJson` free functions (QJsonDocument).

6. **Metadata cache (sidecar).** Extend `ThumbnailCache` to store/load
   `<hash>.json` beside `<hash>.jpg`, same `cacheKey`.
   - *Red:* a test writes metadata for a fixture path, reads it back; a
     size/mtime change misses (mirror the existing thumbnail-cache key tests).
   - *Green:* `storeMetadata(path, meta)` / `loadMetadata(path) -> std::optional`.

7. **Piggyback capture.** In `decodeEmbeddedThumb` (the RAW branch only), call
   `extractMetadata` on the open handle and write the sidecar. Non-RAW fallback
   writes nothing (deferred). Add the `// TODO: non-RAW EXIF (exiv2)` comment at
   the `QImage(path)` fallback site, per ADR 0019.

8. **Lazy backfill + tooltip text.** Tooltip request path:
   - Look up the sidecar; if missing, async `open_file` + `extractMetadata`,
     write sidecar, then refresh the tooltip.
   - Build the tooltip string from cached rows: filename, capture date/time,
     camera + lens, ISO/shutter/aperture/focal length, dimensions; omit absent
     lines. Rating/label intentionally excluded (already shown as overlays).
   - *Red:* a pure formatter `tooltipText(filename, ImageMetadata) -> QString`
     test (omits empty fields, orders rows). Keep the I/O (async read, QToolTip
     re-show) thin and outside the tested unit, per the headless-math convention.
   - Wire `Qt::ToolTipRole` in `FilmStripModel` (or a delegate `helpEvent`) to the
     formatter; replace the current filename-only tooltip.

## Out of scope / deferred
- RAW-grade EXIF for non-RAW formats (JPEG/PNG/TIFF) — needs a separate reader.
- Cache eviction for `~/.arraw/cache` — unchanged, still deferred (ADR 0016).
