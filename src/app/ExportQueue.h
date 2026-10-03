#pragma once

#include "AppSettings.h"

#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageExport.h>

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
    /// How the photograph was developed when the export was asked for.
    DevelopState state;
    /// Full-resolution decoded photograph; shared, never copied.
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
    explicit ExportQueue(std::function<void(ExportResult)> onResult, Device device = Device::Auto,
                         AppSettings settings = {});

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
