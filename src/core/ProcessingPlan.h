#pragma once

#include "ColorSpaces.h"
#include "GeometryPlan.h"

#include <ColorEncoding.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <Photo.h>
#include <RenderCheckpoint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace arraw {

/// @brief Rectangle of whole pixels in a frame.
struct PixelRegion {
    std::uint32_t x = 0;      ///< Column of the left edge.
    std::uint32_t y = 0;      ///< Row of the top edge.
    std::uint32_t width = 0;  ///< Width in pixels.
    std::uint32_t height = 0; ///< Height in pixels.

    /// @brief Gives the size of the rectangle.
    [[nodiscard]] constexpr ImageSize size() const noexcept {
        return {width, height};
    }

    /// @brief Checks whether the rectangle is all of a frame.
    /// @param frame Size of the frame.
    [[nodiscard]] constexpr bool covers(ImageSize frame) const noexcept {
        return x == 0 && y == 0 && size() == frame;
    }

    friend bool operator==(const PixelRegion&, const PixelRegion&) = default;
};

/// @brief Resolves a request's region into whole pixels of a frame.
///
/// Each edge snaps outward: the left and top down, the right and bottom up,
/// and a region thinner than a pixel grows to one. No region gives the frame.
/// @param request What the caller wants rendered.
/// @param frame Size of the frame after geometry.
/// @return The pixels the region covers, inside @p frame, at least 1x1.
/// @throws std::invalid_argument if @p frame is empty or the region is not
/// finite, not within the unit square, or empty.
[[nodiscard]] PixelRegion regionOf(const RenderRequest& request, ImageSize frame);

/// @brief The resample block: the size a render ends at, and how it gets there.
///
/// ADR 011 puts a target size in the resample block and ADR 012 puts anything
/// that changes what a stage computes in the plan, so a render's requested size
/// and filter are resolved here rather than read from the request by each
/// backend. Both backends execute exactly this.
struct ResizePlan {
    /// @brief Part of the cropped frame that is resized, from ::arraw::regionOf.
    ///
    /// Always set by the planner, to the whole frame when no region was asked
    /// for. It belongs here and not in the geometry block so that a checkpoint
    /// after geometry is shared by every region (ADR 025).
    PixelRegion region;

    /// @brief Size of the result, from ::arraw::resolvedSize against the region's size.
    ImageSize outputSize;

    /// @brief Kernel the resize runs with.
    ///
    /// ::arraw::ResizeFilter::Lanczos3 for a resize that does not resample, whatever the
    /// request said, since no kernel runs and two renders with the same pixels
    /// should compare equal.
    ResizeFilter filter = ResizeFilter::Lanczos3;

    /// @brief Whether every alpha sample of the developed pixels is exactly one.
    ///
    /// An execution hint, not a plan input: it does not change the result, so
    /// it is left out of ::arraw::ResizePlan's equality, and two plans that
    /// differ only in it describe the same render (a plan from a
    /// ::arraw::Photo, which has no pixels to scan, still matches one from
    /// pixels). It is worked out from the source, which stands for the
    /// developed pixels only by two invariants: the pointwise chain copies
    /// alpha (see developSamples in `Develop.cpp`), and the geometry pass keeps
    /// the pixels it interpolates between exactly opaque (see `geometry.frag`).
    /// A setting that produced transparency would break both this hint and the
    /// fast path it enables, and must clear it.
    ///
    /// Opaque pixels need no tracking of transparency: no window is
    /// translucent, so no colour is clamped to a visible range and no quotient
    /// by alpha is taken, and the result's alpha is one by construction. Both
    /// backends then take a cheaper path with the same result (see
    /// ::arraw::resample). Only worked out for a resize that runs; `false` for an
    /// identity one, and `false` where the pixels were not at hand to be scanned
    /// (see ::arraw::planFor for a ::arraw::Photo).
    bool opaque = false;

    /// @brief Whether the resize leaves the pixels alone, which both backends skip.
    /// @param cropped Size of the photograph after its crop.
    [[nodiscard]] bool isIdentity(ImageSize cropped) const noexcept {
        return region.covers(cropped) && outputSize == cropped;
    }

