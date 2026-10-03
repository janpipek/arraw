#pragma once

#include "AppSettings.h"

#include <DevelopState.h>
#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

#include <QImage>
#include <QRect>
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

/// @brief What a preview shows: a part of the developed frame, at a size.
struct PreviewView {
    /// Part of the developed frame to render, in whole pixels of the frame at
    /// full resolution (what ::arraw::croppedSize gives), or empty for all of it.
    std::optional<QRect> region;
    /// Box to fit the region (or the frame) inside, in device pixels. The
    /// photograph is never enlarged to it, so a result can be smaller.
    QSize outputSize;
    /// Device pixels per logical pixel of the screen; set on the image.
    qreal devicePixelRatio = 1.0;

    /// @brief Makes a view of the whole frame.
    /// @param box Box to fit the frame inside, in device pixels.
    /// @param ratio Device pixels per logical pixel.
    [[nodiscard]] static PreviewView wholeFrame(QSize box, qreal ratio = 1.0) {
        return {.region = std::nullopt, .outputSize = box, .devicePixelRatio = ratio};
    }
};

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
    /// Part of the frame the image shows, in normalised coordinates of the
    /// frame: what was actually rendered, which the engine snaps outward to
    /// whole pixels of the level it developed (see ::arraw::renderedRegion).
    /// Empty if the render failed.
    QRectF region;
    /// Size of the full-resolution developed frame that @ref region is in.
    QSize frame;
    /// Pyramid level the image was developed from: 0 is the full-resolution
    /// photograph, and each level above halves both sides (ADR 020).
    int level = 0;
    /// Boundary of the checkpoint the render resumed from, or empty when it
    /// developed from the level itself. A diagnostic: nothing shows it.
    std::optional<Stage> resumedFrom;
};

/// @brief Worker thread that renders previews off the thread that asks for them.
///
/// One long-lived thread, not a pool: a GPU context belongs to the thread that
/// made it, and the GPU path renders here. Only the newest request is
/// kept, so a burst of edits costs one render of the latest state, not one per
/// edit. Uses no Qt signals, so it works without an event loop.
///
/// Renders the part of the frame a request names (ADR 025), so that a zoomed
/// view costs the size of the view, not of the photograph.
///
/// Develops from a reduced copy of the photograph when the output is much
/// smaller than what it shows: a pyramid of 2x box reductions, built lazily on the worker
/// from whatever setSource was given, and the smallest level that still covers
/// the output is used (ADR 020). The result says which level it was.
///
/// Keeps the last pointwise and geometry results of the level it renders, as
/// checkpoints (resident on the GPU path, in host memory on the CPU one), so
/// that a geometry edit resumes after the pointwise pass and a viewport change
/// after the geometry. The engine decides whether a checkpoint still applies
/// (ADR 011); the renderer only drops them when the source, the level or the
/// device changes, which the plan cannot tell it.
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
    /// @param settings Desktop GPU preference, captured for the lifetime of the worker.
    explicit PreviewRenderer(std::function<void(PreviewResult)> onResult,
                             Device device = Device::Auto, AppSettings settings = {});

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
    /// @param view Part of the frame to show, and the size to show it at.
    /// @return Identifier of the request, increasing with each call.
    std::uint64_t request(DevelopState state, PreviewView view);

private:
    /// Request waiting for the worker.
    struct Pending {
        std::uint64_t id;
        DevelopState state;
        PreviewView view;
    };

    /// @brief Serves requests until asked to stop.
    /// @param stop Raised by the destructor.
    void run(std::stop_token stop);

    /// Receiver of each finished render, called on the worker thread.
    std::function<void(PreviewResult)> onResult_;

    /// Where previews may be rendered; fixed before the worker starts.
    Device device_;

    /// Desktop preferences captured before the worker starts.
    AppSettings settings_;

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
