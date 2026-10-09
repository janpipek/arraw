#include "GpuDevelop.h"

#include "CheckpointState.h"
#include "GpuPlan.h"
#include "LadderAccess.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"
#include "RenderProgress.h"
#include "Resample.h"
#include "SampleConversion.h"
#include "StageDriver.h"
#include "Taps.h"
#include "TimingTrace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

namespace arraw {

namespace {

/// @brief Views a uniform block as the bytes a pass takes.
template <typename Block> std::span<const std::byte> bytesOf(const Block& block) {
    return std::as_bytes(std::span(&block, 1));
}

/// @brief Resizes a device image, as ::arraw::resample does on the host.
///
/// Four renders in general: the horizontal shader runs once per plane over the
/// image, and the vertical one reads the three planes and writes the result.
/// An opaque image needs no planes beyond the sums, so it takes two.
///
/// The plan's region is cut by the horizontal pass, which reads the image at an
/// offset and clamps to the region's own edges, with weights made for the
/// region's size: the same taps the host resamples a cut-out copy with, and no
/// copy. A region that is only cut runs the same passes with identity weights.
DeviceImage resizeOnGpu(GpuContext& context, const DeviceImage& image, const ResizePlan& resize) {
    const ImageSize size = resize.region.size();
    const ImageSize target = resize.outputSize;
    const std::array<std::uint32_t, 2> offset{resize.region.x, resize.region.y};
    const DeviceImage acrossWeights =
        context.upload(packResizeWeights(size.width, target.width, resize.filter));
    const DeviceImage downWeights =
        context.upload(packResizeWeights(size.height, target.height, resize.filter));

    const ImageSize widened{target.width, size.height};
    // The result spans more sensor pixels per pixel, by the reduction, as on the host.
    const GpuTarget result{.pixelScale =
                               resampledPixelScale(image.pixelScale(), size.width, target.width)};
    if (resize.opaque) {
        const GpuResizeBlock across{.plane = 0, .inputLength = size.width, .offset = offset};
        const GpuResizeBlock down{.plane = 0, .inputLength = size.height};
        const DeviceImage sums =
            context.render(GpuPass::ResizeAcrossOpaque, bytesOf(across),
                           std::array{image, acrossWeights}, widened, workingEncoding);
        return context.render(GpuPass::ResizeDownOpaque, bytesOf(down),
                              std::array{sums, downWeights}, target, workingEncoding, result);
    }

    std::array<DeviceImage, resizePlaneCount> planes;
    for (std::size_t plane = 0; plane < resizePlaneCount; ++plane) {
        const GpuResizeBlock block{.plane = static_cast<std::uint32_t>(plane),
                                   .inputLength = size.width,
                                   .offset = offset};
        const std::array inputs{image, acrossWeights};
        planes[plane] =
            context.render(GpuPass::ResizeAcross, bytesOf(block), inputs, widened, workingEncoding);
    }
    const GpuResizeBlock block{.plane = 0, .inputLength = size.height};
    const std::array inputs{planes[static_cast<std::size_t>(ResizePlane::Sums)], downWeights,
                            planes[static_cast<std::size_t>(ResizePlane::Low)],
                            planes[static_cast<std::size_t>(ResizePlane::High)]};
    return context.render(GpuPass::ResizeDown, bytesOf(block), inputs, target, workingEncoding,
                          result);
}

/// @brief Runs the Denoise pass on a device image, as ::arraw::applyDenoise does on the host.
///
/// Up to six renders: the colour half reduces the source to its grid and blurs
/// it across and down, the luminance half runs its filter (the bilateral's two
/// steps), and one render puts the pixel back together. A half that is off
/// renders nothing, and the source stands in for its input to the last render.
///
/// Each intermediate is dropped as soon as the next step has read it, and the
/// filtered luminance is a one-channel target, so the pass holds at most the
/// source, one RGBA intermediate of its size and that luminance besides its
/// result (ADR 039).
DeviceImage denoiseOnGpu(GpuContext& context, const DeviceImage& source, const DenoisePlan& plan) {
    const ImageSize size = source.size();
    const ColorEncoding& encoding = source.encoding();
    const auto step = [&](DenoiseStep which, const DeviceImage& input, ImageSize output,
                          GpuTargetFormat format = GpuTargetFormat::Rgba32F) {
        const GpuDenoiseBlock block = packDenoise(plan, size, which);
        return context.render(GpuPass::DenoiseFilter, bytesOf(block), input, output, encoding,
                              {.format = format});
    };
    // One unit a render: up to three for the colour, two for the luminance, one to combine.
    const detail::ProgressSpan progress(ProgressStep::Denoise,
                                        (plan.color ? 3U : 0U) + (plan.luminance ? 2U : 0U) + 1U);
    DeviceImage ratios = source;
    if (plan.color) {
        const ImageSize grid = denoiseGridSize(plan, size);
        ratios = step(DenoiseStep::Reduce, source, grid);
        ratios = step(DenoiseStep::BlurAcross, ratios, grid);
        ratios = step(DenoiseStep::BlurDown, ratios, grid);
    }
    DeviceImage luma = source;
    if (plan.luminance) {
        // The luminance filter seam: every filter is steps from the source to
        // a luminance in r, which the combination reads.
        switch (plan.filter) {
        case LuminanceNoiseFilter::Bilateral:
            // Across writes (luminance, perceptual) so that down reads both
            // rather than evaluating pow per tap, as the CPU's planes do.
            // QRhi has no two-channel float target, so it is RGBA32F; down's
            // scalar result replaces it at once.
            luma = step(DenoiseStep::BilateralAcross, source, size);
            luma = step(DenoiseStep::BilateralDown, luma, size, GpuTargetFormat::R32F);
            break;
        }
    }
    const GpuDenoiseBlock block = packDenoise(plan, size, DenoiseStep::Combine);
    const std::array inputs{source, ratios, luma};
    return context.render(GpuPass::DenoiseCombine, bytesOf(block), inputs, size, encoding);
}

/// @brief Runs one step of the Presence context on a device, into a one-channel target.
/// @param limit The grid the step bounds its result by: the opened grid
/// ::arraw::PresenceStep::BlurDownAboveOpening keeps above, the cells
/// ::arraw::PresenceStep::Reconstruct keeps below; null for every other step,
/// where @p from stands in.
DeviceImage presenceStepOnGpu(GpuContext& context, const PresencePlan& plan,
                              const PresenceBase& base, ImageSize source, PresenceStep step,
                              const DeviceImage& from, const DeviceImage* limit = nullptr) {
    const GpuPresenceBlock block = packPresence(plan, base, source, step);
    const std::array inputs{from, limit != nullptr ? *limit : from};
    return context.render(GpuPass::PresenceFilter, bytesOf(block), inputs,
                          presenceGridSize(base, source), workingEncoding,
                          {.format = GpuTargetFormat::R32F});
}

/// @brief Computes one base of the Presence context from its cells on a device, as
/// ::arraw::presenceContextOf does on the host.
///
/// The opening by the octagon when the base has one, the minimum across, down
/// and along both diagonals, then the maximum along the same four, and the
/// steps of its reconstruction, then the blur across and down, a render each,
/// the last kept above the opening when there is one (ADR 041).
DeviceImage presenceBaseOnGpu(GpuContext& context, const DeviceImage& cells,
                              const PresencePlan& plan, const PresenceBase& base,
                              ImageSize source) {
    if (base.window == 0) {
        const DeviceImage across =
            presenceStepOnGpu(context, plan, base, source, PresenceStep::BlurAcross, cells);
        return presenceStepOnGpu(context, plan, base, source, PresenceStep::BlurDown, across);
    }
    // As baseOf(): the same four directions in the same order, for the minimum
    // then the maximum. Every pass renders even when the octagon's diagonal is
    // zero (a window of one or two cells), so the count is fixed.
    constexpr std::array opening{
        PresenceStep::MinimumAcross,   PresenceStep::MinimumDown,
        PresenceStep::MinimumDiagonal, PresenceStep::MinimumAntidiagonal,
        PresenceStep::MaximumAcross,   PresenceStep::MaximumDown,
        PresenceStep::MaximumDiagonal, PresenceStep::MaximumAntidiagonal,
    };
    DeviceImage opened = cells;
    for (const PresenceStep step : opening) {
        opened = presenceStepOnGpu(context, plan, base, source, step, opened);
    }
    for (std::uint32_t index = 0; index < base.reconstruction; ++index) {
        opened = presenceStepOnGpu(context, plan, base, source, PresenceStep::Reconstruct, opened,
                                   &cells);
    }
    const DeviceImage across =
        presenceStepOnGpu(context, plan, base, source, PresenceStep::BlurAcross, opened);
    return presenceStepOnGpu(context, plan, base, source, PresenceStep::BlurDownAboveOpening,
                             across, &opened);
}

/// @brief The Presence context on a device: one image a grid, the pass's input for one that is
/// off.
struct DevicePresence {
    DeviceImage fine;
    DeviceImage coarse;
    DeviceImage coarseCells;
    DeviceImage haze;
};

/// @brief Computes the Presence context of the pointwise pass's input on a device.
///
/// Texture's base from its own cells; Clarity's base and Dehaze's from one
/// reduction to the coarse cells, which Clarity and a positive Dehaze also read
/// unblurred.
DevicePresence presenceOnGpu(GpuContext& context, const DeviceImage& input,
                             const PresencePlan& plan) {
    const ImageSize size = input.size();
    DevicePresence result{input, input, input, input};
    // One unit a render: a reduction for each cell, and each base's steps.
    const auto baseRenders = [](const PresenceBase& base) {
        return base.window == 0 ? 2U : 8U + base.reconstruction + 2U;
    };
    std::uint32_t renders = 0;
    if (plan.fine.active()) {
        renders += 1 + baseRenders(plan.fine);
    }
    if (plan.coarse.active() || plan.haze.active()) {
        renders += 1 + (plan.coarse.active() ? baseRenders(plan.coarse) : 0U) +
                   (plan.haze.active() ? baseRenders(plan.haze) : 0U);
    }
    const detail::ProgressSpan progress(ProgressStep::Context, renders);
    if (plan.fine.active()) {
        const DeviceImage cells =
            presenceStepOnGpu(context, plan, plan.fine, size, PresenceStep::Reduce, input);
        result.fine = presenceBaseOnGpu(context, cells, plan, plan.fine, size);
    }
    if (plan.coarse.active() || plan.haze.active()) {
        const PresenceBase& shared = plan.coarse.active() ? plan.coarse : plan.haze;
        const DeviceImage cells =
            presenceStepOnGpu(context, plan, shared, size, PresenceStep::Reduce, input);
        if (plan.coarse.active()) {
            result.coarse = presenceBaseOnGpu(context, cells, plan, plan.coarse, size);
        }
        if (plan.coarse.active() || plan.haze.window != 0) {
            result.coarseCells = cells;
        }
        if (plan.haze.active()) {
            result.haze = presenceBaseOnGpu(context, cells, plan, plan.haze, size);
        }
    }
    return result;
}

} // namespace

DeviceImage uploadSource(GpuContext& context, const ImageBuffer& source) {
    const detail::TimingSpan timing("gpu.prepare-source");
    return source.format() == PixelFormat::RgbaF32 ? context.upload(source)
                                                   : context.upload(toRgbaF32(source));
}

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DevelopState& state, Stage stopAfter,
                              const RenderRequest& request, ProgressChannel* progress) {
    // One path: the transfer this call pays for is the only difference.
    // Validating first keeps a bad argument from costing an upload.
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    const DeviceImage uploaded = uploadSource(context, source);
    return developOnGpu(context, source, uploaded, state, stopAfter, request, progress);
}

