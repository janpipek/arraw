#include "Develop.h"

#include "BrushCoverage.h"
#include "CheckpointState.h"
#include "Denoise.h"
#include "Effects.h"
#include "LadderAccess.h"
#include "Presence.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"
#include "RenderProgress.h"
#include "Resample.h"
#include "RowBands.h"
#include "SampleConversion.h"
#include "StageDriver.h"
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
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
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
/// called with the pixel's column and row, its colour and its ::arraw::PixelContext.
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
                    chain(x, y, {toUnit(in[0]), toUnit(in[1]), toUnit(in[2])}, around);
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
///
/// The brush masks' coverage is made after the context and before the pass, in the Coverage step
/// between them.
/// @param makeCoverage Callable `() -> const detail::PackedCoverage*`: makes the plan's coverage
/// (opening the Coverage step's span), or gives null when it has none.
/// @param makeChain Callable `(const detail::PackedCoverage*) -> Chain`.
template <typename MakeCoverage, typename MakeChain>
ImageBuffer runPointwise(const ImageBuffer& source, const PointwisePlan& plan,
                         const MakeCoverage& makeCoverage, const MakeChain& makeChain) {
    std::optional<PresenceContext> context;
    if (plan.presence.active()) {
        context = presenceContextOf(source, plan.presence);
    }
    const PresenceContext* around = context ? &*context : nullptr;
    const detail::PackedCoverage* const coverage = makeCoverage();
    const auto chain = makeChain(coverage);
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
///
/// The amounts every pixel has are worked out here, once for the render; a plan with masks
/// resolves them for each pixel instead (::arraw::amountsAt).
///
/// The coverage of the plan's brush masks, packed before the traversal, is read here for each
/// pixel; @p coverage is null when the plan has none.
auto developChain(const PointwisePlan& plan, const detail::PackedCoverage* coverage) {
    return [&plan, coverage, brushSlots = static_cast<std::uint32_t>(plan.local.brushCount()),
            amounts = globalAmountsOf(plan)](std::uint32_t x, std::uint32_t y, Colour colour,
                                             const PixelContext& context) {
        if (plan.local.empty()) {
            return developPixel(plan, amounts, colour, context);
        }
        if (coverage != nullptr) {
            const PixelCoverage codes = coverage->at(x, y, brushSlots);
            return developPixel(plan, amountsAt(plan, x, y, codes), colour, context);
        }
        return developPixel(plan, amountsAt(plan, x, y, PixelCoverage{}), colour, context);
    };
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

/// @brief Pixels on the host: the source or a checkpoint's, borrowed, or a buffer of its own.
///
/// What the CPU stages pass between them. A borrowed buffer is never copied
/// until a pass needs to consume it; ::arraw::HostPixels::take then clones it,
/// so a pass that only reads (Denoise, the pointwise chain) costs no copy and
/// the clone count of every path is what it was (ADR 024, 039).
class HostPixels {
public:
    /// @brief Borrows a buffer that outlives these pixels.
    static HostPixels borrowed(const ImageBuffer& buffer) noexcept {
        HostPixels pixels;
        pixels.borrowed_ = &buffer;
        return pixels;
    }

    /// @brief Takes ownership of a buffer.
    static HostPixels owned(ImageBuffer buffer) {
        HostPixels pixels;
        pixels.owned_.emplace(std::move(buffer));
        return pixels;
    }

    /// @brief Gives the pixels for reading.
    [[nodiscard]] const ImageBuffer& view() const noexcept {
        return owned_ ? *owned_ : *borrowed_;
    }

    /// @brief Gives the pixels as a buffer of their own.
    ///
    /// An owned buffer is handed over; a borrowed one is cloned if it is in the
    /// working layout, and converted to it otherwise (only the source is ever
    /// borrowed in another).
    [[nodiscard]] ImageBuffer take() && {
        if (owned_) {
            return std::move(*owned_);
        }
        return borrowed_->format() == PixelFormat::RgbaF32 ? borrowed_->clone()
                                                           : toRgbaF32(*borrowed_);
    }

private:
    /// Leaves the pixels empty; only the factories make one.
    HostPixels() = default;

    /// Buffer read in place, when the pixels are not their own.
    const ImageBuffer* borrowed_ = nullptr;
    /// Buffer of their own.
    std::optional<ImageBuffer> owned_;
};

/// @brief The CPU's passes, for ::arraw::runStages.
struct CpuStages {
    using Pixels = HostPixels;

    /// Tap the pointwise pass stops at, or empty for the whole chain.
    std::optional<Tap> tap;

    /// Packed brush coverage kept by the ladder this render goes through, or null for a direct
    /// render, which packs its own for the call (ADR 044).
    detail::CoverageResidency* residency = nullptr;

    /// @brief Runs the pass that ends at a stage's boundary.
    Pixels run(Stage stage, Pixels pixels, const ProcessingPlan& plan) const {
        switch (stage) {
        case Stage::Denoise:
            return Pixels::owned(applyDenoise(pixels.view(), plan.denoise));
        case Stage::Pointwise:
            return Pixels::owned(pointwise(pixels.view(), plan));
        case Stage::Geometry:
            return Pixels::owned(applyGeometry(std::move(pixels).take(), *plan.geometry));
        case Stage::Resize:
            return Pixels::owned(resizeBy(std::move(pixels).take(), plan));
        case Stage::Effects:
            return Pixels::owned(effectsBy(std::move(pixels).take(), plan));
        }
        throw std::logic_error("A render ran a pass that does not exist");
    }

    /// @brief Makes a checkpoint of the pixels at a boundary.
    RenderCheckpoint checkpoint(Stage stage, Pixels pixels, const ProcessingPlan& plan) const {
        return makeCheckpoint(stage, plan, std::move(pixels).take());
    }

    /// @brief Reads a checkpoint's pixels in place.
    Pixels borrow(const RenderCheckpoint& checkpoint) const {
        return Pixels::borrowed(std::get<ImageBuffer>(stateOf(checkpoint).pixels));
    }

private:
    /// @brief Runs the pointwise chain, or a tap's prefix of it.
    ImageBuffer pointwise(const ImageBuffer& input, const ProcessingPlan& plan) const {
        // The brushes' coverage is made between the Presence context and the pass, in a span of
        // its own: one unit for each brush that has work to do.
        std::optional<detail::PackedCoverage> packed;
        const auto makeCoverage = [&]() -> const detail::PackedCoverage* {
            if (plan.pointwise.local.brushCount() == 0) {
                return nullptr;
            }
            const std::vector<double> weights =
                detail::coverageUnitWeights(plan.pointwise.local, residency);
            // Only when some brush has work: a delta drag reports no Coverage step at all.
            std::optional<detail::ProgressSpan> progress;
            if (!weights.empty()) {
                progress.emplace(ProgressStep::Coverage, weights);
            }
            const detail::PackedCoverage* made = nullptr;
            if (residency != nullptr) {
                made = &residency->update(plan.pointwise.local, input.size());
            } else {
                packed = detail::packCoverage(plan.pointwise.local);
                made = &*packed;
            }
            if (made->size != input.size()) {
                throw std::logic_error("The brush coverage was packed for another size");
            }
            return made;
        };
        const auto brushSlots = static_cast<std::uint32_t>(plan.pointwise.local.brushCount());
        if (tap) {
            return runPointwise(
                input, plan.pointwise, makeCoverage,
                [&pointwise = plan.pointwise, brushSlots, amounts = globalAmountsOf(plan.pointwise),
                 tap = *tap](const detail::PackedCoverage* coverage) {
                    return [&pointwise, coverage, brushSlots, amounts,
                            tap](std::uint32_t x, std::uint32_t y, Colour colour,
                                 const PixelContext& context) {
                        if (pointwise.local.empty()) {
                            return developToTap(pointwise, amounts, colour, tap, context);
                        }
                        if (coverage != nullptr) {
                            const PixelCoverage codes = coverage->at(x, y, brushSlots);
                            return developToTap(pointwise, amountsAt(pointwise, x, y, codes),
                                                colour, tap, context);
                        }
                        return developToTap(pointwise, amountsAt(pointwise, x, y, PixelCoverage{}),
                                            colour, tap, context);
                    };
                });
        }
        const detail::TimingSpan timing("cpu.pointwise");
        return runPointwise(input, plan.pointwise, makeCoverage,
                            [&plan](const detail::PackedCoverage* coverage) {
                                return developChain(plan.pointwise, coverage);
                            });
    }
};

static_assert(StageBackend<CpuStages>);

/// @brief Tells whether a rung's pixels are on the host.
bool onHost(const CheckpointState& rung) {
    return std::holds_alternative<ImageBuffer>(rung.pixels);
}

} // namespace

ImageBuffer arraw::develop(const ImageBuffer& source, const DevelopState& state,
                           const RenderRequest& request, ProgressChannel* progress) {
    const detail::TimingSpan timing("cpu.develop");
    // The direct path: no checkpoint, so nothing is shared and nothing copied.
    const ProcessingPlan plan = planFor(source, state, request);
    detail::ProgressRoot root(progress, detail::observedStepWeights(progress, plan, request),
                              ProgressStep::Denoise);
    CpuStages backend;
    StagesRun<CpuStages> run =
        runStages(backend, std::nullopt, HostPixels::borrowed(source), plan, Stage::Effects);
    root.finish(ProgressStep::Effects);
    return std::move(run.pixels).take();
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
    CpuStages backend;
    StagesRun<CpuStages> run =
        runStages(backend, std::nullopt, HostPixels::borrowed(source), plan, stopAfter);
    root.finish(detail::stepThrough(stopAfter));
    // A collapsed Denoise hands the source over, and the checkpoint must own its pixels.
    return makeCheckpoint(run.done, std::move(plan), std::move(run.pixels).take());
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
    // The shared pixels are read in place until a pass has to consume them, so
    // a tone edit resumes from the Denoise checkpoint for the price of the chain alone.
    CpuStages backend;
    StagesRun<CpuStages> run =
        runStages(backend, held.boundary, HostPixels::borrowed(*pixels), plan, stopAfter);
    root.finish(last);
    return makeCheckpoint(run.done, std::move(plan), std::move(run.pixels).take());
}

LadderRender arraw::resumeOrDevelop(CheckpointLadder& ladder,
                                    std::shared_ptr<const ImageBuffer> source,
                                    const DevelopState& state, const RenderRequest& request,
                                    ProgressChannel* progress) {
    if (!source) {
        throw std::invalid_argument("A render through a ladder needs a source");
    }
    // Planned before the ladder is touched, so that a bad request drops nothing.
    ProcessingPlan plan = planFor(*source, state, request);
    LadderAccess::bind(ladder, source);
    const std::optional<Stage> resumedFrom =
        LadderAccess::deepestUsable(ladder, plan, source->size(), onHost);
    // A plan without brushes lets the ladder's packed planes go (up to 366 MiB at 24 MP).
    const bool brushes = plan.pointwise.local.brushCount() != 0;
    if (!brushes) {
        LadderAccess::dropCoverage(ladder);
    }
    detail::ProgressRoot root(
        progress,
        detail::observedStepWeights(progress, plan, request,
                                    LadderAccess::coverage(std::as_const(ladder))),
        resumedFrom ? detail::stepAfter(*resumedFrom) : ProgressStep::Denoise);
    CpuStages backend;
    if (brushes) {
        backend.residency = &LadderAccess::coverage(ladder);
    }
    HostPixels start = resumedFrom ? backend.borrow(LadderAccess::rung(ladder, *resumedFrom))
                                   : HostPixels::borrowed(*source);
    StagesRun<CpuStages> run =
        runStages(backend, resumedFrom, std::move(start), plan, Stage::Effects, &ladder);
    root.finish(ProgressStep::Effects);
    return {makeCheckpoint(Stage::Effects, std::move(plan), std::move(run.pixels).take()),
            resumedFrom};
}

bool arraw::drawsBrushCoverage(const CheckpointLadder& ladder, const ImageBuffer& source,
                               const DevelopState& state, const RenderRequest& request,
                               double minimumSeconds) {
    const ProcessingPlan plan = planFor(source, state, request);
    if (LadderAccess::hasUsableFrom(ladder, Stage::Pointwise, plan, source.size(), onHost)) {
        // The render resumes past the pointwise pass and draws nothing.
        return false;
    }
    const detail::CoverageResidency* residency = LadderAccess::coverage(ladder);
    bool fromNothing = false;
    double nanoseconds = 0.0;
    for (const LocalMaskPlan& mask : plan.pointwise.local.masks) {
        if (mask.kind != LocalMaskKind::Brush) {
            continue;
        }
        const detail::CoverageStatus status = detail::statusOf(mask.brush, residency);
        if (status.readiness == detail::CoverageReadiness::Missing) {
            fromNothing = true;
            nanoseconds += detail::coverageWorkOf(status, mask.brush).draw;
        }
    }
    return fromNothing && nanoseconds * 1e-9 >= minimumSeconds;
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
    // The same geometry and resize as a render, in linear light (ADR 020), so
    // the sample covers the frame the render shows; then the tap's encoding.
    CpuStages backend{.tap = tap};
    const StagesRun<CpuStages> run =
        runStages(backend, std::nullopt, HostPixels::borrowed(source), plan, Stage::Resize);
    ImageBuffer encoded = encodeTap(run.pixels.view(), tap);
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
