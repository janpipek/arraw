#include "PreviewRenderer.h"

#include "CurveHistogramRefresh.h"
#include "DisplayImage.h"
#include "GpuContext.h"
#include "GpuDevelop.h"
#include "TimingTrace.h"

#include <CheckpointLadder.h>
#include <CurveHistogram.h>
#include <Develop.h>
#include <ImagePyramid.h>
#include <NoiseReductionSettings.h>
#include <RenderCheckpoint.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace arraw::app {

namespace {

/// @brief Idle time after a region render before the fallback beneath it is refreshed.
///
/// Long enough that the gaps of a slider drag do not start one, which would
/// hold up the next edit; short enough to follow soon after the edit stops.
constexpr std::chrono::milliseconds backgroundDelay{150};

/// @brief Size the whole-frame fallback is rendered within.
constexpr QSize backgroundSize{1024, 1024};

/// @brief Which of a request's renders is made; each keeps its own checkpoints.
enum class Layer {
    Shown,      ///< The view asked for.
    Background, ///< The reduced whole frame beneath a region.
};

/// @brief What the worker holds on the GPU: the device, and the pyramid levels on it.
///
/// Lives as a local of the worker's loop, so that everything holding the device
/// is destroyed on the thread that owns it, before that thread ends.
class GpuPreview {
public:
    explicit GpuPreview(AppSettings settings) : settings_(std::move(settings)) {}
    GpuPreview(const GpuPreview&) = delete;
    GpuPreview& operator=(const GpuPreview&) = delete;
    GpuPreview(GpuPreview&&) = delete;
    GpuPreview& operator=(GpuPreview&&) = delete;

    /// @brief Releases the uploaded levels, then the device, in that order.
    ~GpuPreview() {
        clearCheckpoints();
        uploaded_.clear();
        context_.reset();
    }

    /// @brief Makes the GPU ready to render a source, if it can be.
    ///
    /// Creates the device on the first call, and drops the levels of the
    /// previous source when the source changes. Nothing is uploaded here: a
    /// level goes up when a render first needs it. A failure is remembered: for
    /// the device it is final, for a source it lasts until the source changes.
    /// @param source Photograph about to be rendered.
    /// @return Whether the GPU can render @p source.
    [[nodiscard]] bool prepare(const std::shared_ptr<const ImageBuffer>& source) {
        if (!tried_) {
            tried_ = true;
            create();
        }
        if (!context_) {
            return false;
        }
        if (source != source_) {
            clearCheckpoints();
            uploaded_.clear();
            source_ = source;
            sourceReason_.clear();
        }
        return sourceReason_.empty();
    }

    /// @brief Drops the levels and checkpoints of the source, keeping the device.
    void forgetSource() noexcept {
        clearCheckpoints();
        uploaded_.clear();
        source_.reset();
        sourceReason_.clear();
    }

    /// @brief Renders a request on the device and reads the viewport-sized result back.
    ///
    /// Uploads the level the first time it is rendered from, and keeps it for
    /// the current source.
    /// @pre prepare returned true.
    /// @param layer Render it is, which picks the checkpoints resumed from.
    /// @param level Pyramid level of @p image.
    /// @param image The level to develop.
    /// @param resumedFrom Set to the boundary resumed from, or reset.
    /// @param progress Channel of the render.
    /// @throws ::arraw::Cancelled if @p progress is cancelled, leaving the
    /// checkpoints of the passes that finished.
    [[nodiscard]] ImageBuffer render(Layer layer, int level,
                                     const std::shared_ptr<const ImageBuffer>& image,
                                     const DevelopState& state, const RenderRequest& request,
                                     std::optional<Stage>& resumedFrom, ProgressChannel* progress) {
        resumedFrom.reset();
        const DeviceImage& uploaded = uploadedLevel(level, *image);
        CheckpointLadder& ladder = layer == Layer::Background ? backgroundLadder_ : ladder_;
        LadderRender rendered =
            resumeOrDevelopOnGpu(*context_, ladder, image, uploaded, state, request, progress);
        resumedFrom = rendered.resumedFrom;
        const RenderCheckpoint& checkpoint = rendered.checkpoint;
        return checkpoint.readBack();
    }

