#pragma once

#include <ColorEncoding.h>
#include <ColorGradingSettings.h>

namespace arraw {

/// @brief Offset one Colour Grading zone adds to a colour's Oklab a and b.
struct ZoneTint {
    float a = 0.0F; ///< Toward red, or toward green when negative.
    float b = 0.0F; ///< Toward yellow, or toward blue when negative.

    friend bool operator==(const ZoneTint&, const ZoneTint&) = default;
};

/// @brief The Colour Grading block of a plan: three zone tints and the shape of the zones.
///
/// Part of the colour block, and so of the pointwise group (ADR 011, ADR 034).
/// Each zone's hue and saturation are resolved once into the Oklab offset the
/// zone adds at full weight, so that neither backend takes a sine or cosine
/// per pixel and both add the very same vectors. With every saturation at zero
/// the block is off and left at its defaults, so that Balance and Blending
/// alone neither change the pixels nor the plan.
struct ColorGradingPlan {
    /// @brief Whether any zone tints anything.
    bool active = false;

    /// @brief Offset the Shadows zone adds where it has all the weight.
    ZoneTint shadowTint{};

    /// @brief Offset the Midtones zone adds where it has all the weight.
    ZoneTint midtoneTint{};

    /// @brief Offset the Highlights zone adds where it has all the weight.
    ZoneTint highlightTint{};

    /// @brief How far Balance moves a colour's tonal position before the zones weigh it.
    ///
    /// In the perceptual coordinate: positive reads every colour as lighter,
    /// so more of the range falls to the Highlights zone.
    float balanceShift = 0.0F;

    /// @brief Width of each zone's bell over the tonal position, from Blending.
    float zoneWidth = 1.0F;

    friend bool operator==(const ColorGradingPlan&, const ColorGradingPlan&) = default;
};

/// @brief Resolves the Colour Grading settings into the plan's block.
///
/// Out-of-range values are clamped rather than refused, like every setting
/// the pixel maths reads (ADR 008), except hues, which are angles and are
/// wrapped onto the wheel instead: 400 is 40, -30 is 330.
/// @param settings Zone hues and saturations, Balance and Blending.
/// @return The block; the default, off, when no zone has any saturation.
/// @throws std::invalid_argument if a value is not finite.
[[nodiscard]] ColorGradingPlan colorGradingPlanFor(const ColorGradingSettings& settings);

/// @brief Weights of the three zones at a colour's luminance, summing to one.
struct ZoneWeights {
    float shadows;    ///< Weight of the Shadows zone.
    float midtones;   ///< Weight of the Midtones zone.
    float highlights; ///< Weight of the Highlights zone.
};

/// @brief Weighs the three zones for one luminance.
///
/// The luminance, held to black and white, is taken into the perceptual
/// coordinate the tone controls use, moved by Balance, and held to zero to
/// one again: its tonal position. Each zone is a Gaussian bell over that
/// position, centred at zero, one half and one, as wide as Blending asks; the
/// three are normalised to sum to one.
/// @param plan Colour Grading block.
/// @param luminance Linear luminance in the working space.
/// @return The normalised weights.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] ZoneWeights gradeZoneWeights(const ColorGradingPlan& plan, float luminance);

/// @brief Computes how much of the blended tint a colour of an Oklab lightness takes.
///
/// One up to an Oklab lightness of 0.85, falling smoothly to zero at 1 and
/// staying there above it: the grade runs after the shoulder, so a tint on a
/// near-white tone would otherwise push channels far past white, where the
/// export clips them, and white itself stays white (ADR 034).
/// @param lightness Oklab L of the colour being graded.
/// @return The factor, from zero to one.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] float gradeTintFade(float lightness);

/// @brief Tints one colour by the zones its luminance falls in, holding its Oklab lightness.
///
/// The zones' offsets are blended by their weights, faded toward white by
/// ::arraw::gradeTintFade, and added to the colour's Oklab a and b, so a
/// neutral grey takes on the tint, and a grey from Black & White as much as
/// any other.
/// @param plan Colour Grading block.
/// @param colour Colour in the working encoding.
/// @return The tinted colour; exactly @p colour when the block is off or the
/// colour is at or above white (Oklab L of at least 1), where nothing is left of the tint.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Colour applyColorGrading(const ColorGradingPlan& plan, Colour colour);

} // namespace arraw
