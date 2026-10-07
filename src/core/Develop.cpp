#include "Develop.h"

#include "CheckpointState.h"
#include "Denoise.h"
#include "Effects.h"
#include "Presence.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"
#include "RenderProgress.h"
#include "Resample.h"
#include "RowBands.h"
#include "SampleConversion.h"
#include "Taps.h"
#include "TimingTrace.h"

#include <Progress.h>
#include <WhiteBalance.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <variant>

using namespace arraw;

namespace {

/// @brief Runs the pointwise chain over every pixel of one layout.
///
/// The order itself is in ::arraw::developPixel (or a tap's prefix of it);
/// this is only the traversal, deliberately small enough that nothing can hide
/// in it. Alpha is copied rather than developed: no setting produces
/// transparency, and a source that carried some keeps exactly what it had.
/// ::arraw::ResizePlan::opaque, scanned from the source, relies on this: were
/// alpha ever changed here, it would no longer describe the developed pixels.
/// @param chain What one colour goes through: developPixel, or a tap's prefix,
/// called with the colour and the pixel's ::arraw::PixelContext.
/// @param context The Presence context of @p source, or null when Presence is off.
template <typename Sample, typename Chain>
void developSamples(const ImageBuffer& source, ImageBuffer& result, const Chain& chain,
                    const PresenceContext* context) {
    const auto input = source.samples<Sample>();
    const auto output = result.samples<float>();
    const std::size_t channels = channelCount(source.format());
    const ImageSize size = source.size();

    // Every pixel depends on its own colour and the context alone, so bands of
    // rows on threads give the single-threaded bits (ADR 041).
    detail::forEachRowBand(size.height, size.width, [&](std::uint32_t first, std::uint32_t last) {
        std::optional<PresenceSampler> sampler;
        if (context != nullptr) {
            sampler.emplace(*context, size);
        }
        for (std::uint32_t y = first; y < last; ++y) {
            if (sampler) {
                sampler->setRow(y);
            }
            for (std::uint32_t x = 0; x < size.width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * size.width + x;
                const auto* in = &input[pixel * channels];
                auto* out = &output[pixel * 4];

                const PixelContext around = sampler ? sampler->at(x) : PixelContext{};
                const Colour developed =
                    chain({toUnit(in[0]), toUnit(in[1]), toUnit(in[2])}, around);
                out[0] = developed[0];
                out[1] = developed[1];
                out[2] = developed[2];
                out[3] = channels == 4 ? toUnit(in[3]) : 1.0F;
            }
        }
    });
}

/// @brief Runs the pointwise chain, or a tap's prefix of it, over a source into the working
/// format.
///
/// The pass's input is also what the Presence context is computed from, here,
/// when the plan has Presence on: from the source after noise reduction, so
/// the context is recomputed with the chain rather than carried by a
/// checkpoint (ADR 041).
template <typename Chain>
ImageBuffer runPointwise(const ImageBuffer& source, const ProcessingPlan& plan,
                         const Chain& chain) {
    std::optional<PresenceContext> context;
    if (plan.presence.active()) {
        context = presenceContextOf(source, plan.presence);
    }
    const PresenceContext* around = context ? &*context : nullptr;
    // Opened before the result is made, whose zeroing at 24 MP takes as long
    // as a cheap chain does, so that the step named is the one being paid for.
    const detail::ProgressSpan progress(ProgressStep::Pointwise);
    ImageBuffer result(source.size(), workingFormat, workingEncoding);
    result.setPixelScale(source.pixelScale());
    switch (source.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        developSamples<std::uint8_t>(source, result, chain, around);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        developSamples<std::uint16_t>(source, result, chain, around);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        developSamples<float>(source, result, chain, around);
        break;
    }
    return result;
}

/// @brief The whole chain, as the traversal calls it.
auto developChain(const ProcessingPlan& plan) {
    return [&plan](Colour colour, const PixelContext& context) {
        return developPixel(plan, colour, context);
    };
}

/// @brief Runs the pointwise chain over a source, into the working format.
ImageBuffer developPointwise(const ImageBuffer& source, const ProcessingPlan& plan) {
    const detail::TimingSpan timing("cpu.pointwise");
    return runPointwise(source, plan, developChain(plan));
}

/// @brief Runs the Denoise pass that a plan resolved, or copies the source as it stands.
///
/// What a checkpoint at ::arraw::Stage::Denoise holds. With noise reduction off
/// the boundary collapses onto the source (ADR 039), and only a render that
/// stops there pays for the copy: it must own its pixels.
ImageBuffer denoisedCopy(const ImageBuffer& source, const ProcessingPlan& plan) {
    if (plan.denoise.active()) {
        return applyDenoise(source, plan.denoise);
    }
    return source.format() == PixelFormat::RgbaF32 ? source.clone() : toRgbaF32(source);
}

/// @brief Runs the stages from the source to the pointwise boundary: denoise, if on, then the
/// chain.
///
/// With noise reduction off the chain reads the source itself, so the pixels,
/// and the cost, are those of before the Denoise stage existed.
/// @param chain What one colour goes through: developPixel, or a tap's prefix.
template <typename Chain>
ImageBuffer pointwiseFromSource(const ImageBuffer& source, const ProcessingPlan& plan,
                                const Chain& chain) {
    if (!plan.denoise.active()) {
        return runPointwise(source, plan, chain);
    }
    return runPointwise(applyDenoise(source, plan.denoise), plan, chain);
}

/// @brief Runs noise reduction and the pointwise chain over a source, into the working format.
ImageBuffer developFromSource(const ImageBuffer& source, const ProcessingPlan& plan) {
    const detail::TimingSpan timing("cpu.pointwise");
    return pointwiseFromSource(source, plan, developChain(plan));
}

/// @brief Copies a rectangle of working-format pixels out of a frame.
ImageBuffer cutOut(const ImageBuffer& frame, const PixelRegion& region) {
    ImageBuffer cut(region.size(), workingFormat, frame.encoding(), frame.orientation());
    cut.setPixelScale(frame.pixelScale());
    const auto in = frame.samples<float>();
    const auto out = cut.samples<float>();
    const std::size_t rowSamples = static_cast<std::size_t>(region.width) * 4;
    for (std::uint32_t row = 0; row < region.height; ++row) {
        const std::size_t start =
            (static_cast<std::size_t>(region.y + row) * frame.size().width + region.x) * 4;
        std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(start), rowSamples,
                    out.begin() + static_cast<std::ptrdiff_t>(row * rowSamples));
    }
    return cut;
}