    /// @brief Counts the curve histogram of a level on the device.
    ///
    /// Samples the curve input at ::arraw::curveHistogramRequest, which is bounded
    /// and Bilinear (ADR 035), reads it back and counts it on the host. Uploads
    /// the level if no render has yet.
    /// @pre prepare returned true.
    /// @param level Pyramid level of @p image.
    /// @param image The level to sample.
    /// @param progress Channel of the count.
    [[nodiscard]] CurveHistogram curveHistogramOf(int level,
                                                  const std::shared_ptr<const ImageBuffer>& image,
                                                  const DevelopState& state,
                                                  ProgressChannel* progress) {
        return curveHistogram(sampleOnGpu(*context_, *image, uploadedLevel(level, *image), state,
                                          Tap::CurveInput, curveHistogramRequest, progress));
    }

    /// @brief Releases the photograph from the device, as there is none to show.
    void forget() {
        clearCheckpoints();
        uploaded_.clear();
        source_.reset();
        sourceReason_.clear();
    }

    /// @brief Gives up the GPU for the photograph being shown, and says why.
    void fail(const std::string& reason) {
        sourceReason_ = reason;
        clearCheckpoints();
        uploaded_.clear();
        dropIfLost();
    }

    /// @brief Gives the name of the device that renders.
    /// @pre prepare returned true.
    [[nodiscard]] const std::string& deviceName() const {
        return context_->info().deviceName;
    }

    /// @brief Gives the reason the GPU is not rendering, or the empty string.
    [[nodiscard]] const std::string& reason() const {
        return context_ ? sourceReason_ : reason_;
    }

private:
    /// @brief Gives a level on the device, uploading it the first time it is asked for.
    const DeviceImage& uploadedLevel(int level, const ImageBuffer& image) {
        const auto index = static_cast<std::size_t>(level);
        if (uploaded_.size() <= index) {
            uploaded_.resize(index + 1);
        }
        if (!uploaded_[index].valid()) {
            uploaded_[index] = uploadSource(*context_, image);
        }
        return uploaded_[index];
    }

    /// @brief Creates the device, or records why not.
    void create() {
        context_ = createAppGpuContext(settings_, reason_);
    }

    /// @brief Drops the checkpoints of both layers.
    void clearCheckpoints() noexcept {
        ladder_.clear();
        backgroundLadder_.clear();
    }

    /// @brief Drops the device once it has failed for good; the CPU takes over.
    void dropIfLost() {
        if (context_ && context_->lost()) {
            reason_ = "GPU device lost";
            clearCheckpoints();
            uploaded_.clear();
            source_.reset();
            context_.reset();
        }
    }

    // Declaration order is destruction order reversed: the device is made first
    // and goes last (the destructor says so too).
    /// Desktop GPU preference.
    AppSettings settings_;
    std::unique_ptr<GpuContext> context_;
    /// Pyramid levels on the device, by level; a level not yet needed is empty.
    std::vector<DeviceImage> uploaded_;
    /// Last denoise, pointwise, geometry and resize results of the level shown, resident here.
    CheckpointLadder ladder_;
    /// The same for the background, whose coarser level would evict the shown one's.
    CheckpointLadder backgroundLadder_;
    /// Photograph the levels belong to, or whose upload failed; kept alive so
    /// that its address identifies it.
    std::shared_ptr<const ImageBuffer> source_;
    /// Why the GPU does not serve source_.
    std::string sourceReason_;
    /// Why there is no device; empty while there is one.
    std::string reason_;
    /// Whether a device has been tried for.
    bool tried_ = false;
};

/// @brief Shortest long edge a pyramid level may have, in pixels.
///
/// Below it a level is of no use to a preview, and the levels above would
/// only cost memory.
constexpr std::uint32_t smallestLevelEdge = 256;

/// @brief Reductions of one source, each half the one before, built when asked for.
///
/// Level 0 is the source itself, whatever it is: the pyramid hangs off what
/// the renderer was given, so that another kind of source (a provisional
/// picture, say) is a different thing to reset to and nothing more. Used on the
/// worker only.
class SourcePyramid {
public:
    /// @brief Starts a pyramid on a source, dropping the levels of the previous one.
    ///
    /// Does nothing when @p source is the one already held.
    void reset(const std::shared_ptr<const ImageBuffer>& source) {
        if (source == source_) {
            return;
        }
        source_ = source;
        levels_.clear();
        if (source_) {
            levels_.push_back(source_);
        }
    }