    /// @brief Whether the kernel runs, as opposed to the region being cut and kept as it is.
    [[nodiscard]] bool resamples() const noexcept {
        return outputSize != region.size();
    }

    /// @brief Compares what the resize computes, ignoring the ::arraw::ResizePlan::opaque hint.
    friend bool operator==(const ResizePlan& first, const ResizePlan& second) noexcept {
        return first.region == second.region && first.outputSize == second.outputSize &&
               first.filter == second.filter;
    }
};

/// @brief Everything a photograph's settings imply, worked out once.
///
/// Settings are what a photographer sets; a plan is what the pixels need. The
/// separation is ADR 009's rule — no backend derives anything from
/// ::arraw::DevelopSettings itself — applied to the whole pipeline rather than
/// only to geometry: the CPU reference reads this, and the GPU will upload the
/// same values as its uniform block.
///
/// Settings that are switched off should resolve to a value that costs
/// nothing here rather than to a test inside the per-pixel loop.
struct ProcessingPlan {
    /// @brief Source primaries into the working space, white balance included.
    ///
    /// White balance, the camera matrix, and any change of primaries are all
    /// linear, so they compose into one transform and cost one multiply per
    /// pixel between them.
    Matrix3 toWorking = Matrix3::identity();

    /// @brief Linear gain that the Exposure setting asks for.
    float exposureGain = 1.0F;

    /// @brief Whether any tone control asks for the scale to be shaped.
    ///
    /// Nothing set resolves to `false`, and the chain skips the crossing into
    /// the perceptual coordinate and back — which costs two powers, and would
    /// return a value a hair away from the one it was given. A setting that is
    /// off falls out in the plan rather than as a test inside the loop
    /// (ADR 011).
    bool shapesTone = false;

    /// @brief Exponent the tone scale is raised to about middle grey.
    ///
    /// The Contrast setting resolved into the perceptual slope at the pivot.
    /// One leaves the tone scale alone.
    float contrastSlope = 1.0F;

    /// @brief What the raised value is multiplied by to bring grey back.
    ///
    /// `greyPivot^(1 - contrastSlope)`, worked out once: a power law through
    /// the pivot written as a multiply and a power rather than as a divide, a
    /// power and a multiply. With no contrast asked for it is exactly one, so
    /// the stage returns precisely the value it was given.
    float contrastScale = 1.0F;

    /// @brief How far the dark tones move, at the peak of their region.
    ///
    /// In the perceptual coordinate, where the controls act. The window each
    /// shift is spread over lives in the chain; the plan carries how far.
    float shadowShift = 0.0F;

    /// @copydoc shadowShift
    float highlightShift = 0.0F;

    /// @copydoc shadowShift
    float blackShift = 0.0F;

    /// @copydoc shadowShift
    float whiteShift = 0.0F;

    /// @brief Luminance at which highlights begin to roll toward white.
    ///
    /// Where the Filmic Highlights amount puts the bend, in linear luminance:
    /// a stronger amount brings the knee down out of white. A photograph asked
    /// for no roll-off resolves to a knee no luminance reaches, so the chain
    /// costs a comparison rather than a branch on a setting (ADR 011).
    float shoulderKnee = std::numeric_limits<float>::infinity();

    /// @brief Resolved geometry when source dimensions and orientation are known.
    std::optional<GeometryPlan> geometry = std::nullopt;

    /// @brief Resolved resize, when a geometry and so a cropped size is known.
    std::optional<ResizePlan> resize = std::nullopt;

    friend bool operator==(const ProcessingPlan&, const ProcessingPlan&) = default;
};

