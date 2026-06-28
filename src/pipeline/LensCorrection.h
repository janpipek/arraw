#pragma once
#include "core/ImageBuffer.h"

#include <QPointF>
#include <QString>

#include <array>
#include <functional>

struct ImageBuffer; // defined in ImagePipeline.h

// A correction sampled over normalised radius r in [0, 1], where r = 0 at the
// optical centre and r = 1 at the image corner farthest from it. Every curve is a
// multiplier with 1.0 = identity: `vignette` is a per-pixel gain; `distortion` and
// `tcaR`/`tcaB` are radial scales applied to the vector from the optical centre
// (so geometry warp and chromatic correction share one mechanism). Both lensfun
// (M-a) and embedded/Sony (M-b) sources resolve into this shape, so the apply step
// never sees a polynomial or a knot table (docs/adr/0032).
struct RadialCurve {
    static constexpr int N = 64;
    std::array<float, N> lut{}; // default-constructed: all zero

    // Linear-interpolated lookup at normalised radius r (clamped to [0, 1]).
    float sample(float r) const;

    // Build a curve by evaluating fn at each LUT node (r = i / (N - 1)).
    static RadialCurve fromFn(const std::function<float(float)>& fn);

    // Constant unit gain (lut filled with 1.0) — the identity for a gain curve.
    static RadialCurve identityGain();
};

// The resolved lens correction for one shot. Populated by a source (lensfun or
// embedded); the apply step is source-agnostic.
struct LensCorrectionModel {
    RadialCurve distortion;   // radial displacement
    RadialCurve tcaR, tcaB;   // per-channel radial scale (green = reference)
    RadialCurve vignette;     // radial gain
    bool hasDistortion = false;
    bool hasTCA = false;
    bool hasVignetting = false;
    QPointF center{0.5, 0.5}; // optical centre in normalised image coordinates

    // Provenance, for the sidecar profile identity (docs/adr/0032). Not used by the
    // apply step.
    enum class Source { Lensfun, Embedded };
    Source source = Source::Lensfun;
    QString lensName;
};

// Which corrections the user has enabled (mirrors the GlobalAdjustment toggles).
struct LensCorrectionToggles {
    bool distortion = false;
    bool vignetting = false;
    bool ca = false;
};

// The smallest zoom (>= 1) that keeps every corrected pixel sampling within the
// source after distortion, so the frame fills without invalid/smeared edges
// ("auto-scale to fill", docs/adr/0032). Returns 1.0 when the distortion only
// shrinks (corners already in-bounds) or when there is no distortion. Depends only
// on the distortion curve, optical centre, and dimensions — TCA's sub-pixel deltas
// do not affect framing.
float autoFillZoom(const LensCorrectionModel& model, int width, int height);

// Apply the enabled-and-available corrections to a copy of `buf`; `buf` is not
// mutated. A correction runs only when both its model flag and its toggle are set,
// so an empty model or all-off toggles is an exact no-op. When distortion is active
// the warp is auto-zoomed (see autoFillZoom) so the corrected frame fills.
ImageBuffer applyLensCorrection(const ImageBuffer& buf, const LensCorrectionModel& model,
                                const LensCorrectionToggles& toggles);
