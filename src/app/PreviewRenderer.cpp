#include "PreviewRenderer.h"

#include "DisplayImage.h"
#include "GpuContext.h"
#include "GpuDevelop.h"

#include <Develop.h>
#include <ImagePyramid.h>
#include <RenderCheckpoint.h>

#include <algorithm>
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

/// @brief The last pointwise and geometry results of one level of one source.
///
/// Whether a checkpoint still applies to a request is the engine's to say; what
/// the plan cannot tell is that the pixels underneath changed to others of the
/// same size and encoding, so the cache is bound to the level's buffer and
/// dropped when that is another one. Used on the worker only, which is also
/// where resident checkpoints must be released.
class CheckpointCache {
public:
    /// @brief Binds the cache to a level, dropping the checkpoints of any other.
    /// @param level Buffer the next renders develop from; kept alive, so that its
    /// address identifies it.
    void bind(const std::shared_ptr<const ImageBuffer>& level) {
        if (level != level_) {
            clear();
            level_ = level;
        }
    }

    /// @brief Drops both checkpoints and the binding.
    void clear() noexcept {
        pointwise_.reset();
        geometry_.reset();
        level_.reset();
    }

    /// @brief Renders a request from the best checkpoint there is, refreshing the cache.
    ///
    /// Tries the geometry checkpoint, then the pointwise one, then the level
    /// itself, and carries on to the resize through each boundary it passes so
    /// the next request can reuse them. A checkpoint the engine refuses is
    /// dropped: it is stale, and the render goes on from an earlier one. A
    /// render that fails leaves only checkpoints that are whole.
    /// @tparam Develop Callable `(Stage) -> RenderCheckpoint`, developing the level.
    /// @tparam Resume Callable `(const RenderCheckpoint&, Stage) -> RenderCheckpoint`.
    /// @param resumedFrom Set to the boundary resumed from, or reset.
    /// @return The checkpoint at the resize.
    template <typename Develop, typename Resume>
    [[nodiscard]] RenderCheckpoint render(Develop&& develop, Resume&& resume,
                                          std::optional<Stage>& resumedFrom) {
        resumedFrom.reset();
        if (geometry_) {
            if (auto done = tryResume(resume, *geometry_, Stage::Resize)) {
                resumedFrom = Stage::Geometry;
                return std::move(*done);
            }
            geometry_.reset();
        }
        // The pointwise checkpoint is checked after the geometry one, which
        // was made from it: one that does not match now is useless to keep.
        if (pointwise_) {
            if (auto done = tryResume(resume, *pointwise_, Stage::Geometry)) {
                resumedFrom = Stage::Pointwise;
                geometry_ = std::move(*done);
                return resume(*geometry_, Stage::Resize);
            }
            pointwise_.reset();
        }
        pointwise_ = develop(Stage::Pointwise);
        geometry_ = resume(*pointwise_, Stage::Geometry);
        return resume(*geometry_, Stage::Resize);
    }

private:
    /// @brief Resumes, or says the checkpoint does not apply.
    ///
    /// The engine refuses a stale checkpoint with an `std::invalid_argument`,
    /// which is also what a bad request raises; the latter is raised again by
    /// the render that follows, so nothing is hidden.
    template <typename Resume>
    static std::optional<RenderCheckpoint> tryResume(Resume& resume, const RenderCheckpoint& from,
                                                     Stage stopAfter) {
        try {
            return resume(from, stopAfter);
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
    }

    /// Level the checkpoints were made from.
    std::shared_ptr<const ImageBuffer> level_;
    /// Result after the pointwise chain, at the level's size.
    std::optional<RenderCheckpoint> pointwise_;
    /// Result after the geometry, before any resize.
    std::optional<RenderCheckpoint> geometry_;
};

/// @brief What the worker holds on the GPU: the device, and the pyramid levels on it.
///
/// Lives as a local of the worker's loop, so that everything holding the device
/// is destroyed on the thread that owns it, before that thread ends.
class GpuPreview {
public:
    GpuPreview() = default;
    GpuPreview(const GpuPreview&) = delete;
    GpuPreview& operator=(const GpuPreview&) = delete;
    GpuPreview(GpuPreview&&) = delete;
    GpuPreview& operator=(GpuPreview&&) = delete;

    /// @brief Releases the uploaded levels, then the device, in that order.
    ~GpuPreview() {
        checkpoints_.clear();
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
            checkpoints_.clear();
            uploaded_.clear();
            source_ = source;
            sourceReason_.clear();
        }
        return sourceReason_.empty();
    }

    /// @brief Renders a request on the device and reads the viewport-sized result back.
    ///
    /// Uploads the level the first time it is rendered from, and keeps it for
    /// the current source.
    /// @pre prepare returned true.
    /// @param level Pyramid level of @p image.
    /// @param image The level to develop.
    /// @param resumedFrom Set to the boundary resumed from, or reset.
    [[nodiscard]] ImageBuffer render(int level, const std::shared_ptr<const ImageBuffer>& image,
                                     const DevelopState& state, const RenderRequest& request,
                                     std::optional<Stage>& resumedFrom) {
        const auto index = static_cast<std::size_t>(level);
        if (uploaded_.size() <= index) {
            uploaded_.resize(index + 1);
        }
        if (!uploaded_[index].valid()) {
            uploaded_[index] = uploadSource(*context_, *image);
        }
        checkpoints_.bind(image);
        const RenderCheckpoint checkpoint = checkpoints_.render(
            [&](Stage stop) {
                return developOnGpu(*context_, *image, uploaded_[index], state, stop, request);
            },
            [&](const RenderCheckpoint& from, Stage stop) {
                return developOnGpu(*context_, from, *image, state, stop, request);
            },
            resumedFrom);
        return checkpoint.readBack();
    }

