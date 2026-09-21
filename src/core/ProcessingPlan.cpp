#include "ProcessingPlan.h"

#include <WhiteBalance.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <variant>

using namespace arraw;

namespace {

/// @brief Works out how far the white balance has to move from where it is.
///
/// The decode already multiplied the channels by something — the camera's own
/// reading, or a substitute when the file recorded none. Asking for a light is
/// therefore not asking for gains but for the *difference* between the gains
/// that light wants and the ones already in the pixels. Leaving the mode at
/// As Shot makes that difference exactly one, which is why developing with
/// default settings cannot change a photograph's colour.
/// @param camera Sensor the photograph came from.
/// @param settings Settings to resolve.
/// @return Per-channel multipliers to apply, with green at 1.
Gains whiteBalanceDelta(const CameraNative& camera, const DevelopSettings& settings) {
    // Custom with neither value named is As Shot by another route. Saying so
    // here keeps it exactly unity rather than nearly so, and costs a round trip
    // through the curve that would only introduce error.
    const bool named = settings.temperature.has_value() || settings.tint.has_value();
    if (settings.whiteBalance == WhiteBalanceMode::AsShot || !named) {
        return {1.0F, 1.0F, 1.0F};
    }

    // Moving only the tint leaves the temperature where it was, and the other
    // way round, so neither slider drags the other with it. The half that was
    // not named comes from the balance the decode *applied*, not from what the
    // camera recorded: for a file that declared no neutral those are different
    // numbers, and the pixels went through the applied one (ADR 007).
    const ColourTemperature effective = temperatureForGains(camera, camera.appliedMultipliers);
    const ColourTemperature wanted{settings.temperature.value_or(effective.kelvin),
                                   settings.tint.value_or(effective.tint)};

    // Both sides are normalised about green, so their ratio is too.
    const Gains wantedGains = whiteBalanceGains(camera, wanted);
    const Gains applied = withGreenAtOne(camera.appliedMultipliers);
    return {wantedGains[0] / applied[0], 1.0F, wantedGains[2] / applied[2]};
}

/// @brief Resolves the transform out of a source encoding into the working one.
///
/// A camera encoding carries its own, with the white balance composed into it:
/// two matrices multiplied once per photograph rather than two passes over
/// every pixel. The working encoding needs neither. Any other named space is a
/// decoding concern -- import converts what it reads, so one arriving here
/// means a buffer skipped that step.
Matrix3 toWorkingMatrix(const ColorEncoding& encoding, const DevelopSettings& settings) {
    if (const auto* camera = std::get_if<CameraNative>(&encoding)) {
        return camera->toWorking * Matrix3::scale(whiteBalanceDelta(*camera, settings));
    }
    if (isWorkingEncoding(encoding)) {
        if (settings.whiteBalance != WhiteBalanceMode::AsShot) {
            // A temperature in kelvin is measured against a sensor's response.
            // A photograph that arrived already in a standard colour space has
            // no sensor behind it, and gets the incremental setting instead
            // (ADR 008), which is not implemented yet.
            throw std::invalid_argument(
                "A temperature needs a sensor to measure against; this photograph has none");
        }
        return Matrix3::identity();
    }
    throw std::invalid_argument(
        "Development starts from the working encoding or a camera's own primaries");
}

/// @brief Works out how steeply the tone scale rises through middle grey.
///
/// The slider is the exponent's scale: a hundred is a perceptual slope of
/// 1.41 at the pivot, minus a hundred its reciprocal, so equal moves in
/// either direction undo one another (ADR 013).
/// @param settings Settings to resolve.
/// @return The exponent the tone scale is raised to.
float contrastSlopeFor(const DevelopSettings& settings) {
    if (!std::isfinite(settings.contrast)) {
        throw std::invalid_argument("A contrast adjustment must be finite");
    }
    const float contrast = std::clamp(settings.contrast, flattestContrast, steepestContrast);
    return std::exp2(contrast / (2.0F * steepestContrast));
}

/// @brief Works out where the highlight roll-off bends.
///
/// The amount is where the knee sits: none leaves it out of reach, full brings
/// it down to half of white. The plan carries the knee rather than the amount
/// so that the chain has a comparison to make rather than a setting to
/// interpret (ADR 011).
/// @param settings Settings to resolve.
/// @return The knee, in linear luminance.
float shoulderKneeFor(const DevelopSettings& settings) {
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

ProcessingPlan arraw::planFor(const ColorEncoding& encoding, const DevelopSettings& settings) {
    if (!std::isfinite(settings.exposure)) {
        throw std::invalid_argument("An exposure adjustment must be finite");
    }
    // Clamped rather than refused: the processing contract holds whatever
    // reaches it, so that no pixel maths depends on a caller having checked
    // first (ADR 008).
    const float exposure = std::clamp(settings.exposure, darkestExposure, brightestExposure);
    return {.toWorking = toWorkingMatrix(encoding, settings),
            .exposureGain = std::exp2(exposure),
            .shapesTone = settings.contrast != 0.0F,
            .contrastSlope = contrastSlopeFor(settings),
            .shoulderKnee = shoulderKneeFor(settings)};
}

ProcessingPlan arraw::planFor(const Photo& photo) {
    return planFor(photo.metadata().encoding, photo.settings());
}
