#pragma once

#include <LensCorrection.h>

#include <cstddef>
#include <utility>

namespace arraw::test {

/// @brief Makes a barrel (negative strength) or pincushion distortion by hand, its fill scale set.
/// @param strength Change of the radial scale at the farthest corner.
/// @param aspect Width over height of the frame the tables are for.
/// @return The correction.
[[nodiscard]] inline LensCorrection barrelCorrection(float strength, double aspect = 1.6) {
    LensCorrection correction;
    correction.aspect = aspect;
    correction.distortion.resize(lensTableNodes);
    for (std::size_t node = 0; node < lensTableNodes; ++node) {
        const double radius = static_cast<double>(node) / (lensTableNodes - 1);
        correction.distortion[node] = static_cast<float>(1.0 + strength * radius * radius);
    }
    return withFillScale(std::move(correction));
}

/// @brief Makes a vignetting correction by hand: a gain of 1 at the centre rising to
/// `1 + strength` at the farthest corner.
/// @param strength Gain added at the corner.
/// @param aspect Width over height of the frame the table is for.
/// @return The correction.
[[nodiscard]] inline LensCorrection vignettingCorrection(float strength, double aspect = 1.6) {
    LensCorrection correction;
    correction.aspect = aspect;
    correction.vignetting.resize(lensTableNodes);
    for (std::size_t node = 0; node < lensTableNodes; ++node) {
        const double radius = static_cast<double>(node) / (lensTableNodes - 1);
        correction.vignetting[node] = static_cast<float>(1.0 + strength * radius * radius);
    }
    return withFillScale(std::move(correction));
}

} // namespace arraw::test