/// @brief Runs the resize that a plan resolved: cut the region, then resample it.
ImageBuffer resizeBy(ImageBuffer framed, const ProcessingPlan& plan) {
    const ResizePlan& resize = *plan.resize;
    if (!resize.region.covers(framed.size())) {
        framed = cutOut(framed, resize.region);
    }
    return resample(std::move(framed), resize.outputSize, resize.filter, resize.opaque);
}

/// @brief Runs the Effects pass that a plan resolved, or leaves the pixels as they are.
///
/// With every effect off the boundary collapses onto the resize (ADR 011): no
/// pass, and the buffer is handed on untouched.
ImageBuffer effectsBy(ImageBuffer resized, const ProcessingPlan& plan) {
    if (!plan.effects.active()) {
        return resized;
    }
    return applyEffects(std::move(resized), plan.effects, frameMappingOf(plan));
}

/// @brief Checks that a pass boundary is one that exists.
void requireBoundary(Stage stopAfter) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A render needs a recognised pass boundary to stop after");
    }
}

/// @brief Runs the stages after a boundary, up to another, on pixels taken at the first.
/// @param done Boundary @p pixels were taken at.
/// @param pixels Developed pixels at @p done; consumed.
/// @param plan The plan that made them and makes the rest.
/// @param stopAfter Last boundary to run, not before @p done.
RenderCheckpoint runStages(Stage done, ImageBuffer pixels, ProcessingPlan plan, Stage stopAfter) {
    Stage at = done;
    if (at == Stage::Denoise && stopAfter != Stage::Denoise) {
        pixels = developPointwise(pixels, plan);
        at = Stage::Pointwise;
    }
    if (at == Stage::Pointwise && stopAfter != Stage::Pointwise) {
        pixels = applyGeometry(std::move(pixels), *plan.geometry);
        at = Stage::Geometry;
    }
    if (at == Stage::Geometry && stopAfter >= Stage::Resize) {
        pixels = resizeBy(std::move(pixels), plan);
        at = Stage::Resize;
    }
    if (at == Stage::Resize && stopAfter == Stage::Effects) {
        pixels = effectsBy(std::move(pixels), plan);
        at = Stage::Effects;
    }
    return makeCheckpoint(at, std::move(plan), std::move(pixels));
}

} // namespace

