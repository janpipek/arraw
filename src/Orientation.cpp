#include "Orientation.h"

namespace orient {

// EXIF orientation decomposed as "rotate clockwise by quarterTurnsCW, then mirror
// horizontally if mirrored" (mirror is the outer op). In y-down image coords the
// mirrored-and-rotated states fall out as: mirrorH∘R¹ = transpose (EXIF 5),
// mirrorH∘R³ = transverse (EXIF 7), mirrorH∘R² = vertical mirror (EXIF 4).
Orientation fromExif(int exif) {
    switch (exif) {
    case 2:
        return {0, true};
    case 3:
        return {2, false};
    case 4:
        return {2, true};
    case 5:
        return {1, true};
    case 6:
        return {1, false};
    case 7:
        return {3, true};
    case 8:
        return {3, false};
    case 1:
    default:
        return {0, false};
    }
}

QPointF orientedToBuffer(QPointF uv, Orientation o) {
    double u = uv.x();
    double v = uv.y();
    // Inverse of the display transform (rotateCW^r ∘ mirrorH^m): undo the mirror
    // first (it is its own inverse), then rotate back CCW once per quarter-turn.
    if (o.mirrored)
        u = 1.0 - u;
    for (int i = 0; i < o.quarterTurnsCW; ++i) {
        const double nu = v;
        const double nv = 1.0 - u;
        u = nu;
        v = nv;
    }
    return {u, v};
}

bool swapsAspect(Orientation o) {
    return (o.quarterTurnsCW % 2) != 0;
}

int toExif(Orientation o) {
    switch (o.quarterTurnsCW) {
    case 1:
        return o.mirrored ? 5 : 6;
    case 2:
        return o.mirrored ? 4 : 3;
    case 3:
        return o.mirrored ? 7 : 8;
    case 0:
    default:
        return o.mirrored ? 2 : 1;
    }
}

} // namespace orient