    /// @brief Releases the photograph from the device, as there is none to show.
    void forget() {
        checkpoints_.clear();
        uploaded_.clear();
        source_.reset();
        sourceReason_.clear();
    }

    /// @brief Gives up the GPU for the photograph being shown, and says why.
    void fail(const std::string& reason) {
        sourceReason_ = reason;
        checkpoints_.clear();
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
    /// @brief Creates the device, or records why not.
    void create() {
        context_ = createHardwareContext(reason_);
    }

    /// @brief Drops the device once it has failed for good; the CPU takes over.
    void dropIfLost() {
        if (context_ && context_->lost()) {
            reason_ = "GPU device lost";
            checkpoints_.clear();
            uploaded_.clear();
            source_.reset();
            context_.reset();
        }
    }

    // Declaration order is destruction order reversed: the device is made first
    // and goes last (the destructor says so too).
    std::unique_ptr<GpuContext> context_;
    /// Pyramid levels on the device, by level; a level not yet needed is empty.
    std::vector<DeviceImage> uploaded_;
    /// Last pointwise and geometry results of the level shown, resident here.
    CheckpointCache checkpoints_;
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
PreviewResult render(std::uint64_t id, const DevelopState& state, const PreviewView& view,
                     const std::shared_ptr<const ImageBuffer>& source, SourcePyramid& pyramid,
                     CheckpointCache& cpuCache, GpuPreview* gpu) {
    PreviewResult result{.request = id,
                         .image = std::nullopt,
                         .error = {},
                         .onGpu = false,
                         .deviceName = {},
                         .fallbackReason = {},
                         .region = {},
                         .frame = {},
                         .level = 0,
                         .resumedFrom = std::nullopt};
    // Nothing may escape the thread, or the process terminates.
    try {
        pyramid.reset(source);
        if (!source) {
            cpuCache.clear();
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
        const std::shared_ptr<const ImageBuffer>& reduced = pyramid.level(level);
        result.level = level;
        // Snapped to the level's own pixels, which are coarser than the
        // frame's: where the image goes on screen is what this says.
        const RenderRequest::Region rendered =
            renderedRegion(request, croppedSize(reduced->size(), reduced->orientation(), state));
        result.region =
            QRectF(QPointF(rendered.left, rendered.top), QPointF(rendered.right, rendered.bottom));
        std::string gpuFailure;
        if (gpu != nullptr && gpu->prepare(source)) {
            try {
                QImage image =
                    toDisplayImage(gpu->render(level, reduced, state, request, result.resumedFrom));
                image.setDevicePixelRatio(view.devicePixelRatio);
                result.image = std::move(image);
                result.onGpu = true;
                result.deviceName = gpu->deviceName();
                return result;
            } catch (const std::exception& error) {
                gpuFailure = error.what();
            } catch (...) {
                gpuFailure = "Unknown error while rendering on the GPU";
            }
        }
        // The CPU renders: no GPU was wanted, or it could not. It runs before
        // the GPU is given up for this photograph, since if it fails too the
        // request was at fault and the GPU is not.
        cpuCache.bind(reduced);
        const RenderCheckpoint developed = cpuCache.render(
            [&](Stage stop) { return developUntil(*reduced, state, stop, request); },
            [&](const RenderCheckpoint& from, Stage stop) {
                return resumeFrom(from, *reduced, state, stop, request);
            },
            result.resumedFrom);
        QImage image = toDisplayImage(developed.readBack());
        image.setDevicePixelRatio(view.devicePixelRatio);
        result.image = std::move(image);
        if (gpu != nullptr) {
            if (!gpuFailure.empty()) {
                gpu->fail(gpuFailure);
            }
            result.fallbackReason = gpu->reason();
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "Unknown error while rendering";
    }
    return result;
}

} // namespace

PreviewRenderer::PreviewRenderer(std::function<void(PreviewResult)> onResult, Device device)
    : onResult_(std::move(onResult)), device_(device),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

PreviewRenderer::~PreviewRenderer() {
    // Explicit rather than left to ~jthread, so the order is visible: stop,
    // wake the worker, wait for it.
    worker_.request_stop();
    worker_.join();
}

void PreviewRenderer::setSource(std::shared_ptr<const ImageBuffer> decoded) {
    const std::scoped_lock lock(mutex_);
    source_ = std::move(decoded);
    pending_.reset();
}

std::uint64_t PreviewRenderer::request(DevelopState state, PreviewView view) {
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock(mutex_);
        id = ++lastId_;
        pending_.emplace(Pending{id, std::move(state), view});
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
        gpu.emplace();
    }
    // Host memory only: the GPU's copies of the levels are its own.
    SourcePyramid pyramid;
    // Host checkpoints of the CPU path, for the level it last rendered.
    CheckpointCache cpuCache;
    while (true) {
        std::optional<Pending> job;
        std::shared_ptr<const ImageBuffer> source;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] { return pending_.has_value(); });
            if (stop.stop_requested()) {
                return; // Pending requests are dropped.
            }
            job = std::move(pending_);
            pending_.reset();
            source = source_;
        }
        // Without the lock: developing takes long, and the window must be able
        // to queue the next request meanwhile.
        PreviewResult result = render(job->id, job->state, job->view, source, pyramid, cpuCache,
                                      gpu ? &*gpu : nullptr);
        try {
            onResult_(std::move(result));
        } catch (...) {
            // The callback's failure is not ours to handle, and must not end the thread.
        }
    }
}

} // namespace arraw::app