ImageBuffer arraw::develop(const ImageBuffer& source, const DevelopState& state,
                           const RenderRequest& request, ProgressChannel* progress) {
    const detail::TimingSpan timing("cpu.develop");
    // The direct path: no checkpoint, so nothing is shared and nothing copied.
    const ProcessingPlan plan = planFor(source, state, request);
    detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                              ProgressStep::Denoise);
    ImageBuffer developed = effectsBy(
        resizeBy(applyGeometry(developFromSource(source, plan), *plan.geometry), plan), plan);
    root.finish(ProgressStep::Effects);
    return developed;
}

RenderCheckpoint arraw::developUntil(const ImageBuffer& source, const DevelopState& state,
                                     Stage stopAfter, const RenderRequest& request,
                                     ProgressChannel* progress) {
    requireBoundary(stopAfter);
    // Only a render that reaches the resize plans one: stopping earlier ignores
    // the request, and has no use for the opacity scan.
    ProcessingPlan plan = planFor(source, state, plannedRequest(request, stopAfter));
    // Measured against the whole render the request asks for, so that the
    // resumes after this one carry on from where it stops.
    detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                              ProgressStep::Denoise);
    if (stopAfter == Stage::Denoise) {
        ImageBuffer denoised = denoisedCopy(source, plan);
        root.finish(ProgressStep::Denoise);
        return makeCheckpoint(Stage::Denoise, std::move(plan), std::move(denoised));
    }
    ImageBuffer developed = developFromSource(source, plan);
    RenderCheckpoint done =
        runStages(Stage::Pointwise, std::move(developed), std::move(plan), stopAfter);
    root.finish(detail::stepThrough(stopAfter));
    return done;
}

RenderCheckpoint arraw::resumeFrom(const RenderCheckpoint& from, const ImageBuffer& source,
                                   const DevelopState& state, Stage stopAfter,
                                   const RenderRequest& request, ProgressChannel* progress) {
    requireBoundary(stopAfter);
    const CheckpointState& held = stateOf(from);
    const auto* pixels = std::get_if<ImageBuffer>(&held.pixels);
    if (pixels == nullptr) {
        throw std::invalid_argument("A checkpoint on a device cannot be resumed on the CPU");
    }
    ProcessingPlan plan = planFor(source, state, plannedRequest(request, stopAfter));
    requireResumable(held, plan, source.size(), stopAfter);
    // From the checkpoint's share of the whole render, which a refused one never reports.
    detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                              detail::stepAfter(held.boundary));
    const ProgressStep last = detail::stepThrough(stopAfter);
    if (stopAfter == held.boundary) {
        root.finish(last);
        return from;
    }
    RenderCheckpoint done =
        held.boundary == Stage::Denoise
            // The chain reads its input without consuming it, so the shared pixels
            // need no copy: a tone edit resumes for the price of the chain alone.
            ? runStages(Stage::Pointwise, developPointwise(*pixels, plan), std::move(plan),
                        stopAfter)
            : runStages(held.boundary, pixels->clone(), std::move(plan), stopAfter);
    root.finish(last);
    return done;
}

