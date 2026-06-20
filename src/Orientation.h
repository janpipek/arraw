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

// Inverse of orientedToBuffer: map a native-buffer UV back to the oriented
// display frame. Used by the viewport's buffer→screen mapping so overlays stay
// in lock-step with the shader (docs/adr/0025).
QPointF bufferToOriented(QPointF uv, Orientation o);

// True when the orientation swaps width and height (an odd quarter-turn), so
// callers must invert the image aspect for fitting and the ±45° rotation.
bool swapsAspect(Orientation o);

// The orientation after turning the *displayed* image 90° clockwise /
// counter-clockwise. A mirror reverses the turn direction, so this is not a
// plain increment of quarterTurnsCW (docs/adr/0025).
Orientation turnedClockwise(Orientation o);
Orientation turnedCounterClockwise(Orientation o);

// The orientation after mirroring the displayed image horizontally (or, when
// horizontal is false, vertically — a horizontal mirror plus a 180° turn).
Orientation flipped(Orientation o, bool horizontal);

} // namespace orient
