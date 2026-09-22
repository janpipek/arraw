#include <ToneSettings.h>

#include "ProcessingPlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

using namespace arraw;

namespace {

/// @brief How far Shadows and Highlights move their region, at full setting.
///
/// In the perceptual coordinate. A smoothstep window's slope is at most
/// `1.5 / width`, so this reach perturbs the tone scale's slope by at most
/// 0.6 -- and the widest opposing pair, Shadows against Blacks, stays under
/// one, which is what keeps the scale from folding back (ADR 013).
constexpr float regionalReach = 0.12F;

/// @brief How far Blacks and Whites move their end, at full setting.
constexpr float endpointReach = 0.08F;

/// @brief Works out how steeply the tone scale rises through middle grey.
///
/// The slider is the exponent's scale: a hundred is a perceptual slope of
/// 1.41 at the pivot, minus a hundred its reciprocal, so equal moves in
/// either direction undo one another (ADR 013).
/// @param settings Settings to resolve.
/// @return The exponent the tone scale is raised to.
float contrastSlopeFor(const ToneSettings& settings) {
    if (!std::isfinite(settings.contrast)) {
        throw std::invalid_argument("A contrast adjustment must be finite");
    }
    const float contrast = std::clamp(settings.contrast, flattestContrast, steepestContrast);
    return std::exp2(contrast / (2.0F * steepestContrast));
}

/// @brief Works out how far one regional tone control moves its region.
///
/// Shadows and Highlights reach further than Blacks and Whites, because a
/// region has room to move and an end does not; the amounts are chosen so that
/// no two controls together can fold the tone scale back on itself (ADR 013).
/// @param setting What the photographer asked for.
/// @param reach How far a full setting moves, in the perceptual coordinate.
/// @param name What to call the control if it has to be refused.
/// @return The shift the chain adds at the peak of the region.
float toneShiftFor(float setting, float reach, const char* name) {
    if (!std::isfinite(setting)) {
        throw std::invalid_argument(std::string("A ") + name + " adjustment must be finite");
    }
    return reach * std::clamp(setting, weakestToneControl, strongestToneControl) /
           strongestToneControl;
}

/// @brief Works out where the highlight roll-off bends.
///
/// The amount is where the knee sits: none leaves it out of reach, full brings
/// it down to half of white. The plan carries the knee rather than the amount
/// so that the chain has a comparison to make rather than a setting to
/// interpret (ADR 011).
/// @param settings Settings to resolve.
/// @return The knee, in linear luminance.
float shoulderKneeFor(const ToneSettings& settings) {
    if (!std::isfinite(settings.filmicHighlights)) {
        throw std::invalid_argument("A highlight roll-off must be finite");
    }
    const float amount =
        std::clamp(settings.filmicHighlights, noFilmicHighlights, fullFilmicHighlights);
    if (amount <= noFilmicHighlights) {
        return std::numeric_limits<float>::infinity();
    }
    return 1.0F - 0.5F * amount / fullFilmicHighlights;
}

} // namespace

ProcessingPlan arraw::tonePlanFor(const ToneSettings& settings) {
    if (!std::isfinite(settings.exposure)) {
        throw std::invalid_argument("An exposure adjustment must be finite");
    }
    const float slope = contrastSlopeFor(settings);
    // Clamped rather than refused: the processing contract holds whatever
    // reaches it, so that no pixel maths depends on a caller having checked
    // first (ADR 008).
    const float exposure = std::clamp(settings.exposure, darkestExposure, brightestExposure);
    return {.exposureGain = std::exp2(exposure),
            .shapesTone = settings.contrast != 0.0F || settings.shadows != 0.0F ||
                          settings.highlights != 0.0F || settings.blacks != 0.0F ||
                          settings.whites != 0.0F,
            .contrastSlope = slope,
            .contrastScale = std::pow(greyPivot, 1.0F - slope),
            .shadowShift = toneShiftFor(settings.shadows, regionalReach, "shadows"),
            .highlightShift = toneShiftFor(settings.highlights, regionalReach, "highlights"),
            .blackShift = toneShiftFor(settings.blacks, endpointReach, "blacks"),
            .whiteShift = toneShiftFor(settings.whites, endpointReach, "whites"),
            .shoulderKnee = shoulderKneeFor(settings)};
}
