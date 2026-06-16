#pragma once
#include <QRectF>
#include <QSize>

// Pure crop geometry — no widget or GPU state, so it unit-tests headlessly and
// stays the single source of truth shared by the export sizing and the crop
// overlay's live readout (CONTEXT.md: Crop, Aspect Ratio Lock).
namespace crop {

// Pixel dimensions of the kept region: the full-image pixel size scaled by the
// normalised crop rectangle, rounded to the nearest pixel. Matches what the
// export pipeline writes, so the overlay readout cannot disagree with it.
QSize cropPixelSize(int fullWidth, int fullHeight, const QRectF& cropRect);

// Largest rectangle of the given pixel aspect ratio (width:height) that fits
// inside `rect`, keeping its centre — the reshape applied when an Aspect Ratio
// Lock is selected (shrink-to-fit, never grows outside the current crop).
// `imageAspect` (full-image width/height) converts the pixel ratio into the
// non-square normalised-UV space the crop rectangle lives in, so a 1:1 lock on
// a 3:2 image is a tall UV rect, not a square one.
QRectF fitRatioInside(const QRectF& rect, double pixelRatio, double imageAspect);

// New crop rectangle while dragging a handle under an Aspect Ratio Lock. Handles
// are indexed TL=0, TC=1, TR=2, MR=3, BR=4, BC=5, BL=6, ML=7. Only the four
// corners resize (ratio-preserving, anchored at the opposite corner); the four
// edge handles are inactive while locked and return `startRect` unchanged.
// `pointer` is the dragged corner's target in normalised UV; the result holds
// the locked pixel ratio and still needs the caller's rotation/bounds gate.
QRectF lockedResize(
    int handle,
    const QRectF& startRect,
    const QPointF& pointer,
    double pixelRatio,
    double imageAspect);

// The Aspect Ratio Lock choices offered by the crop tool (CONTEXT.md).
enum class AspectPreset { Free, Original, Square, R2x3, R3x4, R4x5, R16x9 };

// The locked pixel ratio (width:height) for a preset in the given orientation.
// `landscape` true yields the wider orientation (ratio >= 1), false the taller.
// Original derives from `imageAspect`; Free returns 0.0, meaning "unlocked".
double presetRatio(AspectPreset preset, bool landscape, double imageAspect);

} // namespace crop
