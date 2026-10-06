#include "GpuDevelop.h"

#include "CheckpointState.h"
#include "GpuPlan.h"
#include "ProcessingPlan.h"
#include "Resample.h"
#include "SampleConversion.h"
#include "Taps.h"
#include "TimingTrace.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
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
/// The opening by the window when the base has one, the minimum across and
/// down then the maximum across and down and the steps of its reconstruction,
/// then the blur across and down, a render each, the last kept above the
/// opening when there is one (ADR 041).
DeviceImage presenceBaseOnGpu(GpuContext& context, const DeviceImage& cells,
                              const PresencePlan& plan, const PresenceBase& base,
                              ImageSize source) {
    if (base.window == 0) {
        const DeviceImage across =
            presenceStepOnGpu(context, plan, base, source, PresenceStep::BlurAcross, cells);
        return presenceStepOnGpu(context, plan, base, source, PresenceStep::BlurDown, across);
    }
    DeviceImage opened = cells;
    opened = presenceStepOnGpu(context, plan, base, source, PresenceStep::MinimumAcross, opened);
    opened = presenceStepOnGpu(context, plan, base, source, PresenceStep::MinimumDown, opened);
    opened = presenceStepOnGpu(context, plan, base, source, PresenceStep::MaximumAcross, opened);
    opened = presenceStepOnGpu(context, plan, base, source, PresenceStep::MaximumDown, opened);
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
                              const RenderRequest& request) {
    // One path: the transfer this call pays for is the only difference.
    // Validating first keeps a bad argument from costing an upload.
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    const DeviceImage uploaded = uploadSource(context, source);
    return developOnGpu(context, source, uploaded, state, stopAfter, request);
}