namespace {

/// @brief Checks that an uploaded source is this device's copy of a host source.
/// @throws std::invalid_argument if it is not.
void requireUploaded(const GpuContext& context, const ImageBuffer& source,
                     const DeviceImage& uploaded) {
    if (!uploaded.valid() || uploaded.device() != context.id()) {
        throw std::invalid_argument("The uploaded source does not belong to this device");
    }
    if (uploaded.size() != source.size()) {
        throw std::invalid_argument("The uploaded source is not of the source's size");
    }
}

/// @brief Runs the Pointwise pass on a device image.
///
/// Uploads the curves' tables when a curve is active and the probe reads them,
/// works out the Presence context, and renders the chain (or a tap's prefix
/// of it).
/// @param probe What the pass writes: the developed colour, or a tap's.
DeviceImage pointwiseOnGpu(GpuContext& context, const DeviceImage& image,
                           const ProcessingPlan& plan, PointwiseProbe probe) {
    // The block's grid sizes come from the image the bases are rendered from,
    // so the two cannot differ.
    const GpuPointwiseBlock pointwise = packPointwise(plan.pointwise, image.size(), probe);
    // The curves' tables are uploaded here, once per development that runs
    // this pass: a resumed one starts after it and pays nothing. With no
    // active curve, or a probe that stops before the curves, the shader
    // never reads the second input, so the source stands in for it and
    // nothing is uploaded.
    const ToneCurvePlan& tones = plan.pointwise.toneCurves;
    const bool anyCurve =
        tones.luma.active || tones.red.active || tones.green.active || tones.blue.active;
    const DeviceImage curves =
        anyCurve && probeReadsCurves(probe) ? context.upload(packToneCurves(tones)) : image;
    // The Presence context is worked out from this pass's input every time
    // the pass runs, rather than kept with a checkpoint (ADR 041); with
    // Presence off nothing is rendered and the image stands in for every grid.
    const DevicePresence around = presenceOnGpu(context, image, plan.pointwise.presence);
    const std::array inputs{image,      curves, around.fine, around.coarse, around.coarseCells,
                            around.haze};
    const detail::ProgressSpan progress(ProgressStep::Pointwise, 1);
    return context.render(GpuPass::Pointwise, bytesOf(pointwise), inputs, image.size(),
                          workingEncoding);
}

/// @brief Runs the Geometry pass on a device image, as ::arraw::applyGeometry does on the host.
DeviceImage geometryOnGpu(GpuContext& context, const DeviceImage& image,
                          const GeometryPlan& geometry) {
    const GpuGeometryBlock block = packGeometry(geometry);
    const detail::ProgressSpan progress(ProgressStep::Geometry, 1);
    return context.render(GpuPass::Geometry, bytesOf(block), image, geometry.outputSize,
                          workingEncoding);
}

/// @brief Runs the Effects pass on a device image, as ::arraw::applyEffects does on the host.
DeviceImage effectsOnGpu(GpuContext& context, const DeviceImage& image,
                         const ProcessingPlan& plan) {
    const GpuEffectsBlock block = packEffects(plan);
    const detail::ProgressSpan progress(ProgressStep::Effects, 1);
    return context.render(GpuPass::Effects, bytesOf(block), image, image.size(), workingEncoding);
}

/// @brief The GPU's passes, for ::arraw::runStages.
///
/// The pixels are device images, which are shared handles: a checkpoint and a
/// borrow of it cost nothing.
struct GpuStages {
    using Pixels = DeviceImage;

