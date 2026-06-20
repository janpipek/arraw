#include "CropGeometry.h"

#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace crop {

QSize cropPixelSize(
    int fullWidth, int fullHeight, const QRectF& cropRect, orient::Orientation orientation) {
    // The crop is normalised in the oriented display frame, so an odd quarter-turn
    // presents the buffer with its width and height swapped (docs/adr/0025).
    if (orient::swapsAspect(orientation))
        std::swap(fullWidth, fullHeight);
    return {int(fullWidth * cropRect.width() + 0.5), int(fullHeight * cropRect.height() + 0.5)};
}

QRectF fitRatioInside(const QRectF& rect, double pixelRatio, double imageAspect) {
    // Target width/height ratio in the non-square UV space the crop lives in.
    const double uvRatio = pixelRatio / imageAspect;
    double du = rect.width();
    double dv = du / uvRatio;
    if (dv > rect.height()) {
        dv = rect.height();
        du = dv * uvRatio;
    }
    const QPointF c = rect.center();
    return {c.x() - du / 2.0, c.y() - dv / 2.0, du, dv};
}

QRectF lockedResize(
    int handle,
    const QRectF& startRect,
    const QPointF& pointer,
    double pixelRatio,
    double imageAspect) {
    // Anchor = the corner opposite the dragged one; signs place the dragged
    // corner on its own side so it never flips across the anchor.
    QPointF anchor;
    double sx = 0.0;
    double sy = 0.0;
    switch (handle) {
    case 0:
        anchor = startRect.bottomRight();
        sx = -1.0;
        sy = -1.0;
        break; // TL
    case 2:
        anchor = startRect.bottomLeft();
        sx = +1.0;
        sy = -1.0;
        break; // TR
    case 4:
        anchor = startRect.topLeft();
        sx = +1.0;
        sy = +1.0;
        break; // BR
    case 6:
        anchor = startRect.topRight();
        sx = -1.0;
        sy = +1.0;
        break; // BL
    default:
        return startRect; // edge handles are inactive while locked
    }

    const double uvRatio = pixelRatio / imageAspect;
    const double dx = pointer.x() - anchor.x();
    const double dy = pointer.y() - anchor.y();
    // Drive by whichever axis demands the larger rect so the corner keeps up
    // with the pointer in the dominant direction, then derive the other side.
    const double du = std::max(std::abs(dx), std::abs(dy) * uvRatio);
    const double dv = du / uvRatio;

    const QPointF dragged(anchor.x() + sx * du, anchor.y() + sy * dv);
    return QRectF(anchor, dragged).normalized();
}

double presetRatio(AspectPreset preset, bool landscape, double imageAspect) {
    double longShort = 1.0; // longer side / shorter side, always >= 1
    switch (preset) {
    case AspectPreset::Free:
        return 0.0;
    case AspectPreset::Original:
        longShort = std::max(imageAspect, 1.0 / imageAspect);
        break;
    case AspectPreset::Square:
        longShort = 1.0;
        break;
    case AspectPreset::R2x3:
        longShort = 3.0 / 2.0;
        break;
    case AspectPreset::R3x4:
        longShort = 4.0 / 3.0;
        break;
    case AspectPreset::R4x5:
        longShort = 5.0 / 4.0;
        break;
    case AspectPreset::R16x9:
        longShort = 16.0 / 9.0;
        break;
    }
    return landscape ? longShort : 1.0 / longShort;
}

double cropPixelRatio(const QRectF& cropRect, double imageAspect) {
    if (cropRect.height() <= 0.0)
        return 0.0;
    return cropRect.width() / cropRect.height() * imageAspect;
}

PresetMatch matchPreset(double pixelRatio, double imageAspect) {
    if (pixelRatio <= 0.0)
        return {AspectPreset::Free, true, true};
    // Named ratios win over Original, which only coincides with one when the
    // image itself is that shape (then the explicit name is the clearer label).
    const AspectPreset presets[] = {
        AspectPreset::Square,
        AspectPreset::R2x3,
        AspectPreset::R3x4,
        AspectPreset::R4x5,
        AspectPreset::R16x9,
        AspectPreset::Original,
    };
    for (AspectPreset preset : presets) {
        for (bool landscape : {true, false}) {
            if (std::abs(presetRatio(preset, landscape, imageAspect) - pixelRatio) < 1e-3)
                return {preset, landscape, true};
        }
    }
    return {AspectPreset::Free, true, false};
}

QRectF rotateQuarterTurns(const QRectF& cropRect, int quarterTurnsCW) {
    QRectF r = cropRect;
    const int turns = ((quarterTurnsCW % 4) + 4) % 4; // normalise to 0..3
    for (int i = 0; i < turns; ++i) {
        // One CW step maps a point (u,v) → (1-v,u). The two opposite corners
        // (x,y) and (x+w,y+h) become (1-y,x) and (1-(y+h),x+w); the new
        // axis-aligned rect takes the min corner and swaps the extents.
        r = QRectF(1.0 - (r.y() + r.height()), r.x(), r.height(), r.width());
    }
    return r;
}

} // namespace crop