bool arraw::canResumeFrom(const RenderCheckpoint& from, const ImageBuffer& source,
                          const DevelopState& state, Stage stopAfter,
                          const RenderRequest& request) {
    const CheckpointState& held = stateOf(from);
    requireStopAfter(held.boundary, stopAfter);
    // Planned as both backends' resumes plan, so that the answer is theirs.
    const ProcessingPlan plan = planFor(source, state, plannedRequest(request, stopAfter));
    return !staleReason(held, plan, source.size()).has_value();
}

ImageBuffer arraw::sample(const ImageBuffer& source, const DevelopState& state, Tap tap,
                          const RenderRequest& request, ProgressChannel* progress) {
    // Validated first, so a bad tap costs nothing.
    static_cast<void>(tapEncoding(tap));
    const detail::TimingSpan timing("cpu.sample");
    const ProcessingPlan plan = planFor(source, state, request);
    // A sample stops before the effects, which therefore take no share.
    detail::StepWeights weights = detail::observedStepWeights(progress, plan, request);
    weights[static_cast<std::size_t>(ProgressStep::Effects)] = 0.0;
    detail::ProgressRoot root(progress, weights, ProgressStep::Denoise);
    ImageBuffer tapped =
        pointwiseFromSource(source, plan, [&plan, tap](Colour colour, const PixelContext& context) {
            return developToTap(plan, colour, tap, context);
        });
    // The same geometry and resize as a render, in linear light (ADR 020), so
    // the sample covers the frame the render shows; then the tap's encoding.
    const ImageBuffer framed = resizeBy(applyGeometry(std::move(tapped), *plan.geometry), plan);
    ImageBuffer encoded = encodeTap(framed, tap);
    root.finish(ProgressStep::Resize);
    return encoded;
}

ImageSize arraw::croppedSize(ImageSize sourceSize, ImageOrientation orientation,
                             const DevelopState& state) {
    return geometryPlanFor(sourceSize, orientation, state.settings.geometry).outputSize;
}

RenderRequest::Region arraw::renderedRegion(const RenderRequest& request, ImageSize frame) {
    const PixelRegion pixels = regionOf(request, frame);
    const auto width = static_cast<double>(frame.width);
    const auto height = static_cast<double>(frame.height);
    return {.left = pixels.x / width,
            .top = pixels.y / height,
            .right = (pixels.x + pixels.width) / width,
            .bottom = (pixels.y + pixels.height) / height};
}

ImageSize arraw::resolvedSize(const RenderRequest& request, ImageSize cropped) {
    if (cropped.empty()) {
        throw std::invalid_argument("Cannot resolve a size against an empty photograph");
    }
    if (!request.size.has_value()) {
        return cropped;
    }

    double scale = 1.0;
    std::uint32_t maxWidth = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maxHeight = maxWidth;
    if (const auto* box = std::get_if<RenderRequest::FitInside>(&*request.size)) {
        if (box->width == 0 || box->height == 0) {
            throw std::invalid_argument("A box to fit inside needs both sides above zero");
        }
        scale = std::min(static_cast<double>(box->width) / cropped.width,
                         static_cast<double>(box->height) / cropped.height);
        maxWidth = box->width;
        maxHeight = box->height;
    } else {
        scale = std::get<RenderRequest::Scale>(*request.size).factor;
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::invalid_argument("A scale factor must be finite and above zero");
        }
    }
    if (request.upscale == Upscale::Never) {
        scale = std::min(scale, 1.0);
    }

    const auto side = [scale](std::uint32_t length, std::uint32_t limit) {
        const double rounded = std::round(length * scale);
        if (rounded >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("The requested size does not fit an image side");
        }
        return std::min(std::max(static_cast<std::uint32_t>(rounded), std::uint32_t{1}), limit);
    };
    return {side(cropped.width, maxWidth), side(cropped.height, maxHeight)};
}