/// @brief Groups the plan's fields by the pass that consumes them.
///
/// ADR 011's partition, with the passes that exist today. The plan is still
/// flat — decode, lens, spots and noise are not stages yet — so this groups
/// fields rather than blocks, and becomes `std::tie(p.decode, ...)` as each of
/// those arrives. The grouping is what the prefix comparison folds over, so
/// there is no per-stage line to forget, only this list to keep honest.
/// @param plan Plan to view by stage.
/// @return One tuple element per ::arraw::Stage, in pipeline order.
[[nodiscard]] inline auto stagesOf(const ProcessingPlan& plan) {
    return std::make_tuple(std::tie(plan.toWorking, plan.exposureGain, plan.shapesTone,
                                    plan.contrastSlope, plan.contrastScale, plan.shadowShift,
                                    plan.highlightShift, plan.blackShift, plan.whiteShift,
                                    plan.shoulderKnee),
                           std::tie(plan.geometry), std::tie(plan.resize));
}

static_assert(std::tuple_size_v<decltype(stagesOf(std::declval<const ProcessingPlan&>()))> ==
                  stageCount,
              "every Stage needs a group in stagesOf, and no group may lack a Stage");

/// @brief Whether two plans agree on everything up to and including a boundary.
///
/// What decides that a ::arraw::RenderCheckpoint may still be resumed from, or
/// its pixels reused. Comparing whole plans would defeat the purpose — nudging
/// Exposure would discard a result that does not depend on it — so the fold
/// stops at the checkpoint's boundary (ADR 011).
///
/// Exact float equality is sound here: these are values our own code derived
/// deterministically from the same inputs, not measurements, and the failure
/// direction is safe, since a NaN never compares equal and simply recomputes.
///
/// This only works while the plan genuinely holds everything the stages
/// consume. That is ADR 009's discipline, and comparing plans is what makes a
/// violation fail rather than merely be untidy.
/// @param first One plan.
/// @param second The other.
/// @param upTo Last boundary to compare; later stages are not looked at.
/// @return `true` if @p upTo is a known boundary and every stage up to it
/// resolved identically.
[[nodiscard]] inline bool prefixMatches(const ProcessingPlan& first, const ProcessingPlan& second,
                                        Stage upTo) {
    const auto left = stagesOf(first);
    const auto right = stagesOf(second);
    const auto last = static_cast<std::size_t>(upTo);
    if (last >= stageCount) {
        return false;
    }
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return ((I > last || std::get<I>(left) == std::get<I>(right)) && ...);
    }(std::make_index_sequence<stageCount>{});
}

/// @brief Resolves tone settings into a plan with an identity colour transform.
/// @param settings Tone adjustments to resolve.
/// @return Exposure gain and tone coefficients for the pointwise chain.
/// @throws std::invalid_argument if a tone setting is not finite.
[[nodiscard]] ProcessingPlan tonePlanFor(const ToneSettings& settings);

/// @brief Resolves source primaries and colour settings into the working transform.
/// @param encoding Encoding the decoded pixels are in.
/// @param settings Colour adjustments to resolve.
/// @return The source-to-working matrix including white balance.
/// @throws std::invalid_argument if the encoding or colour settings cannot be resolved.
[[nodiscard]] Matrix3 colorMatrixFor(const ColorEncoding& encoding, const ColorSettings& settings);

/// @brief Works out what a photograph's settings mean for its pixels.
///
/// Resolves only colour and tone; buffer and Photo overloads also resolve geometry.
/// This overload carries no source identity and must not be used as a cache key.
/// @param encoding Encoding the decoded pixels are in.
/// @param state State to resolve.
/// @return The plan both backends execute.
/// @throws std::invalid_argument if development cannot start from @p encoding,
/// or the settings cannot be resolved against it.
[[nodiscard]] ProcessingPlan planFor(const ColorEncoding& encoding, const DevelopState& state);

/// @brief Resolves pointwise processing, geometry and the resize against decoded pixels.
///
/// The one overload that knows whether the pixels are opaque (see
/// ::arraw::ResizePlan::opaque). A source with no alpha channel is opaque for
/// free; one with alpha is scanned once, on the host, stopping at the first
/// sample that is not exactly one: a read of the alpha samples, at worst the
/// whole buffer, and only when the request actually resizes.
/// @param source Decoded pixels the plan is for.
/// @param state State to resolve.
/// @param request Size and filter to render at; the default is the cropped size.
/// @throws std::invalid_argument as the other overloads, and if @p request
/// cannot be resolved (see ::arraw::resolvedSize).
[[nodiscard]] ProcessingPlan planFor(const ImageBuffer& source, const DevelopState& state,
                                     const RenderRequest& request = {});