    /// @brief Gives the highest level that may exist for a source of a size.
    ///
    /// The last level whose long edge is still at least ::smallestLevelEdge,
    /// and 0 for a source that is smaller already.
    [[nodiscard]] static int highestLevel(ImageSize size) {
        int level = 0;
        while (true) {
            const ImageSize next{(size.width + 1) / 2, (size.height + 1) / 2};
            if (next == size || std::max(next.width, next.height) < smallestLevelEdge) {
                return level;
            }
            size = next;
            ++level;
        }
    }

    /// @brief Gives a level, building it and those below it if need be.
    /// @pre reset was given a source, and @p level is at most highestLevel of its size.
    [[nodiscard]] const std::shared_ptr<const ImageBuffer>& level(int level) {
        while (levels_.size() <= static_cast<std::size_t>(level)) {
            levels_.push_back(std::make_shared<const ImageBuffer>(halved(*levels_.back())));
        }
        return levels_[static_cast<std::size_t>(level)];
    }

private:
    /// Source the levels come from; keeps it alive and identifies it.
    std::shared_ptr<const ImageBuffer> source_;
    /// Levels built so far; the first is the source.
    std::vector<std::shared_ptr<const ImageBuffer>> levels_;
};

/// @brief Renders one request, turning a failure into a result.
///
/// The checkpoints are only ever replaced by a pass that returned, so a render
/// cancelled part-way leaves those it finished and no other (ADR 042).
/// @param progress Channel of the render, which a newer request cancels.
/// @return The result, or nothing when the render was cancelled: neither an
/// image nor a failure.
std::optional<PreviewResult> render(std::uint64_t id, const DevelopState& state,
                                    const PreviewView& view,
                                    const std::shared_ptr<const ImageBuffer>& source,
                                    SourcePyramid& pyramid, CheckpointLadder& cpuLadder,
                                    GpuPreview* gpu, Layer layer, ProgressChannel* progress) {
    const detail::TimingSpan timing("preview.render", id);
    PreviewResult result{.request = id,
                         .image = std::nullopt,
                         .background = std::nullopt,
                         .error = {},
                         .onGpu = false,
                         .deviceName = {},
                         .fallbackReason = {},
                         .region = {},
                         .frame = {},
                         .level = 0,
                         .resumedFrom = std::nullopt,
                         .curveHistogram = std::nullopt,
                         .thumbnail = std::nullopt};
    // Nothing may escape the thread, or the process terminates.
    try {
        pyramid.reset(source);
        if (!source) {
            cpuLadder.clear();
            result.error = "No photograph to render";
            if (gpu != nullptr) {
                gpu->forget();
            }
            return result;
        }
        RenderRequest request = previewRequest(view.outputSize);
        const ImageSize cropped = croppedSize(source->size(), source->orientation(), state);
        const QSize frame(static_cast<int>(cropped.width), static_cast<int>(cropped.height));
        QRect shown(QPoint(0, 0), frame);
        if (view.region) {
            shown = *view.region;
            if (shown.isEmpty() || !QRect(QPoint(0, 0), frame).contains(shown)) {
                throw std::invalid_argument("The region is not inside the developed frame");
            }
            request.region = RenderRequest::Region{
                .left = static_cast<double>(shown.left()) / frame.width(),
                .top = static_cast<double>(shown.top()) / frame.height(),
                .right = static_cast<double>(shown.right() + 1) / frame.width(),
                .bottom = static_cast<double>(shown.bottom() + 1) / frame.height()};
        }
        result.frame = frame;
        const int level =
            std::min(pyramidLevelFor(source->size(), source->orientation(), state, request),
                     SourcePyramid::highestLevel(source->size()));
        const std::shared_ptr<const ImageBuffer> reduced = [&] {
            const detail::TimingSpan pyramidTiming("preview.pyramid");
            return pyramid.level(level);
        }();
        result.level = level;
        if (detail::timingLog().isDebugEnabled()) {
            timing.note("level=" + std::to_string(level) +
                        " source=" + std::to_string(reduced->size().width) + "x" +
                        std::to_string(reduced->size().height) +
                        " viewport=" + std::to_string(view.outputSize.width()) + "x" +
                        std::to_string(view.outputSize.height()));
        }
        // Snapped to the level's own pixels, which are coarser than the
        // frame's: where the image goes on screen is what this says.
        const RenderRequest::Region rendered =
            renderedRegion(request, croppedSize(reduced->size(), reduced->orientation(), state));
        result.region =
            QRectF(QPointF(rendered.left, rendered.top), QPointF(rendered.right, rendered.bottom));
        std::string gpuFailure;
        if (gpu != nullptr && gpu->prepare(source)) {
            try {
                QImage image = toDisplayImage(gpu->render(layer, level, reduced, state, request,
                                                          result.resumedFrom, progress));
                image.setDevicePixelRatio(view.devicePixelRatio);
                result.image = std::move(image);
                result.onGpu = true;
                result.deviceName = gpu->deviceName();
                timing.note(result.deviceName);
                return result;
            } catch (const Cancelled&) {
                // Not a failure of the GPU: nothing falls back.
                throw;
            } catch (const std::exception& error) {
                gpuFailure = error.what();
            } catch (...) {
                gpuFailure = "Unknown error while rendering on the GPU";
            }
        }
        // The CPU renders: no GPU was wanted, or it could not. It runs before
        // the GPU is given up for this photograph, since if it fails too the
        // request was at fault and the GPU is not.
        LadderRender developed = resumeOrDevelop(cpuLadder, reduced, state, request, progress);
        result.resumedFrom = developed.resumedFrom;
        QImage image = toDisplayImage(developed.checkpoint.readBack());
        image.setDevicePixelRatio(view.devicePixelRatio);
        result.image = std::move(image);
        timing.note("CPU");
        if (gpu != nullptr) {
            if (!gpuFailure.empty()) {
                gpu->fail(gpuFailure);
            }
            result.fallbackReason = gpu->reason();
        }
    } catch (const Cancelled&) {
        timing.note("cancelled");
        return std::nullopt;
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "Unknown error while rendering";
    }
    return result;
}

/// @brief Counts the curve histogram of a state, unless the last one counted is still current.
///
/// Samples the pyramid level that covers the histogram's own request, not the
/// level of the view, so that zooming neither changes nor recounts it. On the
/// GPU when it renders this source, with ::arraw::curveHistogramRequest; on the
/// CPU when it does not or fails, with the smaller ::cpuCurveHistogramRequest.
/// @param progress Channel of the count, which a newer request cancels.
/// @return The histogram, or nothing when the last one is current, it failed or
/// it was cancelled; a failure is not retried before the next render, and a
/// cancelled count is owed again at the next pause.
std::optional<CurveHistogram> countCurveHistogram(const DevelopState& state,
                                                  const std::shared_ptr<const ImageBuffer>& source,
                                                  SourcePyramid& pyramid, GpuPreview* gpu,
                                                  CurveHistogramRefresh& refresh,
                                                  ProgressChannel* progress) {
    const detail::TimingSpan timing("preview.curveHistogram");
    const auto levelFor = [&](const RenderRequest& request) {
        return std::min(pyramidLevelFor(source->size(), source->orientation(), state, request),
                        SourcePyramid::highestLevel(source->size()));
    };
    try {
        const bool onGpu = gpu != nullptr && gpu->prepare(source);
        const RenderRequest& request = onGpu ? curveHistogramRequest : cpuCurveHistogramRequest;
        int level = levelFor(request);
        std::shared_ptr<const ImageBuffer> reduced = pyramid.level(level);
        ProcessingPlan plan = planFor(*reduced, state, request);
        if (refresh.isCurrent(reduced, plan)) {
            timing.note("current");
            return std::nullopt;
        }
        std::optional<CurveHistogram> counted;
        if (onGpu) {
            try {
                counted = gpu->curveHistogramOf(level, reduced, state, progress);
                timing.note(gpu->deviceName());
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception&) {
                // The CPU counts instead, from its own level; the next render
                // reports a GPU that is gone.
                level = levelFor(cpuCurveHistogramRequest);
                reduced = pyramid.level(level);
                plan = planFor(*reduced, state, cpuCurveHistogramRequest);
            }
        }
        if (!counted) {
            counted = curveHistogram(*reduced, state, cpuCurveHistogramRequest, progress);
            timing.note("CPU");
        }
        refresh.record(reduced, std::move(plan));
        return counted;
    } catch (const Cancelled&) {
        timing.note("cancelled");
        return std::nullopt;
    } catch (const std::exception&) {
        // The render of the same state reports what is wrong with it.
        return std::nullopt;
    }
}

/// @brief Gives a receiver of one request's progress that hands it on at a bounded rate.
///
/// Each report whose step differs from the last one handed on, the end, and
/// otherwise one at least ::previewProgressInterval after the last. Called on
/// the worker only.
/// @param onProgress Where reports go; must outlive the receiver.
/// @param id The request whose render it is.
ProgressChannel::Callback thinned(const PreviewRenderer::ProgressCallback& onProgress,
                                  std::uint64_t id) {
    if (!onProgress) {
        return {};
    }
    return [&onProgress, id, step = std::optional<ProgressStep>(),
            at = std::chrono::steady_clock::time_point()](const Progress& progress) mutable {
        const auto now = std::chrono::steady_clock::now();
        // The end always goes through, so that a bar is not left short of it.
        if (step == progress.step && now - at < previewProgressInterval &&
            progress.fraction < 1.0) {
            return;
        }
        step = progress.step;
        at = now;
        onProgress(id, progress);
    };
}

/// @brief Reduces a render of the whole frame for a thumbnail.
/// @param result A render that delivered an image.
/// @param edge Long edge of the thumbnail; 0 for none.
/// @return The reduced image at a pixel ratio of 1, or nothing when none was asked for or the
/// image shows a part of the frame.
std::optional<QImage> reducedForThumbnail(const PreviewResult& result, int edge) {
    constexpr double tolerance = 1e-3;
    const QRectF& region = result.region;
    if (edge <= 0 || !result.image || region.left() > tolerance || region.top() > tolerance ||
        region.right() < 1.0 - tolerance || region.bottom() < 1.0 - tolerance) {
        return std::nullopt;
    }
    const detail::TimingSpan timing("preview.thumbnail", result.request);
    QImage thumbnail = *result.image;
    if (thumbnail.width() > edge || thumbnail.height() > edge) {
        thumbnail = thumbnail.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // The preview's pixel ratio is for the photograph view, not for a cell.
    thumbnail.setDevicePixelRatio(1.0);
    return thumbnail;
}

} // namespace

PreviewRenderer::PreviewRenderer(std::function<void(PreviewResult)> onResult, Device device,
                                 AppSettings settings, ProgressCallback onProgress)
    : onResult_(std::move(onResult)), onProgress_(std::move(onProgress)),
      device_(settings.cpuOnly ? Device::Cpu : device), settings_(std::move(settings)),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

PreviewRenderer::~PreviewRenderer() {
    // Explicit rather than left to ~jthread, so the order is visible: stop,
    // cancel what is in flight, wake the worker, wait for it.
    worker_.request_stop();
    {
        const std::scoped_lock lock(mutex_);
        if (inFlight_) {
            inFlight_->cancel();
        }
    }
    worker_.join();
}

std::shared_ptr<ProgressChannel>
PreviewRenderer::startChannel(ProgressChannel::Callback onProgress,
                              const std::shared_ptr<const ImageBuffer>& source) {
    auto channel = std::make_shared<ProgressChannel>(std::move(onProgress));
    const std::scoped_lock lock(mutex_);
    // A request or a new source that came while the work was being taken has
    // superseded it already. A stop requested before this lock found nothing in
    // flight to cancel, so the new work stops here instead of holding up the
    // destructor.
    if (pending_ || source_ != source || worker_.get_stop_token().stop_requested()) {
        channel->cancel();
    }
    inFlight_ = channel;
    return channel;
}

void PreviewRenderer::endChannel() {
    const std::scoped_lock lock(mutex_);
    inFlight_.reset();
}

void PreviewRenderer::setSource(std::shared_ptr<const ImageBuffer> decoded) {
    {
        const std::scoped_lock lock(mutex_);
        sourceChanged_ = sourceChanged_ || decoded != source_;
        source_ = std::move(decoded);
        pending_.reset();
        // Whatever the worker does is for the previous photograph.
        if (inFlight_) {
            inFlight_->cancel();
        }
    }
    // The worker lets go of the previous photograph's levels and checkpoints now, not at
    // the next render, which waits for the next decode (ADR 043).
    wake_.notify_one();
}

void PreviewRenderer::setCurveHistogramWanted(bool wanted) {
    bool wake = false;
    {
        const std::scoped_lock lock(mutex_);
        wake = wanted && !curveHistogramWanted_;
        curveHistogramWanted_ = wanted;
        recountHistogram_ = recountHistogram_ || wake;
    }
    if (wake) {
        wake_.notify_one();
    }
}

std::uint64_t PreviewRenderer::request(DevelopState state, PreviewView view) {
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock(mutex_);
        id = ++lastId_;
        pending_.emplace(Pending{id, std::move(state), view});
        // Superseded: whatever the worker does now, this request makes pointless.
        if (inFlight_) {
            inFlight_->cancel();
        }
    }
    wake_.notify_one();
    return id;
}

