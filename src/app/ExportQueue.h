#pragma once

#include "AppSettings.h"
#include "LensSupport.h"

#include <DecodeOptions.h>
#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageExport.h>
#include <NegativeSettings.h>
#include <Photo.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace arraw::app {

/// @brief One export: a snapshot of the photograph and what to do with it.
///
/// Owns everything it needs, so that the photograph may be edited, or
/// replaced, while the job waits and runs.
struct ExportJob {
    /// Whether the export thread opens input itself, with ::arraw::openPhoto, and takes state,
    /// decode, document and the metadata's marks from it, the sidecar's saved settings. The
    /// fields they replace are then ignored. For a photograph that is not open in the window:
    /// opening reads the file's header and sidecar, which a batch of many would not do on the
    /// interface thread. What the open reports is part of the result's warnings.
    bool fromSaved = false;
    /// How the photograph was developed when the export was asked for.
    DevelopState state;
    /// How the photograph must be decoded, from decodeOptionsFor() (ADR 049).
    DecodeOptions decode;
    /// The photograph as it was when the export was asked for, whose lens corrections the export
    /// thread resolves against the lens database (::arraw::lensCorrectionFor, ADR 050): off the
    /// interface thread, and by the same code every other front end uses. Empty to apply none.
    std::optional<Photo> document;
    /// File to decode when source is null; the export thread then decodes it with decode.
    std::filesystem::path input;
    /// Full-resolution decoded photograph, shared, never copied; null when the window's source
    /// was decoded with other options, so that the export thread decodes input itself.
    std::shared_ptr<const ImageBuffer> source;
    /// Size and filter to render at.
    RenderRequest request;
    /// Format, colour, and compression of the file.
    ExportOptions options;
    /// File to create or replace.
    std::filesystem::path path;
    /// Metadata to carry into the file; none when nothing is to be carried.
    std::optional<ExportMetadata> metadata;
};

/// @brief Outcome of one export.
struct ExportResult {
    /// Identifier ExportQueue::enqueue returned for the job.
    std::uint64_t id = 0;
    /// File the job was to write.
    std::filesystem::path path;
    /// Description of the failure; empty when the file was written.
    std::string error;
    /// Sentences about what the file was written without, as metadata that could not be
    /// carried; empty when nothing was left out. Meaningful only when error is empty.
    std::vector<std::string> warnings;
    /// Whether the GPU developed the photograph, rather than the CPU.
    bool onGpu = false;
    /// Why the CPU developed when the GPU was wanted, or empty when it was not.
    std::string fallbackReason;
    /// Sentences about lens corrections that were asked for and could not be applied, such as a
    /// lens that is not in the database; empty when all were applied or none asked. The export
    /// still went ahead without them. Meaningful only when error is empty.
    std::vector<std::string> lensNotices;
};

/// @brief Worker thread that develops and writes exports, one after the other.
///
/// One long-lived thread, as PreviewRenderer has, so that a GPU context lives
/// and dies on the thread that uses it. Jobs run first in, first out. Each is
/// developed on the GPU when there is one, and on the CPU otherwise or when the
/// GPU fails it (a lost device means the CPU for good), then written with
/// ::arraw::exportImage, which replaces the file atomically: a job that fails,
/// or is cut off by the destructor, leaves no partial file. Uses no Qt signals,
/// so it works without an event loop.
class ExportQueue {
public:
    /// @brief Where exports may be developed.
    enum class Device {
        Auto, ///< On the GPU when one can be used, else on the CPU.
        Cpu,  ///< Always on the CPU, without ever creating a GPU device.
    };

    /// @brief Starts the worker thread.
    /// @param onResult Receives each finished job, successful or not, called on
    /// the worker thread; the caller marshals it where it is needed. Must not
    /// throw; what it throws is dropped.
    /// @param device Where exports may be developed.
    /// @param settings Desktop GPU preference, captured for the lifetime of the worker.
    /// @param lensDatabase Gives the lens database to resolve corrections against, called on the
    /// worker when a job asks for one; empty for ::arraw::app::systemLensDatabase.
    explicit ExportQueue(std::function<void(ExportResult)> onResult, Device device = Device::Auto,
                         AppSettings settings = {}, LensDatabaseSource lensDatabase = {});

    ExportQueue(const ExportQueue&) = delete;
    ExportQueue& operator=(const ExportQueue&) = delete;
    ExportQueue(ExportQueue&&) = delete;
    ExportQueue& operator=(ExportQueue&&) = delete;

    /// @brief Stops the worker and waits for it.
    ///
    /// Waits for the job in progress, which cannot be interrupted (and whose
    /// file is written atomically), and drops the jobs not yet started. The
    /// callback is not called once this returns.
    ~ExportQueue();

    /// @brief Appends a job.
    /// @param job What to export.
    /// @return Identifier of the job, increasing with each call.
    std::uint64_t enqueue(ExportJob job);

    /// @brief Drops the jobs not yet started; the one in progress finishes.
    /// @return How many were dropped. They produce no result.
    std::size_t cancelQueued();

private:
    /// Job waiting for the worker, with its identifier.
    struct Pending {
        std::uint64_t id;
        ExportJob job;
    };

    /// @brief Serves jobs until asked to stop.
    /// @param stop Raised by the destructor.
    void run(std::stop_token stop);

    /// Receiver of each finished job, called on the worker thread.
    std::function<void(ExportResult)> onResult_;

    /// Where exports may be developed; fixed before the worker starts.
    Device device_;

    /// Desktop preferences captured before the worker starts.
    AppSettings settings_;

    /// Gives the lens database; called on the worker.
    LensDatabaseSource lensDatabase_;

    /// Guard of queue_ and lastId_.
    std::mutex mutex_;

    /// Signal that a job is queued or the worker should stop.
    std::condition_variable_any wake_;

    /// Jobs not yet started, oldest first.
    std::deque<Pending> queue_;

    /// Identifier of the newest job.
    std::uint64_t lastId_ = 0;

    /// Declared last so that every member above exists before it starts.
    std::jthread worker_;
};

} // namespace arraw::app
