#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace arraw {

/// @brief Strongest darkening and lightening the post-crop vignette may ask for.
///
/// Minus a hundred darkens the corners by two stops at the full falloff, plus
/// a hundred lightens them toward white by the same measure (ADR 037).
inline constexpr float darkestVignette = -100.0F;

/// @copydoc darkestVignette
inline constexpr float lightestVignette = 100.0F;

/// @brief Limits of the vignette's Midpoint and Feather, both zero to a hundred.
inline constexpr float minimumVignetteShape = 0.0F;

/// @copydoc minimumVignetteShape
inline constexpr float maximumVignetteShape = 100.0F;

/// @brief Post-crop vignette: an elliptical falloff fitted to the cropped frame, in domain units.
///
/// Relative to the frame the photographer cropped to, not to the sensor, so it
/// follows every crop, and the same point of that frame gets the same falloff
/// whatever region or size is rendered (ADR 037). With the amount at zero
/// nothing changes, whatever the midpoint and feather say.
struct VignetteSettings {
    /// @brief How far the edges move: negative darkens, positive lightens, -100 to 100.
    float amount = 0.0F;

    /// @brief Where the falloff begins, zero (at the centre) to a hundred (near the corners).
    float midpoint = 50.0F;

    /// @brief Softness of the falloff, zero for a hard edge to a hundred for the broadest.
    float feather = 50.0F;

    friend bool operator==(const VignetteSettings&, const VignetteSettings&) = default;
};

/// @brief Limits of the grain's Amount, Size and Roughness, all zero to a hundred.
inline constexpr float minimumGrainControl = 0.0F;

/// @copydoc minimumGrainControl
inline constexpr float maximumGrainControl = 100.0F;

/// @brief Algorithm that draws the grain (ADR 038).
///
/// What the photographer sets (amount, size, roughness, seed) means the same
/// for every model; a new algorithm is a new value here, not new settings, and
/// a document keeps rendering with the model it was made with.
enum class GrainModel {
    /// @brief Smoothly interpolated random values on up to three lattices, finer to coarser.
    ///
    /// `main`'s grain, band-limited to the pixel it is drawn on.
    ValueNoise,
};

/// @brief Stable names of the grain models, as documents and the command line spell them.
inline constexpr std::array<std::pair<GrainModel, std::string_view>, 1> grainModelNames{{
    {GrainModel::ValueNoise, "valueNoise"},
}};

/// @brief Film-like grain on the cropped frame, in domain units.
///
/// Anchored to the frame the photographer cropped to and to a seed, so the
/// same photograph shows the same grain across pan, zoom, preview and export;
/// a preview may soften it, never rearrange it (ADR 038). With the amount at
/// zero nothing changes, whatever the other fields say.
struct GrainSettings {
    /// @brief Strength of the grain, zero (none) to a hundred.
    float amount = 0.0F;

    /// @brief Size of a grain relative to the cropped frame's long edge, zero (finest) to a
    /// hundred (coarsest).
    float size = 50.0F;

    /// @brief How clumped the grain is, zero (even, one scale) to a hundred (clustered).
    float roughness = 50.0F;

    /// @brief Algorithm that draws it.
    GrainModel model = GrainModel::ValueNoise;

    /// @brief Which of the model's patterns this photograph has; zero for none chosen yet.
    ///
    /// A property of the photograph, not of a look: hidden from the panel and
    /// kept out of presets and copied settings. Zero renders with one fixed
    /// pattern, so a document without a seed is still deterministic; front ends
    /// give grain they turn on a seed of its own through ::arraw::chooseGrainSeed,
    /// and the core never invents one.
    std::uint32_t seed = 0;

    friend bool operator==(const GrainSettings&, const GrainSettings&) = default;
};

/// @brief Effects applied to the finished, cropped frame, in domain units.
///
/// Effects read the crop frame, so they run after the resize, in their own
/// pass (ADR 037): the vignette, then grain on the vignetted tones.
struct EffectsSettings {
    /// @brief Darkening or lightening of the frame's edges.
    VignetteSettings vignette{};

    /// @brief Film-like texture over the whole frame.
    GrainSettings grain{};

    friend bool operator==(const EffectsSettings&, const EffectsSettings&) = default;
};

/// @brief Source of random seeds: each call returns 32 fresh bits.
using GrainEntropy = std::function<std::uint32_t()>;

/// @brief Gives the seed grain should carry after an edit: the one place that decides it (ADR 038).
///
/// A seed is chosen only when the edit turns grain on, its amount going from
/// zero to above zero, and the grain has none: it is drawn from @p entropy and
/// is never zero. Anything else keeps the seed @p next has, zero included, so
/// grain that was already on keeps its pattern through every later edit,
/// even when that pattern is the fixed one of seed zero. Front ends call this
/// with the settings before and after each edit, and store the result; a seed
/// the user gave explicitly is stored without asking. Whether the seed is
/// random, or derived from the photograph, is decided here alone.
/// @param previous The grain settings before the edit.
/// @param next The grain settings after it.
/// @param entropy Where new bits come from; empty for `std::random_device`.
/// @return The seed to store in @p next's GrainSettings::seed.
[[nodiscard]] std::uint32_t chooseGrainSeed(const GrainSettings& previous,
                                            const GrainSettings& next,
                                            const GrainEntropy& entropy = {});

} // namespace arraw