/// @brief Works out what a photograph's document means for its pixels.
///
/// What a render is planned against (ADR 012): the encoding comes from what
/// the file declared and the settings from the document that declared it, so
/// the two cannot arrive from different photographs.
///
/// A photograph carries no pixels, so whether they are opaque is unknown here:
/// the resize it plans is never marked ::arraw::ResizePlan::opaque.
/// @param photo Document to resolve.
/// @param request Size and filter to render at; the default is the cropped size.
/// @return The plan both backends execute.
/// @throws std::invalid_argument if the photograph's encoding or settings
/// cannot be resolved, or @p request cannot be resolved against its crop.
[[nodiscard]] ProcessingPlan planFor(const Photo& photo, const RenderRequest& request = {});

/// @brief Perceptual coordinate the tone controls act in.
///
/// Tone shaping happens on `y^(1/2.2)` rather than on scene-linear luminance:
/// linear 0.25 is upper-midtone grey, not the dark quarter, so a control that
/// acted there would put its whole range in the highlights (ADR 010).
/// @param luminance Linear luminance, zero or above.
/// @return The same brightness, perceptually spaced.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float toPerceptual(float luminance) {
    return std::pow(luminance, 1.0F / 2.2F);
}

/// @brief Returns a perceptual value to scene-linear luminance.
/// @param value Perceptually spaced brightness.
/// @return The linear luminance it stands for.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float toLinear(float value) {
    return std::pow(value, 2.2F);
}

/// @brief Middle grey in the perceptual coordinate, which Contrast pivots on.
///
/// An eighteen percent grey card, encoded: `0.18^(1/2.2)`, to the nearest
/// float, so that a contrast control leaves the value a photographer metered
/// for exactly where it was.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
inline constexpr float greyPivot = 0.45865646F;

/// @brief Smooth rise from zero to one between two edges.
/// @param first Edge below which the result is zero.
/// @param last Edge above which it is one.
/// @param value Where to evaluate it.
/// @return The eased fraction, never outside zero to one.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float smoothstep(float first, float last, float value) {
    const float t = std::clamp((value - first) / (last - first), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

/// @brief Weight of the Shadows region: zero at black, zero by the midtones.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float shadowWeight(float value) {
    return smoothstep(0.0F, 0.3F, value) * (1.0F - smoothstep(0.3F, 0.6F, value));
}

/// @brief Weight of the Highlights region, reaching a little past white.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float highlightWeight(float value) {
    return smoothstep(0.4F, 0.75F, value) * (1.0F - smoothstep(0.75F, 1.2F, value));
}

/// @brief Weight of the black end, full at black and gone by the shadows.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float blackWeight(float value) {
    return 1.0F - smoothstep(0.0F, 0.35F, value);
}

/// @brief Weight of the white end, full at white and above.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float whiteWeight(float value) {
    return smoothstep(0.6F, 1.0F, value);
}

/// @brief Shapes one luminance through the tone controls, in their fixed order.
///
/// Contrast is a straight line in log--log through the grey pivot: monotone
/// everywhere, never negative, and continuing smoothly past white rather than
/// flattening into it. The four regional controls are smooth windows added on
/// top, each fading out where the next one's territory begins (ADR 013).
/// @param plan Resolved settings.
/// @param luminance Linear luminance to shape.
/// @return The shaped linear luminance.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float shapeLuminance(const ProcessingPlan& plan, float luminance) {
    float value = toPerceptual(luminance);
    value = plan.contrastScale * std::pow(value, plan.contrastSlope);

    // The global shape first, then the regions, then the ends -- each reading
    // what the one before it left, and each moving its region by little enough
    // that no combination can fold the tone scale back on itself (ADR 013).
    value += plan.shadowShift * shadowWeight(value);
    value += plan.highlightShift * highlightWeight(value);
    value += plan.blackShift * blackWeight(value);
    value += plan.whiteShift * whiteWeight(value);
    return toLinear(std::max(value, 0.0F));
}

