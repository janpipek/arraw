#include "TonePlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

using namespace arraw;

namespace {

/// @brief Refuses a setting that is not finite.
/// @param setting What the photographer asked for.
/// @param name What to call the control, with its article, if it has to be refused.
/// @throws std::invalid_argument if @p setting is not finite.
void requireFinite(float setting, const char* name) {
    if (!std::isfinite(setting)) {
        throw std::invalid_argument(std::string(name) + " adjustment must be finite");
    }
}

} // namespace

ToneAmounts arraw::toneAmountsOf(const ToneSettings& settings) {
    requireFinite(settings.exposure, "An exposure");
    requireFinite(settings.contrast, "A contrast");
    requireFinite(settings.shadows, "A shadows");
    requireFinite(settings.highlights, "A highlights");
    requireFinite(settings.blacks, "A blacks");
    requireFinite(settings.whites, "A whites");
    // Clamped rather than refused: the processing contract holds whatever
    // reaches it, so that no pixel maths depends on a caller having checked
    // first (ADR 008).
    return {.exposure = std::clamp(settings.exposure, darkestExposure, brightestExposure),
            .contrast = std::clamp(settings.contrast, flattestContrast, steepestContrast),
            .highlights = std::clamp(settings.highlights, weakestToneControl, strongestToneControl),
            .shadows = std::clamp(settings.shadows, weakestToneControl, strongestToneControl),
            .whites = std::clamp(settings.whites, weakestToneControl, strongestToneControl),
            .blacks = std::clamp(settings.blacks, weakestToneControl, strongestToneControl)};
}

TonePlan arraw::tonePlanFor(const ToneSettings& settings) {
    return resolveTone(toneAmountsOf(settings));
}

float arraw::shoulderKneeFor(const ToneSettings& settings) {
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
