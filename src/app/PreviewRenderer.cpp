#include "PreviewRenderer.h"

#include "DisplayImage.h"

#include <exception>
#include <utility>

namespace arraw::app {

PreviewRenderer::PreviewRenderer(std::function<void(PreviewResult)> onResult)
    : onResult_(std::move(onResult)), worker_([this](const std::stop_token& stop) { run(stop); }) {}

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

PreviewResult PreviewRenderer::render(const Pending& pending,
                                      const std::shared_ptr<const ImageBuffer>& source) {
    PreviewResult result{.request = pending.id, .image = std::nullopt, .error = {}};
    // Nothing may escape the thread, or the process terminates.
    try {
        if (!source) {
            result.error = "No photograph to render";
        } else {
            result.image = renderForViewport(*source, pending.state, pending.viewport,
                                             pending.devicePixelRatio);
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "Unknown error while rendering";
    }
    return result;
}

void PreviewRenderer::run(std::stop_token stop) {
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
        PreviewResult result = render(*job, source);
        try {
            onResult_(std::move(result));
        } catch (...) {
            // The callback's failure is not ours to handle, and must not end the thread.
        }
    }
}

} // namespace arraw::app
