#pragma once

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <NoiseReductionSettings.h>

#include <array>
#include <cstdint>

namespace arraw {

/// @brief Spatial sigma of the luminance filter, in sensor pixels.
///
/// A small fixed reach: Amount drives the blend and Detail the edge-stop, so a
/// photographer sees two controls (`main`'s ADR 0046).
inline constexpr float luminanceNoiseSpatialSigma = 2.0F;

/// @brief Edge-stop sigma at Detail 0, in the perceptual coordinate: the loosest.
inline constexpr float loosestLuminanceNoiseRange = 0.20F;

/// @brief Edge-stop sigma at Detail 100: the tightest, still above zero.
inline constexpr float tightestLuminanceNoiseRange = 0.02F;

/// @brief Sensor pixels of colour blur sigma per unit of Smoothness: 100 is 25 pixels.
inline constexpr float colorNoiseSigmaPerSmoothness = 0.25F;

/// @brief Sensor pixels per cell of the grid the colour is blurred on, at full resolution.
///
/// The colour blur runs on a box reduction of the source: four sensor pixels a
/// side, so a cell is four source pixels at a pixel scale of 1, two at 2 and
/// one from 4 on, and the grid keeps the same size in sensor pixels whatever
/// the reduction (ADR 039).
inline constexpr std::uint32_t colorNoiseGridReduction = 4;

/// @brief Largest tap radius of either filter, the shaders' compile-time bound.
inline constexpr std::uint32_t maximumDenoiseRadius = 64;

/// @brief Luminance below which the ratio decomposition blends into a neutral offset.
///
/// 2^-14, the floor of a 14-bit raw file, as ::arraw::curveRatioFloor. The
/// unit-luma ratio of a colour is `(c + (D - Y) n) / D` with
/// `D = max(Y, 0) + floor`: `c / Y` for any luminance well above it, a bounded
/// vector for a luminance near zero or below, and continuous between (ADR 039).
inline constexpr float denoiseRatioFloor = 0x1p-14F;

/// @brief Weights of a symmetric Gaussian, entry `i` for a tap `i` pixels away.
///
/// Entry 0 is one; entries past the radius are zero. Both backends read these
/// numbers, so that neither evaluates `exp` for the spatial term.
using DenoiseWeights = std::array<float, maximumDenoiseRadius + 1>;

/// @brief The denoise block of a plan: what the Denoise pass does to the source.
///
/// The group of ::arraw::Stage::Denoise in `stagesOf` (ADR 011, ADR 039). It
/// holds the concrete numbers the pass runs with, radii already divided by
/// the source's pixel scale. A half that is off resolves to its defaults,
/// and with both off the block is the default and the pass does not run. The
/// luminance row is the source's as-shot one, never the plan's `toWorking`, so
/// that white balance and exposure never reach this block.
struct DenoisePlan {
    /// @brief Whether luminance is smoothed.
    bool luminance = false;

    /// @brief Whether colour is smoothed.
    bool color = false;

    /// @brief Source channels to working luminance: the as-shot camera row, or Rec.2020's.
    Colour lumaRow{};

    /// @brief The neutral of unit luminance in the source's channels: `(1, 1, 1) / sum(lumaRow)`.
    Colour neutral{};

    /// @brief Filter that smooths luminance.
    LuminanceNoiseFilter filter = LuminanceNoiseFilter::Bilateral;

    /// @brief How much of the filtered luminance replaces the original, zero to one.
    float luminanceMix = 0.0F;

    /// @brief Edge-stop sigma, in the perceptual coordinate.
    float rangeSigma = 0.0F;

    /// @brief Spatial sigma of the luminance filter, in the source's pixels.
    float spatialSigma = 0.0F;

    /// @brief Tap radius of the luminance filter, in the source's pixels.
    std::uint32_t spatialRadius = 0;

    /// @brief How much of the smoothed colour replaces the original, zero to one.
    float colorMix = 0.0F;