    /// Device the images live on.
    GpuContext& context;
    /// What the pointwise pass writes: the developed colour, or a tap's.
    PointwiseProbe probe = PointwiseProbe::Developed;

    /// @brief Runs the pass that ends at a stage's boundary.
    Pixels run(Stage stage, Pixels image, const ProcessingPlan& plan) const {
        switch (stage) {
        case Stage::Denoise:
            return denoiseOnGpu(context, image, plan.denoise);
        case Stage::Pointwise:
            return pointwiseOnGpu(context, image, plan, probe);
        case Stage::Geometry:
            return geometryOnGpu(context, image, *plan.geometry);
        case Stage::Resize: {
            const detail::ProgressSpan progress(ProgressStep::Resize,
                                                plan.resize->opaque ? 2U : resizePlaneCount + 1);
            return resizeOnGpu(context, image, *plan.resize);
        }
        case Stage::Effects:
            return effectsOnGpu(context, image, plan);
        }
        throw std::logic_error("A render ran a pass that does not exist");
    }

    /// @brief Makes a checkpoint of the image at a boundary, sharing it.
    RenderCheckpoint checkpoint(Stage stage, Pixels image, const ProcessingPlan& plan) const {
        return makeCheckpoint(stage, plan, std::move(image));
    }

    /// @brief Gives a checkpoint's image.
    Pixels borrow(const RenderCheckpoint& checkpoint) const {
        return std::get<DeviceImage>(stateOf(checkpoint).pixels);
    }
};

static_assert(StageBackend<GpuStages>);

/// @brief Runs the passes after a boundary and keeps the result as a checkpoint.
///
/// Observed as ::arraw::developUntil and ::arraw::resumeFrom are, against the
/// whole render @p request asks for; nothing is made into a checkpoint until
/// the last pass returned, so a cancelled render keeps none.
/// @param request What the whole render is asked for, which sizes the shares.
/// @param progress The caller's channel, or null.
RenderCheckpoint developStages(GpuContext& context, std::optional<Stage> done, DeviceImage image,
                               ProcessingPlan plan, Stage stopAfter, const RenderRequest& request,
                               ProgressChannel* progress) {
    const detail::TimingSpan timing("gpu.develop");
    detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                              done ? detail::stepAfter(*done) : ProgressStep::Denoise);
    GpuStages backend{context};
    StagesRun<GpuStages> run = runStages(backend, done, std::move(image), plan, stopAfter);
    root.finish(detail::stepThrough(run.done));
    return makeCheckpoint(run.done, std::move(plan), std::move(run.pixels));
}

} // namespace

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DeviceImage& uploaded, const DevelopState& state,
                              Stage stopAfter, const RenderRequest& request,
                              ProgressChannel* progress) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    requireUploaded(context, source, uploaded);
    // Only a render that reaches the resize plans one: stopping earlier ignores
    // the request, whatever it says, and has no use for the opacity scan.
    ProcessingPlan plan = planFor(source, state, plannedRequest(request, stopAfter));
    return developStages(context, std::nullopt, uploaded, std::move(plan), stopAfter, request,
                         progress);
}

