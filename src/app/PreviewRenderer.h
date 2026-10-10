#pragma once

#include "AppSettings.h"

#include <CurveHistogram.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <QImage>
#include <QRect>
#include <QSize>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace arraw::app {

/// @brief Render the CPU counts a curve histogram from: the frame fitted inside 512 pixels.
///
/// A quarter of the pixels of ::arraw::curveHistogramRequest, which the GPU
/// keeps: 256 bins need far fewer than a million pixels, and the host sample
/// costs about 150-200 ns a source pixel (ADR 035), so the larger request would
/// hold up the next render by up to half a second (ADR 036). Bilinear, as
/// ::arraw::curveHistogram uses whatever the request says.
inline constexpr RenderRequest cpuCurveHistogramRequest{
    .size = RenderRequest::FitInside{512, 512},
    .filter = ResizeFilter::Bilinear,
};

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
    /// Long edge of a reduced copy to deliver beside an image of the whole frame, for a
    /// thumbnail; 0 for none. Reduced on the worker, so the receiver does not (ADR 043).
    int thumbnailEdge = 0;

    /// @brief Makes a view of the whole frame.
    /// @param box Box to fit the frame inside, in device pixels.
    /// @param ratio Device pixels per logical pixel.
    [[nodiscard]] static PreviewView wholeFrame(QSize box, qreal ratio = 1.0) {
        return {.region = std::nullopt,
                .outputSize = box,
                .devicePixelRatio = ratio,
                .thumbnailEdge = 0};
    }
};

/// @brief Least time between two progress reports of one preview render, unless its step changes.
///
/// About thirty a second: as often as a bar can be seen to move.
inline constexpr std::chrono::milliseconds previewProgressInterval{33};

/// @brief Least modelled time to draw brush coverage from nothing that earns a coarser stand-in.
///
/// Below it the stand-in, a full render of a quarter of the pixels that reuses no saved stage,
/// would cost about what it hides (report B7's time model; ADR 044, section 8).
inline constexpr double defaultStandInSeconds = 0.3;

/// @brief Outcome of one preview render.
///
/// A result with only @ref background set is a refreshed fallback, rendered
/// for its request's state after the render of that request was delivered. A
/// result with only @ref curveHistogram set is a recounted curve histogram,
/// likewise.
struct PreviewResult {
    /// Identifier PreviewRenderer::request returned for the render.
    std::uint64_t request = 0;
    /// Rendered image, set when the render succeeded.
    std::optional<QImage> image;
    /// Reduced whole-frame image beneath the detailed region; possibly of an
    /// earlier state with the same geometry, until a refreshed one follows.
    std::optional<QImage> background;
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
    /// Whether it is a coarser stand-in: the render of the same request at its own level
    /// follows (the coarser first render, ADR 044, section 8). Such a result is not a
    /// thumbnail's source, not the fallback beneath a region, and does not end the render.
    bool provisional = false;
    /// Boundary of the checkpoint the render resumed from, or empty when it
    /// developed from the level itself. A diagnostic: nothing shows it.
    std::optional<Stage> resumedFrom;
    /// Histogram of the curve input for the request's state (ADR 035), set only
    /// on a result of its own, once requests paused and the curve input changed.
    std::optional<CurveHistogram> curveHistogram;
    /// The image reduced to PreviewView::thumbnailEdge, at a pixel ratio of 1; set only
    /// when the request asked for one and the image shows the whole frame.
    std::optional<QImage> thumbnail;
};