    /// @brief Source pixels per side of a grid cell the colour is blurred on.
    std::uint32_t gridReduction = 0;

    /// @brief Sigma of the colour blur, in grid cells.
    float colorSigma = 0.0F;

    /// @brief Tap radius of the colour blur, in grid cells.
    std::uint32_t colorRadius = 0;

    /// @brief Whether anything is smoothed, and so whether the pass runs.
    [[nodiscard]] bool active() const noexcept {
        return luminance || color;
    }

    friend bool operator==(const DenoisePlan&, const DenoisePlan&) = default;
};

/// @brief Resolves the noise reduction settings for one source.
/// @param settings What the photographer set.
/// @param encoding Encoding of the source's pixels, which gives the luminance row.
/// @param pixelScale Sensor pixels per source pixel (::arraw::ImageBuffer::pixelScale);
/// radii in sensor pixels are divided by it.
/// @return The block; the default when both amounts are zero.
/// @throws std::invalid_argument if a setting is not finite, the filter is
/// unknown, @p pixelScale is not finite and above zero, or a smoothing is
/// asked of an encoding development cannot start from.
[[nodiscard]] DenoisePlan denoisePlanFor(const NoiseReductionSettings& settings,
                                         const ColorEncoding& encoding, double pixelScale);

/// @brief Gives the weights of a Gaussian of a sigma, out to a radius.
/// @param sigma Standard deviation, in taps; above zero.
/// @param radius Last tap, at most ::arraw::maximumDenoiseRadius.
[[nodiscard]] DenoiseWeights denoiseWeights(float sigma, std::uint32_t radius);

/// @brief Gives the factor of the edge-stop's exponent, `1 / (2 rangeSigma^2)`.
[[nodiscard]] float rangeFactorOf(const DenoisePlan& plan);

/// @brief Gives the perceptual value the edge-stop measures luminance differences in.
///
/// `max(y, 0)^(1/2.2)`: ::arraw::toPerceptual with negatives held at black.
/// Mirrored by `perceptualLuma` in `src/gpu/shaders/denoise_filter.frag`.
[[nodiscard]] float perceptualLuma(float luminance);

/// @brief A colour split into its luminance and its unit-luma ratio.
struct LumaRatio {
    float luminance = 0.0F; ///< Luminance through the plan's row.
    Colour ratio{};         ///< Unit-luma ratio: its luminance through the row is one.
};

/// @brief Splits a colour into luminance and unit-luma ratio, as both halves share.
///
/// `r = (c + (D - Y) n) / D` with `D = max(Y, 0) + ::arraw::denoiseRatioFloor`.
/// Mirrored by `decompose` in the denoise shaders.
[[nodiscard]] LumaRatio decompose(const DenoisePlan& plan, Colour colour);

/// @brief Rebuilds a colour from a luminance and a unit-luma ratio, the inverse of
/// ::arraw::decompose.
///
/// `c = D r - (D - Y) n`: its luminance is @p luminance whatever the ratio, so
/// colour smoothing keeps luminance and luminance smoothing keeps the ratio.
[[nodiscard]] Colour recompose(const DenoisePlan& plan, float luminance, Colour ratio);

/// @brief Gives how far a source pixel's result reaches, in source pixels, for a region render.
///
/// A render restricted to part of the source would need this margin around
/// the part's footprint to give the same pixels as a whole render (ADR 025's
/// future work). Zero when the plan is off.
[[nodiscard]] std::uint32_t denoiseReach(const DenoisePlan& plan);

/// @brief Runs the Denoise pass on a source.
/// @param source Decoded photograph, in any layout; read only.
/// @param plan Active block to run.
/// @return A new ::arraw::PixelFormat::RgbaF32 buffer with @p source's size,
/// encoding and pending orientation; alpha is copied.
[[nodiscard]] ImageBuffer applyDenoise(const ImageBuffer& source, const DenoisePlan& plan);

} // namespace arraw