RenderCheckpoint developOnGpu(GpuContext& context, const RenderCheckpoint& from,
                              const ImageBuffer& source, const DevelopState& state, Stage stopAfter,
                              const RenderRequest& request, ProgressChannel* progress) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    const CheckpointState& held = stateOf(from);
    const auto* image = std::get_if<DeviceImage>(&held.pixels);
    if (image == nullptr) {
        throw std::invalid_argument("A checkpoint in host memory cannot be resumed on the GPU");
    }
    if (image->device() != context.id()) {
        throw std::invalid_argument("The checkpoint belongs to another device");
    }
    ProcessingPlan plan = planFor(source, state, plannedRequest(request, stopAfter));
    requireResumable(held, plan, source.size(), stopAfter);
    if (stopAfter == held.boundary) {
        // Nothing to run, but a cancelled channel still says so, as on the CPU.
        detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                                  detail::stepAfter(held.boundary));
        root.finish(detail::stepThrough(stopAfter));
        return from;
    }
    return developStages(context, held.boundary, *image, std::move(plan), stopAfter, request,
                         progress);
}

LadderRender resumeOrDevelopOnGpu(GpuContext& context, CheckpointLadder& ladder,
                                  std::shared_ptr<const ImageBuffer> source,
                                  const DeviceImage& uploaded, const DevelopState& state,
                                  const RenderRequest& request, ProgressChannel* progress) {
    if (!source) {
        throw std::invalid_argument("A render through a ladder needs a source");
    }
    requireUploaded(context, *source, uploaded);
    // Planned before the ladder is touched, so that a bad request drops nothing.
    ProcessingPlan plan = planFor(*source, state, request);
    LadderAccess::bind(ladder, source);
    // A rung on the host, or on another device, cannot serve this render.
    const std::optional<Stage> resumedFrom = LadderAccess::deepestUsable(
        ladder, plan, source->size(), [&context](const CheckpointState& rung) {
            const auto* image = std::get_if<DeviceImage>(&rung.pixels);
            return image != nullptr && image->device() == context.id();
        });
    DeviceImage start =
        resumedFrom
            ? std::get<DeviceImage>(stateOf(LadderAccess::rung(ladder, *resumedFrom)).pixels)
            : uploaded;
    const detail::TimingSpan timing("gpu.develop");
    detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                              resumedFrom ? detail::stepAfter(*resumedFrom)
                                          : ProgressStep::Denoise);
    GpuStages backend{context};
    StagesRun<GpuStages> run =
        runStages(backend, resumedFrom, std::move(start), plan, Stage::Effects, &ladder);
    root.finish(ProgressStep::Effects);
    return {makeCheckpoint(Stage::Effects, std::move(plan), std::move(run.pixels)), resumedFrom};
}

