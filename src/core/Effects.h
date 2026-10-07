#pragma once

#include "GrainModels.h"

#include <ColorEncoding.h>
#include <EffectsSettings.h>
#include <ImageBuffer.h>

#include <array>
#include <cstdint>

namespace arraw {

/// @brief Widest the vignette's falloff starts from the centre, as a fraction of the corner radius.
///
/// Midpoint 100 starts the falloff here, so that even the outermost vignette
/// still has a band to fall off in before the corners (main's ADR 0026).
inline constexpr float vignetteInnermostReach = 0.85F;

/// @brief Narrowest a feathered falloff may be, in corner radii.
///
/// Keeps a smooth falloff's two edges apart, so that its slope stays finite;
/// a feather of exactly zero is a hard edge instead (see VignettePlan::hardEdge).
inline constexpr float narrowestVignetteFalloff = 1.0e-4F;

/// @brief Stops the vignette moves the corners by at an amount of a hundred, either way.
inline constexpr float strongestVignetteStops = 2.0F;

/// @brief The vignette's part of the effects block, resolved.
///
/// The falloff is measured in corner radii: zero at the centre of the crop
/// frame, one in its corners, along ellipses fitted to the frame (ADR 037).
/// An amount of zero resolves to the default, off, whatever the midpoint and
/// feather, so that they neither change pixels nor the plan.
struct VignettePlan {
    /// @brief Whether the vignette changes anything.
    bool active = false;

    /// @brief Whether the edges are lightened toward white rather than darkened.
    bool lightens = false;

    /// @brief Whether the falloff is a hard edge at VignettePlan::inner, with no feather.
    bool hardEdge = false;

    /// @brief Exposure change at the full falloff, in stops; positive, the direction is
    /// VignettePlan::lightens.
    float stops = 0.0F;

    /// @brief Radius at which the falloff begins.
    float inner = 0.0F;

    /// @brief Radius at which it is complete; above VignettePlan::inner unless the edge is hard.
    float outer = 1.0F;

    friend bool operator==(const VignettePlan&, const VignettePlan&) = default;
};

/// @brief The effects block of a plan: what the Effects pass does to the cropped frame.
///
/// The group of ::arraw::Stage::Effects in `stagesOf` (ADR 011). It holds only
/// what the settings resolve to; where an output pixel lies in the crop frame
/// comes from the resize block before it (::arraw::frameMappingOf), so that a
/// region or a size changes the mapping and never this block. With every
/// effect off the pass does not run and its boundary collapses onto the resize.
struct EffectsPlan {
    /// @brief Darkening or lightening of the frame's edges.
    VignettePlan vignette{};

    /// @brief Film-like texture, drawn by the model it names (ADR 038).
    GrainPlan grain{};

    /// @brief Whether any effect changes anything, and so whether the pass runs.
    [[nodiscard]] bool active() const noexcept {
        return vignette.active || grain.active;
    }

    friend bool operator==(const EffectsPlan&, const EffectsPlan&) = default;
};

/// @brief Where a render's output pixels lie in the crop frame.
///
/// Output pixel `(x, y)`, at its centre, is at
/// `origin + (pixel + 0.5) * step` in fractions of the cropped frame, which is
/// `(region origin + (pixel + 0.5) * region / outputSize) / croppedSize` per
/// axis (ADR 037). The same point of the frame therefore gets the same
/// position whatever region, size or pyramid level is rendered. Worked out in
/// double; the GPU takes it as floats.
struct FrameMapping {
    /// @brief Position of output pixel (0, 0)'s top-left corner, in fractions of the frame.
    std::array<double, 2> origin{0.0, 0.0};

    /// @brief Fraction of the frame one output pixel covers along each axis: its footprint.
    ///
    /// What a band-limited effect such as grain needs to fade out detail finer
    /// than a pixel.
    std::array<double, 2> step{1.0, 1.0};

    /// @brief Width over height of the cropped frame, for effects that must stay round.
    double aspect = 1.0;

    friend bool operator==(const FrameMapping&, const FrameMapping&) = default;
};

/// @brief An effects plan placed on one render: what the Effects pass reads besides the plan.
///
/// Worked out once per render (::arraw::effectsPlacementOf); the GPU packs the
/// same numbers into its uniform block.
struct EffectsPlacement {
    /// @brief Where the render's pixels lie in the crop frame.
    FrameMapping mapping{};