namespace {

/// @brief Pixels on the device, and the boundary they were taken at.
struct PassResult {
    DeviceImage image; ///< The pixels.
    Stage done;        ///< Last boundary run.
};

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

/// @brief Runs the passes after a boundary, up to another, on an image taken at the first.
///
/// The one path for every GPU development and sample: a fresh one starts at
/// the Denoise pass, a resumed one after the boundary it resumes from, and
/// each pass is skipped under exactly the condition the CPU skips it.
/// @param context Device the images live on.
/// @param done Boundary @p image was taken at, or empty for the uploaded source.
/// @param image Source pixels, or the pixels at @p done.
/// @param plan The plan that makes the rest.
/// @param stopAfter Last boundary to run.
/// @param probe What the pointwise pass writes: the developed colour, or a tap's.
PassResult runPasses(GpuContext& context, std::optional<Stage> done, DeviceImage image,
                     const ProcessingPlan& plan, Stage stopAfter,
                     PointwiseProbe probe = PointwiseProbe::Developed) {
    if (!done) {
        // As denoisedCopy and pointwiseFromSource: with noise reduction off the
        // boundary collapses onto the source, which is shared, not copied.
        if (plan.denoise.active()) {
            image = denoiseOnGpu(context, image, plan.denoise);
        }
        done = Stage::Denoise;
    }
    if (*done == Stage::Denoise && stopAfter != Stage::Denoise) {
        // The block's grid sizes come from the plan's geometry and the bases are rendered
        // from the image: they must be of one size, or Presence would misread its grids.
        assert(!plan.presence.active() ||
               (plan.geometry && plan.geometry->sourceSize == image.size()));
        const GpuPointwiseBlock pointwise = packPointwise(plan, probe);
        // The curves' tables are uploaded here, once per development that runs
        // this pass: a resumed one starts after it and pays nothing. With no
        // active curve, or a probe that stops before the curves, the shader
        // never reads the second input, so the source stands in for it and
        // nothing is uploaded.
        const ToneCurvePlan& tones = plan.toneCurves;
        const bool anyCurve =
            tones.luma.active || tones.red.active || tones.green.active || tones.blue.active;
        const DeviceImage curves =
            anyCurve && probeReadsCurves(probe) ? context.upload(packToneCurves(tones)) : image;
        // The Presence context is worked out from this pass's input every time
        // the pass runs, rather than kept with a checkpoint (ADR 041); with
        // Presence off nothing is rendered and the image stands in for every grid.
        const DevicePresence around = presenceOnGpu(context, image, plan.presence);
        const std::array inputs{image,      curves, around.fine, around.coarse, around.coarseCells,
                                around.haze};
        image = context.render(GpuPass::Pointwise, bytesOf(pointwise), inputs, image.size(),
                               workingEncoding);
        done = Stage::Pointwise;
    }
    if (*done == Stage::Pointwise && stopAfter != Stage::Pointwise) {
        // As applyGeometry: an identity geometry leaves the developed pixels as
        // they are, rather than resampling them onto themselves.
        const GeometryPlan& geometry = *plan.geometry;
        if (!geometry.isIdentity()) {
            const GpuGeometryBlock block = packGeometry(geometry);
            image = context.render(GpuPass::Geometry, bytesOf(block), image, geometry.outputSize,
                                   workingEncoding);
        }
        done = Stage::Geometry;
    }
    if (*done == Stage::Geometry && stopAfter >= Stage::Resize) {
        // As resample: a size equal to the cropped one is not resized, and the
        // pixels are reused as they are.
        if (!plan.resize->isIdentity(plan.geometry->outputSize)) {
            image = resizeOnGpu(context, image, *plan.resize);
        }
        done = Stage::Resize;
    }
    if (*done == Stage::Resize && stopAfter == Stage::Effects) {
        // As effectsBy: with every effect off the boundary collapses onto the
        // resize, and the image is shared rather than rendered again.
        if (plan.effects.active()) {
            const GpuEffectsBlock block = packEffects(plan);
            image = context.render(GpuPass::Effects, bytesOf(block), image, image.size(),
                                   workingEncoding);
        }
        done = Stage::Effects;
    }
    return {std::move(image), *done};
}

/// @brief Runs the passes and keeps the result as a checkpoint.
RenderCheckpoint developPasses(GpuContext& context, std::optional<Stage> done, DeviceImage image,
                               ProcessingPlan plan, Stage stopAfter) {
    const detail::TimingSpan timing("gpu.develop");
    PassResult result = runPasses(context, done, std::move(image), plan, stopAfter);
    return makeCheckpoint(result.done, std::move(plan), std::move(result.image));
}

} // namespace

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DeviceImage& uploaded, const DevelopState& state,
                              Stage stopAfter, const RenderRequest& request) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    requireUploaded(context, source, uploaded);
    // Only a render that reaches the resize plans one: stopping earlier ignores
    // the request, whatever it says, and has no use for the opacity scan.
    ProcessingPlan plan = planFor(source, state, plannedRequest(request, stopAfter));
    return developPasses(context, std::nullopt, uploaded, std::move(plan), stopAfter);
}

RenderCheckpoint developOnGpu(GpuContext& context, const RenderCheckpoint& from,
                              const ImageBuffer& source, const DevelopState& state, Stage stopAfter,
                              const RenderRequest& request) {
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
        return from;
    }
    return developPasses(context, held.boundary, *image, std::move(plan), stopAfter);
}

ImageBuffer sampleOnGpu(GpuContext& context, const ImageBuffer& source, const DeviceImage& uploaded,
                        const DevelopState& state, Tap tap, const RenderRequest& request) {
    const PointwiseProbe probe = probeFor(tap);
    requireUploaded(context, source, uploaded);
    const detail::TimingSpan timing("gpu.sample");
    const ProcessingPlan plan = planFor(source, state, request);
    const PassResult result =
        runPasses(context, std::nullopt, uploaded, plan, Stage::Resize, probe);
    return encodeTap(result.image.readBack(), tap);
}

ImageBuffer sampleOnGpu(GpuContext& context, const ImageBuffer& source, const DevelopState& state,
                        Tap tap, const RenderRequest& request) {
    // Validating first keeps a bad tap from costing an upload.
    static_cast<void>(probeFor(tap));
    const DeviceImage uploaded = uploadSource(context, source);
    return sampleOnGpu(context, source, uploaded, state, tap, request);
}

} // namespace arraw
