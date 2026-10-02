#pragma once

#include <DevelopState.h>
#include <ImageBuffer.h>

#include <QImage>
#include <QSize>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace arraw::app {

/// @brief Outcome of one preview render.
struct PreviewResult {
    /// Identifier PreviewRenderer::request returned for the render.
    std::uint64_t request = 0;
    /// Rendered image, set when the render succeeded.
    std::optional<QImage> image;
    /// Description of the failure, set when the render failed.
    std::string error;
    /// Whether the GPU rendered the image, rather than the CPU.
    bool onGpu = false;
    /// Name of the GPU that rendered the image; empty when the CPU did.
    std::string deviceName;
    /// Why the CPU rendered when the GPU was wanted, or empty when it was not
    /// (the GPU rendered, or the CPU was asked for).
    std::string fallbackReason;
    /// Pyramid level the image was developed from: 0 is the full-resolution
    /// photograph, and each level above halves both sides (ADR 020).
    int level = 0;
};

/// @brief Worker thread that renders previews off the thread that asks for them.
///
/// One long-lived thread, not a pool: a GPU context belongs to the thread that
/// made it, and the GPU path renders here. Only the newest request is
/// kept, so a burst of edits costs one render of the latest state, not one per
/// edit. Uses no Qt signals, so it works without an event loop.
///
/// Develops from a reduced copy of the photograph when the viewport is much
/// smaller than it: a pyramid of 2x box reductions, built lazily on the worker
/// from whatever setSource was given, and the smallest level that still covers
/// the viewport is used (ADR 020). The result says which level it was.
///
/// Renders on the GPU when there is one: the first render creates the device,
/// on the worker, and the decoded photograph is uploaded once per source rather
/// than once per render. Whatever the GPU cannot do, the CPU does, and the
/// result says which it was (see PreviewResult::device).
class PreviewRenderer {
public:
    /// @brief Where previews may be rendered.
    enum class Device {
        Auto, ///< On the GPU when one can be used, else on the CPU.
        Cpu,  ///< Always on the CPU, without ever creating a GPU device.
    };

    /// @brief Starts the worker thread.
    /// @param onResult Receives each finished render, called on the worker
    /// thread; the caller marshals it to wherever it is needed. Must not throw;
    /// what it throws is dropped.
    /// @param device Where previews may be rendered.
    explicit PreviewRenderer(std::function<void(PreviewResult)> onResult,
                             Device device = Device::Auto);

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;
    PreviewRenderer(PreviewRenderer&&) = delete;
    PreviewRenderer& operator=(PreviewRenderer&&) = delete;

    /// @brief Stops the worker and waits for it.
    ///
    /// Waits for a render in progress, as developing cannot be interrupted, and
    /// drops pending requests. The callback is not called once this returns.
    ~PreviewRenderer();

    /// @brief Replaces the photograph to render, dropping requests not yet started.
    ///
    /// A render in progress finishes with the previous source, which the worker
    /// keeps alive, and its result is still delivered.
    /// @param decoded Decoded photograph; may be empty to clear the source.
    void setSource(std::shared_ptr<const ImageBuffer> decoded);

    /// @brief Queues a render, replacing any request not yet started.
    ///
    /// A request made while no source is set is not ignored: it yields a result
    /// with an error.
    /// @param state How to develop the photograph.
    /// @param viewport Size to fit inside, in device pixels.
    /// @param devicePixelRatio Device pixels per logical pixel of the screen.
    /// @return Identifier of the request, increasing with each call.
    std::uint64_t request(DevelopState state, QSize viewport, qreal devicePixelRatio);

private:
    /// Request waiting for the worker.
    struct Pending {
        std::uint64_t id;
        DevelopState state;
        QSize viewport;
        qreal devicePixelRatio;
    };

    /// @brief Serves requests until asked to stop.
    /// @param stop Raised by the destructor.
    void run(std::stop_token stop);

    /// Receiver of each finished render, called on the worker thread.
    std::function<void(PreviewResult)> onResult_;

    /// Where previews may be rendered; fixed before the worker starts.
    Device device_;

    /// Guard of source_, pending_ and lastId_.
    std::mutex mutex_;

    /// Signal that a request is pending or the worker should stop.
    std::condition_variable_any wake_;

    /// Photograph to render; null until one is set.
    std::shared_ptr<const ImageBuffer> source_;

    /// Newest request not yet started.
    std::optional<Pending> pending_;

    /// Identifier of the newest request.
    std::uint64_t lastId_ = 0;

    /// Declared last so that every member above exists before it starts.
    std::jthread worker_;
};

} // namespace arraw::app
