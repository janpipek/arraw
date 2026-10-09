#pragma once

#include "ColorAdjustments.h"
#include "ColorSpaces.h"
#include "Denoise.h"
#include "Effects.h"
#include "GeometryPlan.h"
#include "PointwisePlan.h"

#include <ColorEncoding.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <Photo.h>
#include <RenderCheckpoint.h>

#include <algorithm>
#include <cassert>
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
    /// @brief Luminance and colour noise reduction, run on the source before anything else.
    ///
    /// Its own pass and the first group of `stagesOf` (ADR 039). It reads the
    /// source's as-shot luminance row, never ::arraw::PointwisePlan::toWorking, so that white
    /// balance and exposure cannot reach it. With both halves off it is the
    /// default, and the pass does not run.
    DenoisePlan denoise{};

    /// @brief White balance, tone, Presence, curves, shoulder and colour controls, resolved.
    ///
    /// The fused per-pixel chain (ADR 011), as one block.
    PointwisePlan pointwise{};

    /// @brief Resolved geometry when source dimensions and orientation are known.
    std::optional<GeometryPlan> geometry = std::nullopt;

    /// @brief Resolved resize, when a geometry and so a cropped size is known.
    std::optional<ResizePlan> resize = std::nullopt;

    /// @brief Vignette and grain, applied to the cropped frame after the resize.
    ///
    /// Its own pass, after the resize, because it reads the crop frame (ADR 037, ADR 038).
    /// Where a pixel lies in that frame comes from the geometry and the resize
    /// (::arraw::frameMappingOf), which the prefix before this group already
    /// compares. With every effect off it is the default, and the pass does not run.
    EffectsPlan effects{};

    friend bool operator==(const ProcessingPlan&, const ProcessingPlan&) = default;
};

/// @brief Groups the plan's blocks by the pass that consumes them.
///
/// ADR 011's partition, with the passes that exist today: one block per
/// ::arraw::Stage, so this is a flat `std::tie` and a pass that arrives (decode,
/// lens, spots) adds a block and a line. The grouping is what the prefix
/// comparison folds over, so there is no per-stage line to forget, only this
/// list to keep honest; a test pins that the plan has no member outside it.
/// @param plan Plan to view by stage.
/// @return One tuple element per ::arraw::Stage, in pipeline order.
[[nodiscard]] inline auto stagesOf(const ProcessingPlan& plan) {
    return std::tie(plan.denoise, plan.pointwise, plan.geometry, plan.resize, plan.effects);
}

static_assert(std::tuple_size_v<decltype(stagesOf(std::declval<const ProcessingPlan&>()))> ==
                  stageCount,
              "every Stage needs a group in stagesOf, and no group may lack a Stage");

/// @brief Gives where a render's output pixels lie in the crop frame.
///
/// `(region origin + (pixel + 0.5) * region / outputSize) / croppedSize` per
/// axis, from the resize block and the geometry's output size (ADR 037): the
/// same point of the frame maps to the same position whatever region, size or
/// pyramid level is rendered.
/// @param plan Plan with a geometry and a resize.
/// @return The mapping the Effects pass reads.
/// @pre @p plan has a geometry and a resize, as every plan from pixels or a
/// ::arraw::Photo does.
[[nodiscard]] FrameMapping frameMappingOf(const ProcessingPlan& plan);

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

/// @brief Gives the part of a request a render stopping after a boundary reads.
///
/// A render that stops before the resize ignores the request's size, region
/// and filter, whatever they say, and so has no use for the opacity scan
/// either. What the source's pixels are, such as their pixel scale, is read
/// from the source, not from the request (ADR 039).
/// @param request What the caller asked for.
/// @param stopAfter Last boundary the render runs.
/// @return @p request itself, or a default one.
[[nodiscard]] inline RenderRequest plannedRequest(const RenderRequest& request, Stage stopAfter) {
    if (stopAfter >= Stage::Resize) {
        return request;
    }
    return {};
}

/// @brief Resolves source primaries and colour settings into the working transform.
/// @param encoding Encoding the decoded pixels are in.
/// @param settings Colour adjustments to resolve.
/// @return The source-to-working matrix including white balance.
/// @throws std::invalid_argument if the encoding or colour settings cannot be resolved.
[[nodiscard]] Matrix3 colorMatrixFor(const ColorEncoding& encoding, const ColorSettings& settings);

/// @brief Works out what a photograph's settings mean for its pixels.
///
/// Resolves only noise reduction, colour and tone; buffer and Photo overloads
/// also resolve geometry, and Texture, Clarity and Dehaze, whose radius needs
/// the source's size: here they stay off. This overload carries no source
/// identity and must not be used as a cache key.
/// @param encoding Encoding the decoded pixels are in.
/// @param state State to resolve.
/// @param pixelScale Sensor pixels per pixel of the source (::arraw::ImageBuffer::pixelScale).
/// @return The plan both backends execute.
/// @throws std::invalid_argument if development cannot start from @p encoding,
/// or the settings cannot be resolved against it.
[[nodiscard]] ProcessingPlan planFor(const ColorEncoding& encoding, const DevelopState& state,
                                     double pixelScale = 1.0);

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

/// @brief Whether two plans give the same sample at a tap.
///
/// What tells a caller that a sample, or a histogram of one, is still current
/// after an edit: dragging a curve changes the plan but not the curve input,
/// so a curve widget need not sample again (ADR 035). Compares the denoise
/// block, which runs before the chain, the fields the chain reads up to
/// @p tap, then geometry and the resize, which a sample runs through as a
/// render does. Exact float equality, for the reasons
/// ::arraw::prefixMatches gives. The source is not part of a plan, so the
/// caller must also know that both are for the same pixels.
/// @param first One plan.
/// @param second The other.
/// @param tap Tap the sample was taken at.
/// @return `true` if @p tap is a tap and both plans resolve identically up to
/// it, and in geometry and the resize.
[[nodiscard]] inline bool sameAtTap(const ProcessingPlan& first, const ProcessingPlan& second,
                                    Tap tap) {
    const bool sameFrame = first.denoise == second.denoise && first.geometry == second.geometry &&
                           first.resize == second.resize;
    switch (tap) {
    case Tap::CurveInput:
        return sameFrame &&
               curveInputFieldsOf(first.pointwise) == curveInputFieldsOf(second.pointwise);
    }
    return false;
}

/// @brief Encodes one linear value into ::arraw::perceptualEncoding.
///
/// ::arraw::toPerceptual made odd: `sign(v) * |v|^(1/2.2)`, so that a negative
/// channel, which only a colour outside the working gamut has, keeps its
/// magnitude rather than collapsing onto black. NaN stays NaN.
/// @param value Linear channel value.
/// @return The value in the perceptual coordinate.
[[nodiscard]] inline float toPerceptualSigned(float value) {
    return value < 0.0F ? -toPerceptual(-value) : toPerceptual(value);
}

/// @brief Decodes one value from ::arraw::perceptualEncoding back to linear.
/// @param value Channel value in the perceptual coordinate.
/// @return The linear value: `sign(v) * |v|^2.2`.
[[nodiscard]] inline float fromPerceptualSigned(float value) {
    return value < 0.0F ? -toLinear(-value) : toLinear(value);
}

} // namespace arraw