/// @brief Worker thread that renders previews off the thread that asks for them.
///
/// One long-lived thread, not a pool: a GPU context belongs to the thread that
/// made it, and the GPU path renders here. Only the newest request is
/// kept, so a burst of edits costs one render of the latest state, not one per
/// edit. Uses no Qt signals, so it works without an event loop.
///
/// Renders the part of the frame a request names (ADR 025), so that a zoomed
/// view costs the size of the view, not of the photograph. Beneath such a
/// region it keeps a reduced whole frame, to fill what a pan uncovers: the
/// last whole-frame render, refreshed once requests pause after an edit.
///
/// Develops from a reduced copy of the photograph when the output is much
/// smaller than what it shows: a pyramid of 2x box reductions, built lazily on the worker
/// from whatever setSource was given, and the smallest level that still covers
/// the output is used (ADR 020). The result says which level it was.
///
/// Renders through checkpoint ladders (::arraw::CheckpointLadder; resident on
/// the GPU path, in host memory on the CPU one), so that a geometry edit
/// resumes after the pointwise pass and a viewport change after the geometry.
/// The engine keeps the rungs and decides whether each still applies (ADR 011,
/// 045), and binds a ladder to the level it renders; the renderer only clears
/// them when the source or the device changes, which the plan cannot tell it.
///
/// A newer request cancels the render in flight (ADR 042): on the CPU it stops
/// within a few milliseconds, on the GPU after the render pass it is in. A
/// cancelled render delivers nothing, neither an image nor an error, and keeps
/// the checkpoints of the passes it finished, so the newer request resumes
/// from them. The newest request is never cancelled by another, so it always
/// ends with a result. The refreshed fallback and the histogram recount below
/// are cancelled the same way, so neither holds up the next request.
///
/// A render at full size whose brush masks would take ::arraw::app::defaultStandInSeconds or more
/// to rasterise from nothing delivers a coarser
/// stand-in first (PreviewResult::provisional): the same request developed from the pyramid's
/// level 1, a quarter of the pixels, as a direct render. The window shows it while level 0
/// renders, which delivers as usual. A request whose coverage is held, or extends a held list
/// (an undo, a delta drag, an appended stroke), delivers one result.
///
/// Reports the progress of the render of each request, at most about
/// ::arraw::app::previewProgressInterval apart and whenever the step changes,
/// so that whoever shows it is not flooded.
///
/// Once requests pause, also counts the histogram of the curve input that the
/// curve editor draws behind its curves, on the GPU when it renders, and only
/// while someone shows it (setCurveHistogramWanted()) and the curve input may
/// have changed since the last count (ADR 036): a curve drag never costs one.
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

    /// @brief Receiver of the progress of the render of a request.
    ///
    /// Called on the worker thread with the identifier PreviewRenderer::request
    /// returned and the render's progress; the caller marshals it to wherever it
    /// is needed. Must not throw; what it throws stops the render, as a failure.
    using ProgressCallback = std::function<void(std::uint64_t request, const Progress& progress)>;

    /// @brief Starts the worker thread.
    /// @param onResult Receives each finished render, called on the worker
    /// thread; the caller marshals it to wherever it is needed. Must not throw;
    /// what it throws is dropped.
    /// @param device Where previews may be rendered.
    /// @param settings Desktop GPU preference, captured for the lifetime of the worker.
    /// @param onProgress Receives the progress of the render of each request,
    /// thinned (see ::arraw::app::previewProgressInterval); may be empty.
    /// Neither the fallback's nor the histogram's renders report.
    explicit PreviewRenderer(std::function<void(PreviewResult)> onResult,
                             Device device = Device::Auto, AppSettings settings = {},
                             ProgressCallback onProgress = {});

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;
    PreviewRenderer(PreviewRenderer&&) = delete;
    PreviewRenderer& operator=(PreviewRenderer&&) = delete;

    /// @brief Stops the worker and waits for it.
    ///
    /// Cancels a render in progress and waits for it to stop, and drops pending
    /// requests. The callbacks are not called once this returns.
    ~PreviewRenderer();

    /// @brief Replaces the photograph to render, dropping requests not yet started.
    ///
    /// Cancels the work in progress (ADR 042, ADR 043), which then delivers
    /// nothing; a render past its last check finishes with the previous source,
    /// which the worker keeps alive, and its result is still delivered.
    /// The worker then drops the levels and checkpoints it kept of the previous source.
    /// @param decoded Decoded photograph; may be empty to clear the source.
    void setSource(std::shared_ptr<const ImageBuffer> decoded);

    /// @brief Queues a render, replacing any request not yet started and cancelling the one in
    /// flight.
    ///
    /// A request made while no source is set is not ignored: it yields a result
    /// with an error.
    /// @param state How to develop the photograph.
    /// @param view Part of the frame to show, and the size to show it at.
    /// @return Identifier of the request, increasing with each call.
    std::uint64_t request(DevelopState state, PreviewView view);

    /// @brief Says whether the curve histogram is shown, so whether it is worth counting.
    ///
    /// Nothing is counted while it is not wanted; none is wanted until this
    /// says so. Becoming wanted counts once, at the next pause, for the state
    /// last rendered, if the last histogram counted is not current for it.
    /// @param wanted Whether the curve editor is on screen.
    void setCurveHistogramWanted(bool wanted);

    /// @brief Sets the least modelled time to draw brush coverage that earns a coarser stand-in.
    ///
    /// For requests made after the call. The default is the cost at which the stand-in, a
    /// full render of a quarter of the pixels, starts to pay for itself; tests set 0.
    /// @param seconds Modelled seconds of drawing, per report B7.
    void setStandInThreshold(double seconds);

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

    /// @brief Makes the channel of the work about to start, which a newer request cancels.
    ///
    /// Already cancelled when a request or another source arrived since the work was taken.
    /// @param onProgress Receiver of the work's progress; may be empty.
    /// @param source Source the work was taken with.
    [[nodiscard]] std::shared_ptr<ProgressChannel>
    startChannel(ProgressChannel::Callback onProgress,
                 const std::shared_ptr<const ImageBuffer>& source);

    /// @brief Forgets the channel of the work that ended.
    void endChannel();

    /// Receiver of each finished render, called on the worker thread.
    std::function<void(PreviewResult)> onResult_;

    /// Receiver of the progress of each request's render, called on the worker thread.
    ProgressCallback onProgress_;

    /// Where previews may be rendered; fixed before the worker starts.
    Device device_;

    /// Desktop preferences captured before the worker starts.
    AppSettings settings_;

    /// Least modelled drawing time of brush coverage that earns a stand-in; see
    /// setStandInThreshold.
    std::atomic<double> standInSeconds_{defaultStandInSeconds};

    /// Guard of source_, pending_, sourceChanged_, lastId_, curveHistogramWanted_,
    /// recountHistogram_ and inFlight_.
    std::mutex mutex_;

    /// Signal that a request or a recount is pending, or the worker should stop.
    std::condition_variable_any wake_;

    /// Photograph to render; null until one is set.
    std::shared_ptr<const ImageBuffer> source_;

    /// Newest request not yet started.
    std::optional<Pending> pending_;

    /// Whether source_ changed since the worker last looked, so that it drops what it keeps
    /// of the previous one.
    bool sourceChanged_ = false;

    /// Channel of the work the worker is doing, which a newer request cancels; null when idle.
    std::shared_ptr<ProgressChannel> inFlight_;

    /// Identifier of the newest request.
    std::uint64_t lastId_ = 0;

    /// Whether the curve histogram is shown, so counted.
    bool curveHistogramWanted_ = false;

    /// Whether the histogram became wanted since the worker last looked, so
    /// that it counts for the state last rendered without a new request.
    bool recountHistogram_ = false;

    /// Declared last so that every member above exists before it starts.
    std::jthread worker_;
};

} // namespace arraw::app