ImageBuffer sampleOnGpu(GpuContext& context, const ImageBuffer& source, const DeviceImage& uploaded,
                        const DevelopState& state, Tap tap, const RenderRequest& request,
                        ProgressChannel* progress) {
    const PointwiseProbe probe = probeFor(tap);
    requireUploaded(context, source, uploaded);
    const detail::TimingSpan timing("gpu.sample");
    const ProcessingPlan plan = planFor(source, state, request);
    // As ::arraw::sample: no effects, so they take no share.
    detail::StepWeights weights = detail::observedStepWeights(progress, plan, request);
    weights[static_cast<std::size_t>(ProgressStep::Effects)] = 0.0;
    detail::ProgressRoot root(progress, weights, ProgressStep::Denoise);
    GpuStages backend{context, probe};
    const StagesRun<GpuStages> run =
        runStages(backend, std::nullopt, uploaded, plan, Stage::Resize);
    ImageBuffer encoded = encodeTap(run.pixels.readBack(), tap);
    root.finish(ProgressStep::Resize);
    return encoded;
}

ImageBuffer sampleOnGpu(GpuContext& context, const ImageBuffer& source, const DevelopState& state,
                        Tap tap, const RenderRequest& request, ProgressChannel* progress) {
    // Validating first keeps a bad tap from costing an upload.
    static_cast<void>(probeFor(tap));
    const DeviceImage uploaded = uploadSource(context, source);
    return sampleOnGpu(context, source, uploaded, state, tap, request, progress);
}

} // namespace arraw
