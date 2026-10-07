#pragma once

#include <array>
#include <string_view>
#include <utility>

namespace arraw {

/// @brief Limits of every noise reduction control, all zero to a hundred.
inline constexpr float minimumNoiseReduction = 0.0F;

/// @copydoc minimumNoiseReduction
inline constexpr float maximumNoiseReduction = 100.0F;

/// @brief Colour noise reduction a RAW starts with, as Lightroom's (see ::arraw::defaultStateFor).
///
/// The settings' own default stays zero, so that a picture that is not a RAW,
/// and a document that leaves the key out, are untouched (ADR 039).
inline constexpr float rawDefaultColorNoiseReduction = 25.0F;

/// @brief Filter that smooths luminance noise (ADR 039).
///
/// Amount and Detail mean "how much" and "how much edge to keep" for every
/// filter; a new filter is a new value here, not new settings, and a document
/// keeps rendering with the filter it was made with.
enum class LuminanceNoiseFilter {
    /// @brief Separable bilateral whose edge-stop measures perceptual luma differences.
    ///
    /// `main`'s filter (its ADR 0046): two one-dimensional passes, cheap, with
    /// faint cross-shaped artefacts at strong settings.
    Bilateral,
};

/// @brief Stable names of the luminance noise filters, as documents and the command line spell
/// them.
inline constexpr std::array<std::pair<LuminanceNoiseFilter, std::string_view>, 1>
    luminanceNoiseFilterNames{{
        {LuminanceNoiseFilter::Bilateral, "bilateral"},
    }};

/// @brief Luminance and colour noise reduction, in domain units.
///
/// Runs on the decoded photograph before any other development, in its own
/// pass (ADR 039), so that every later control reuses its result. Radii are
/// measured in sensor pixels, since noise has the size of a sensor pixel
/// whatever the crop or the output size. With both amounts at zero nothing
/// happens, whatever the other fields say.
struct NoiseReductionSettings {
    /// @brief Strength of the luminance smoothing, zero (none) to a hundred.
    float luminance = 0.0F;

    /// @brief How much edge the luminance smoothing keeps, zero (smooths across most edges) to a
    /// hundred (keeps nearly all).
    float luminanceDetail = 50.0F;

    /// @brief Filter that smooths luminance.
    LuminanceNoiseFilter luminanceFilter = LuminanceNoiseFilter::Bilateral;

    /// @brief Strength of the colour smoothing, zero (none) to a hundred.
    ///
    /// Lightroom's "Color". Zero here, the neutral default every document and
    /// descriptor shares; a RAW starts at ::arraw::rawDefaultColorNoiseReduction
    /// instead (::arraw::defaultStateFor).
    float color = 0.0F;

    /// @brief Size of the colour blotches smoothed, zero (none) to a hundred (about 25 sensor
    /// pixels).
    float colorSmoothness = 50.0F;

    friend bool operator==(const NoiseReductionSettings&, const NoiseReductionSettings&) = default;
};

/// @brief Whether the luminance half does anything: an amount above zero.
[[nodiscard]] constexpr bool
reducesLuminanceNoise(const NoiseReductionSettings& settings) noexcept {
    return settings.luminance > 0.0F;
}

/// @brief Whether the colour half does anything: an amount and a smoothness both above zero.
///
/// Strength zero blends nothing back, and Smoothness zero is a blur of no width.
[[nodiscard]] constexpr bool reducesColorNoise(const NoiseReductionSettings& settings) noexcept {
    return settings.color > 0.0F && settings.colorSmoothness > 0.0F;
}

/// @brief Whether noise reduction does anything, and so whether the Denoise pass runs.
///
/// Either half does something; Detail and the filter only say how. The one
/// written rule of the pass: ::arraw::denoisePlanFor reads the halves from the
/// same predicates. What a front end asks before keeping a checkpoint at
/// ::arraw::Stage::Denoise, which with this false would only be a copy of the
/// source (ADR 039).
[[nodiscard]] constexpr bool reducesNoise(const NoiseReductionSettings& settings) noexcept {
    return reducesLuminanceNoise(settings) || reducesColorNoise(settings);
}

} // namespace arraw