void PreviewRenderer::run(std::stop_token stop) {
    // The GPU context and everything on it belong to this thread: created on
    // the first render, and destroyed when this function returns, before the
    // thread ends. Only built when the GPU is allowed at all.
    std::optional<GpuPreview> gpu;
    if (device_ == Device::Auto) {
        gpu.emplace(settings_);
    }
    // Host memory only: the GPU's copies of the levels are its own.
    SourcePyramid pyramid;
    // Host rungs of the CPU path, for the level it last rendered.
    CheckpointLadder cpuLadder;
    // Whole-frame fallback beneath region renders, with its own CPU ladder so it
    // never evicts the detailed rungs (GpuPreview keeps a second one too).
    // Kept across edits that leave the geometry alone, as it still lines up;
    // backgroundState is what it was rendered for.
    CheckpointLadder backgroundLadder;
    std::shared_ptr<const ImageBuffer> backgroundSource;
    std::optional<DevelopState> backgroundState;
    std::optional<QImage> background;
    // What the last curve histogram was counted from.
    CurveHistogramRefresh histogramRefresh;
    const auto deliver = [this](PreviewResult result) {
        try {
            onResult_(std::move(result));
        } catch (...) {
            // The callback's failure is not ours to handle, and must not end the thread.
        }
    };
    // The last request rendered and its source, which a recount without a
    // new request is counted for.
    std::optional<Pending> lastShown;
    std::shared_ptr<const ImageBuffer> lastShownSource;
    while (true) {
        std::optional<Pending> job;
        std::shared_ptr<const ImageBuffer> source;
        bool recountOnly = false;
        bool forget = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] {
                return pending_.has_value() || recountHistogram_ || sourceChanged_;
            });
            if (stop.stop_requested()) {
                return; // Pending requests are dropped.
            }
            source = source_;
            forget = std::exchange(sourceChanged_, false);
            if (pending_) {
                job = std::move(pending_);
                pending_.reset();
            } else if (recountHistogram_) {
                // Only a recount: of the state last rendered, if it was of this source.
                recountHistogram_ = false;
                if (!forget && lastShown && lastShownSource == source) {
                    job = lastShown;
                    recountOnly = true;
                }
            }
        }
        if (forget) {
            // Everything kept for the previous source goes, so that its memory is free
            // while the next photograph decodes.
            pyramid.reset(nullptr);
            cpuLadder.clear();
            backgroundLadder.clear();
            backgroundSource.reset();
            backgroundState.reset();
            background.reset();
            histogramRefresh.clear();
            lastShown.reset();
            lastShownSource.reset();
            if (gpu) {
                gpu->forgetSource();
            }
        }
        if (!job) {
            continue;
        }
        if (!recountOnly) {
            if (source != backgroundSource ||
                (backgroundState &&
                 backgroundState->settings.geometry != job->state.settings.geometry)) {
                background.reset();
                backgroundState.reset();
                if (source != backgroundSource) {
                    // Also lets go of the previous source's level.
                    histogramRefresh.clear();
                }
                backgroundSource = source;
            }
            bool shown = false;
            {
                const detail::TimingSpan timing("preview", job->id);
                // Without the lock: developing takes long, and the window must be
                // able to queue the next request meanwhile.
                const std::shared_ptr<ProgressChannel> channel =
                    startChannel(thinned(onProgress_, job->id), source);
                std::optional<PreviewResult> rendered =
                    render(job->id, job->state, job->view, source, pyramid, cpuLadder,
                           gpu ? &*gpu : nullptr, Layer::Shown, channel.get());
                endChannel();
                if (!rendered) {
                    // Cancelled by a newer request, which is pending: nothing to deliver.
                    continue;
                }
                PreviewResult& result = *rendered;
                if (result.image && result.region == QRectF(0.0, 0.0, 1.0, 1.0)) {
                    background = result.image;
                    backgroundState = job->state;
                }
                if (result.image) {
                    shown = true;
                    result.background = background;
                    result.thumbnail = reducedForThumbnail(result, job->view.thumbnailEdge);
                }
                deliver(std::move(result));
            }
            // The fallback and the curve histogram are refreshed only once the
            // requests pause, after the render that was asked for, so that they
            // never delay one.
            if (!shown) {
                continue;
            }
            lastShown = job;
            lastShownSource = source;
        }
        const auto paused = [&] {
            std::unique_lock lock(mutex_);
            return !wake_.wait_for(lock, stop, backgroundDelay,
                                   [this] { return pending_.has_value(); }) &&
                   source_ == source && !stop.stop_requested();
        };
        if (!paused()) {
            if (stop.stop_requested()) {
                return;
            }
            continue;
        }
        if (job->view.region && backgroundState != job->state) {
            const detail::TimingSpan timing("preview.background", job->id);
            const std::shared_ptr<ProgressChannel> channel = startChannel({}, source);
            std::optional<PreviewResult> reduced = render(
                job->id, job->state, PreviewView::wholeFrame(backgroundSize), source, pyramid,
                backgroundLadder, gpu ? &*gpu : nullptr, Layer::Background, channel.get());
            endChannel();
            if (!reduced) {
                // Cancelled by a newer request: refreshed after that one instead.
                continue;
            }
            // A failure is not retried for the same state; the region render reports it.
            backgroundState = job->state;
            background = std::move(reduced->image);
            if (background) {
                PreviewResult update;
                update.request = job->id;
                update.background = background;
                deliver(std::move(update));
            }
        }
        {
            const std::scoped_lock lock(mutex_);
            // Closing must not wait for a count as well as for the render.
            if (stop.stop_requested() || pending_ || source_ != source) {
                if (stop.stop_requested()) {
                    return;
                }
                continue;
            }
            // Counted now, or not wanted: either way no recount is owed.
            recountHistogram_ = false;
            if (!curveHistogramWanted_) {
                continue;
            }
        }
        const std::shared_ptr<ProgressChannel> channel = startChannel({}, source);
        std::optional<CurveHistogram> histogram = countCurveHistogram(
            job->state, source, pyramid, gpu ? &*gpu : nullptr, histogramRefresh, channel.get());
        endChannel();
        if (histogram) {
            PreviewResult update;
            update.request = job->id;
            update.curveHistogram = std::move(histogram);
            deliver(std::move(update));
        }
    }
}

} // namespace arraw::app