    /// @brief The grain's lattices on the render; unused when the grain is off.
    GrainPlacement grain{};
};

/// @brief A point of the crop frame, in fractions of its width and height.
struct FramePoint {
    float x = 0.5F; ///< Fraction of the width, zero at the left edge.
    float y = 0.5F; ///< Fraction of the height, zero at the top edge.
};

/// @brief Resolves the effects settings into the plan's block.
///
/// Out-of-range values are clamped rather than refused, like every setting the
/// pixel maths reads (ADR 008).
/// @param settings Vignette and grain settings.
/// @return The block; every effect off when its amount is zero.
/// @throws std::invalid_argument if a value is not finite.
[[nodiscard]] EffectsPlan effectsPlanFor(const EffectsSettings& settings);

/// @brief Places an effects plan on a render.
/// @param plan Resolved effects.
/// @param mapping Where the render's pixels lie in the crop frame.
/// @return The placement; the grain's is only worked out when the grain is on.
[[nodiscard]] EffectsPlacement effectsPlacementOf(const EffectsPlan& plan,
                                                  const FrameMapping& mapping);

/// @brief Gives the centre of an output pixel in the crop frame.
/// @param mapping Mapping of the render.
/// @param column Output column.
/// @param row Output row.
/// @return The point, rounded to float once from the double position.
[[nodiscard]] FramePoint framePointOf(const FrameMapping& mapping, std::uint32_t column,
                                      std::uint32_t row);

/// @brief Weighs a point of the crop frame by the vignette's falloff.
///
/// The radius is `sqrt(((2x - 1)^2 + (2y - 1)^2) / 2)`: zero at the centre and
/// one in the corners, so the ellipses are fitted to the frame. The weight is
/// zero inside VignettePlan::inner and rises to one at VignettePlan::outer by
/// ::arraw::smoothstep, or jumps there for a hard edge.
/// @param plan Resolved vignette.
/// @param point Where in the frame.
/// @return The weight, from zero (untouched) to one (the full falloff).
///
/// Mirrored by `src/gpu/shaders/effects.frag`, which must change with it.
[[nodiscard]] float vignetteWeight(const VignettePlan& plan, FramePoint point);

/// @brief Darkens or lightens a colour by the vignette, at a weight of its falloff.
///
/// In the perceptual coordinate (ADR 010) with `g = 2^(-stops * weight / 2.2)`:
/// - darkening multiplies each channel's perceptual value by `g`, which is
///   exactly an exposure change of `-stops * weight` and so is done as that,
///   a linear gain on all three channels: hue-preserving, and negatives scale;
/// - lightening maps each perceptual value `v` to `1 - (1 - v) * g`, a screen:
///   it lifts the edges toward white and never past it, so no second shoulder
///   is needed after it. It is the darkening mirrored about the middle of
///   the perceptual range, so both halves meet smoothly at amount zero.
///
/// A weight of zero returns the colour exactly.
/// @param plan Resolved vignette.
/// @param colour Colour in the working encoding.
/// @param weight Falloff at the pixel, from ::arraw::vignetteWeight.
/// @return The vignetted colour.
///
/// Mirrored by `src/gpu/shaders/effects.frag`, which must change with it.
[[nodiscard]] Colour applyVignette(const VignettePlan& plan, Colour colour, float weight);

/// @brief Adds grain to a colour, the same to every channel, in the perceptual coordinate.
///
/// Each channel goes to the signed perceptual coordinate `v = y^(1/2.2)`
/// (ADR 010), gets @p grain added, and comes back: monochrome texture whose
/// contrast looks alike in the shadows and the highlights, as `main` adds it
/// in sRGB's encoding. A grain of exactly zero returns the colour exactly.
/// @param colour Colour in the working encoding.
/// @param grain From ::arraw::grainAt.
/// @return The grainy colour.
///
/// Mirrored by `src/gpu/shaders/effects.frag`, which must change with it.
[[nodiscard]] Colour applyGrain(Colour colour, float grain);

/// @brief Applies every effect to one output pixel, in their fixed order.
///
/// The order lives here: the vignette, then grain, so that the texture sits
/// on the vignetted tones, as on film. Grain is asked of its model through
/// ::arraw::grainAt; this function does not know which model that is.
/// @param plan Resolved effects.
/// @param placement The plan on this render.
/// @param column Output column.
/// @param row Output row.
/// @param colour Colour in the working encoding.
/// @return The colour with every active effect applied.
///
/// Mirrored by `src/gpu/shaders/effects.frag`, which must change with it.
[[nodiscard]] Colour effectsPixel(const EffectsPlan& plan, const EffectsPlacement& placement,
                                  std::uint32_t column, std::uint32_t row, Colour colour);

/// @brief Runs the Effects pass over a frame.
///
/// Alpha is copied, as the pointwise pass copies it, so the resize's opacity
/// hint still describes the pixels.
/// @param pixels Working-format pixels after the resize; consumed.
/// @param plan Resolved effects; the pass is only run when it is active.
/// @param mapping Where the pixels lie in the crop frame.
/// @return The pixels with the effects applied.
[[nodiscard]] ImageBuffer applyEffects(ImageBuffer pixels, const EffectsPlan& plan,
                                       const FrameMapping& mapping);

} // namespace arraw
