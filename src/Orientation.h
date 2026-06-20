#pragma once

#include <QPointF>

// Coarse, lossless image orientation: one of the eight EXIF states (four 90°
// quarter-turns × an optional horizontal mirror). Distinct from the fine ±45°
// Rotation (CONTEXT.md, docs/adr/0025). Pure value type + EXIF mapping, so it
// unit-tests headlessly and is the single source of truth shared by the decode
// seed, the sidecar (tiff:Orientation), and the geometry remap.
namespace orient {

struct Orientation {
    int quarterTurnsCW = 0; // 0..3, applied before the mirror
    bool mirrored = false;  // horizontal mirror

    bool operator==(const Orientation&) const = default;
};

// EXIF / tiff:Orientation value (1..8) → canonical Orientation. An out-of-range
// value maps to the identity (treat unknown as "as shot").
Orientation fromExif(int exif);

// Canonical Orientation → EXIF / tiff:Orientation value (1..8).
int toExif(Orientation o);

// Map a point in the *oriented display frame's* UV [0,1]² to the UV it samples
// from in the *native buffer*. The inverse of the display transform (which is
// "rotate CW by quarterTurnsCW, then mirror horizontally if mirrored"), so it is
// rotateCCW^quarterTurnsCW ∘ mirrorH^mirrored. Exact — no resampling. This is the
// single source of truth for the coarse-orientation remap shared by the viewport
// overlay maths and the renderer.
QPointF orientedToBuffer(QPointF uv, Orientation o);

// True when the orientation swaps width and height (an odd quarter-turn), so
// callers must invert the image aspect for fitting and the ±45° rotation.
bool swapsAspect(Orientation o);

} // namespace orient
