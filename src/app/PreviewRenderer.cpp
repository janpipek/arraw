#include "PreviewRenderer.h"

#include "DisplayImage.h"
#include "GpuContext.h"
#include "GpuDevelop.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace arraw::app {

namespace {

/// @brief What the worker holds on the GPU: the device, and the photograph on it.
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

    /// @brief Releases the uploaded photograph, then the device, in that order.
    ~GpuPreview() {
        uploaded_ = {};
        context_.reset();
    }

    /// @brief Makes the GPU ready to render a source, if it can be.
    ///
    /// Creates the device on the first call, and uploads the source when it is
    /// not the one already on the device, dropping the previous one first. A
    /// failure is remembered: for the device it is final, for a source it lasts
    /// until the source changes.
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
            uploaded_ = {};
            source_ = source;
            sourceReason_.clear();
            try {
                uploaded_ = uploadSource(*context_, *source);
            } catch (const std::exception& error) {
                sourceReason_ = error.what();
                dropIfLost();
            }
        }
        return uploaded_.valid();
    }

    /// @brief Renders a request on the device and reads the viewport-sized result back.
    /// @pre prepare returned true for @p source.
    [[nodiscard]] ImageBuffer render(const ImageBuffer& source, const DevelopState& state,
                                     const RenderRequest& request) {
        const RenderCheckpoint checkpoint =
            developOnGpu(*context_, source, uploaded_, state, Stage::Resize, request);
        return checkpoint.readBack();
    }

    /// @brief Releases the photograph from the device, as there is none to show.
    void forget() {
        uploaded_ = {};
        source_.reset();
        sourceReason_.clear();
    }

    /// @brief Gives up the GPU for the photograph being shown, and says why.
    void fail(const std::string& reason) {
        sourceReason_ = reason;
        uploaded_ = {};
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
        try {
            auto context = std::make_unique<GpuContext>(defaultGpuBackend());
            if (context->info().kind == GpuDeviceKind::Software) {
                // As the command line's auto mode: a rasteriser on the CPU is
                // no faster than the CPU path, and is slower to start.
                reason_ = "Software rasteriser refused: " + context->info().deviceName;
                return;
            }
            context_ = std::move(context);
        } catch (const std::exception& error) {
            reason_ = error.what();
        }
    }

    /// @brief Drops the device once it has failed for good; the CPU takes over.
    void dropIfLost() {
        if (context_ && context_->lost()) {
            reason_ = "GPU device lost";
            uploaded_ = {};
            source_.reset();
            context_.reset();
        }
    }

    // Declaration order is destruction order reversed: the device is made first
    // and goes last (the destructor says so too).
    std::unique_ptr<GpuContext> context_;
    DeviceImage uploaded_;
    /// Photograph on the device, or whose upload failed; kept alive so that its
    /// address identifies it.
    std::shared_ptr<const ImageBuffer> source_;
    /// Why the GPU does not serve source_.
    std::string sourceReason_;
    /// Why there is no device; empty while there is one.
    std::string reason_;
    /// Whether a device has been tried for.
    bool tried_ = false;
};

/// @brief Renders one request, turning a failure into a result.
PreviewResult render(std::uint64_t id, const DevelopState& state, QSize viewport,
                     qreal devicePixelRatio, const std::shared_ptr<const ImageBuffer>& source,
                     GpuPreview* gpu) {
    PreviewResult result{.request = id,
                         .image = std::nullopt,
                         .error = {},
                         .onGpu = false,
                         .deviceName = {},
                         .fallbackReason = {}};
    // Nothing may escape the thread, or the process terminates.
    try {
        if (!source) {
            result.error = "No photograph to render";
            if (gpu != nullptr) {
                gpu->forget();
            }
            return result;
        }
        const RenderRequest request = previewRequest(viewport);
        std::string gpuFailure;
        if (gpu != nullptr && gpu->prepare(source)) {
            try {
                QImage image = toDisplayImage(gpu->render(*source, state, request));
                image.setDevicePixelRatio(devicePixelRatio);
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
        QImage image = toDisplayImage(develop(*source, state, request));
        image.setDevicePixelRatio(devicePixelRatio);
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

std::uint64_t PreviewRenderer::request(DevelopState state, QSize viewport, qreal devicePixelRatio) {
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock(mutex_);
        id = ++lastId_;
        pending_.emplace(Pending{id, std::move(state), viewport, devicePixelRatio});
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
        PreviewResult result = render(job->id, job->state, job->viewport, job->devicePixelRatio,
                                      source, gpu ? &*gpu : nullptr);
        try {
            onResult_(std::move(result));
        } catch (...) {
            // The callback's failure is not ours to handle, and must not end the thread.
        }
    }
}

} // namespace arraw::app