/// @brief Luminance at or below which tone treats a colour as black.
///
/// Below it a luminance cannot be told from black, and the ratio the tone
/// controls scale the colour by could be denormal or overflow, which GPUs flush
/// where CPUs do not. Both backends therefore take the lifted branch on the same
/// side of this value, a normal float far under anything a photograph holds.
/// NaN fails the comparison and lifts too. Mirrored by `liftedBlackThreshold` in
/// `src/gpu/shaders/develop.frag`.
inline constexpr float liftedBlackThreshold = 1.0e-20F;

/// @brief Applies the tone controls to a colour, through its luminance.
///
/// Tone shapes brightness and the colour follows by the ratio, so hue and
/// saturation come through untouched and colour work stays in Oklab where
/// ADR 010 puts it. A colour with no brightness has no ratio to scale by, and
/// takes the shaped value neutrally — which is what a lifted black is.
/// @param plan Resolved settings.
/// @param colour Colour in the working encoding.
/// @return The colour with its tone shaped.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr Colour shapeTone(const ProcessingPlan& plan, Colour colour) {
    if (!plan.shapesTone) {
        return colour;
    }

    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    if (!(luminance > liftedBlackThreshold)) {
        const float lifted = shapeLuminance(plan, 0.0F);
        return {lifted, lifted, lifted};
    }

    const float ratio = shapeLuminance(plan, luminance) / luminance;
    return {colour[0] * ratio, colour[1] * ratio, colour[2] * ratio};
}

/// @brief Rolls a colour's brightest values toward white rather than clipping.
///
/// The shoulder that ends the chain (ADR 010), as a bend in luminance: below
/// the knee nothing moves, above it the excess is compressed ever harder, so
/// the result approaches white without ever reaching it and two values that
/// would both have clipped stay apart. The bend starts at slope one, so a
/// gradient shows no edge where it begins.
///
/// Colour fades with it. Something genuinely overexposed loses colour as it
/// brightens, so as a value is pulled down its chroma is faded toward the
/// neutral of the same luminance — the "path to white". Brightness is the
/// shoulder's, colour is the fade's: the luminance that comes out is the one
/// the bend asked for either way.
/// @param plan Resolved settings.
/// @param colour Colour in the working encoding, possibly above white.
/// @return The colour with its highlights rolled.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr Colour rollHighlights(const ProcessingPlan& plan, Colour colour) {
    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    if (!(luminance > plan.shoulderKnee)) {
        return colour;
    }

    const float headroom = 1.0F - plan.shoulderKnee;
    const float above = (luminance - plan.shoulderKnee) / headroom;
    const float rolled = plan.shoulderKnee + headroom * (above / (1.0F + above));

    const float ratio = rolled / luminance;
    // Chroma fades as the square of how far the value was pulled down, times
    // its square root: gentle while the roll is gentle, complete by the time
    // the value is being crushed into white.
    const float chroma = ratio * std::sqrt(ratio);
    return {rolled + chroma * (colour[0] * ratio - rolled),
            rolled + chroma * (colour[1] * ratio - rolled),
            rolled + chroma * (colour[2] * ratio - rolled)};
}

/// @brief Applies the pointwise stages to one colour, in their fixed order.
///
/// The order lives here and nowhere else, so that it can be read in one place
/// and tested without a buffer. A fragment shader's `main` mirrors this
/// sequence by hand, and per-stage comparisons hold the two together (ADR 011).
/// @param plan Resolved settings.
/// @param colour Source colour, in the encoding the plan was built for.
/// @return The developed colour, in the working encoding.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr Colour developPixel(const ProcessingPlan& plan, Colour colour) {
    colour = plan.toWorking * colour;
    colour = {colour[0] * plan.exposureGain, colour[1] * plan.exposureGain,
              colour[2] * plan.exposureGain};
    colour = shapeTone(plan, colour);
    return rollHighlights(plan, colour);
}

} // namespace arraw
