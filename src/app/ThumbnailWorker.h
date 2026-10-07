#pragma once

#include "ThumbnailCache.h"

#include <QImage>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace arraw::app {

/// @brief Which picture a thumbnail is.
enum class ThumbnailKind {
    Embedded,  ///< The camera's own preview.
    Developed, ///< arraw's rendering of the saved settings.
};

/// @brief One thumbnail the worker made.
struct ThumbnailResult {
    /// Shot's primary file.
    std::filesystem::path primary;
    /// Which picture it is.
    ThumbnailKind kind = ThumbnailKind::Embedded;
    /// The picture, at most ::arraw::app::ThumbnailCache::maxEdge on a side.
    QImage image;
    /// ::arraw::app::ThumbnailWorker::generation when the job was queued.
    std::uint64_t generation = 0;
};

/// @brief Gives a file's embedded camera preview, upright, at most
/// ::arraw::app::ThumbnailCache::maxEdge on a side.
///
/// From the cache when it holds it, else read from the file and kept there.
/// Safe from any thread, as the cache is.
/// @param cache Where embedded previews are kept.
/// @param file Photograph.
/// @return The preview, or a null image when the file has none or cannot be read.
/// @throws std::exception if reading the file fails in a way readEmbeddedPreview does not catch.
[[nodiscard]] QImage embeddedPreviewImage(const ThumbnailCache& cache,
                                          const std::filesystem::path& file);

/// @brief Background thread that makes the film strip's thumbnails.
///
/// One thread, at a lower priority than the rest of the application, that uses the CPU only and
/// never a GPU context: the preview owns the GPU (ADR 031). Per shot it does two jobs: the
/// camera's embedded preview, from the cache or from the file, and then arraw's developed
/// rendering of the *saved* settings (sidecar), from the cache or from a half-size decode
/// developed to ::arraw::app::ThumbnailCache::maxEdge and then cached. All embedded jobs of the
/// folder run before any developed one, and within each group the shots reported as visible
/// (::arraw::app::ThumbnailWorker::setVisible) go first.
///
/// ::arraw::app::ThumbnailWorker::invalidate puts a shot's developed job ahead of everything and
/// drops its embedded one, as the developed picture is about to replace it. A new folder
/// (::arraw::app::ThumbnailWorker::setShots) drops all queued jobs and the result of a running one
/// is not delivered. Uses no Qt signals, so it works without an event loop.
class ThumbnailWorker {
public:
    /// @brief Starts the thread, and the one-off pruning of the cache beside it.
    /// @param cache Where thumbnails are kept; copied.
    /// @param onResult Receives each thumbnail, on the worker thread; the caller marshals it. A
    /// result whose generation is not ::arraw::app::ThumbnailWorker::generation any more is stale.
    /// Must not throw.
    /// @param pruneAtStart Whether to prune the cache to its cap on a second background thread.
    ThumbnailWorker(ThumbnailCache cache, std::function<void(ThumbnailResult)> onResult,
                    bool pruneAtStart = true);

    ThumbnailWorker(const ThumbnailWorker&) = delete;
    ThumbnailWorker& operator=(const ThumbnailWorker&) = delete;
    ThumbnailWorker(ThumbnailWorker&&) = delete;
    ThumbnailWorker& operator=(ThumbnailWorker&&) = delete;

    /// @brief Stops both threads, after the file the worker is reading.
    ~ThumbnailWorker();

    /// @brief Replaces everything to do with the shots of a new folder, or the same one again.
    /// @param primaries Primary files, in the order shown.
    /// @return The new generation.
    std::uint64_t setShots(std::vector<std::filesystem::path> primaries);

    /// @brief Adds shots that appeared in the folder; those already queued are not doubled.
    /// @param primaries Primary files.
    void addShots(const std::vector<std::filesystem::path>& primaries);

    /// @brief Names the shots on screen, to serve first.
    /// @param primaries Primary files; replaces the previous list.
    void setVisible(std::vector<std::filesystem::path> primaries);

    /// @brief Queues a developed thumbnail afresh, because the shot's saved settings changed.
    ///
    /// Served before every other job. Its result is delivered like any other, so the caller
    /// decides whether it still wants it.
    /// @param primary Primary file.
    void invalidate(const std::filesystem::path& primary);

    /// @brief Gives the current generation, which a new folder increments.
    /// @return The generation.
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load();
    }

    /// @brief Counts the jobs queued or running.
    /// @return Jobs still owed.
    [[nodiscard]] std::size_t pendingJobs() const;

private:
    /// @brief One thing to make.
    struct Job {
        std::filesystem::path primary;
        ThumbnailKind kind = ThumbnailKind::Embedded;
    };

    /// @brief Serves jobs until asked to stop.
    void run(const std::stop_token& stop);

    /// @brief Takes the next job by priority, or waits for one.
    /// @return Nothing when asked to stop.
    [[nodiscard]] std::optional<Job> next(const std::stop_token& stop, std::uint64_t& generation);

    /// @brief Makes one job's thumbnail and delivers it.
    void execute(const Job& job, std::uint64_t generation);

    /// @brief Pops the first job of a queue that is visible, else the first.
    /// @pre The queue is not empty.
    Job takeFrom(std::deque<std::filesystem::path>& queue, ThumbnailKind kind);

    /// Where thumbnails are kept.
    ThumbnailCache cache_;
    /// Receiver of results, called on the worker thread.
    std::function<void(ThumbnailResult)> onResult_;
    /// Folder generation.
    std::atomic<std::uint64_t> generation_{0};

    /// Guard of the queues below.
    mutable std::mutex mutex_;
    /// Signal that a job is queued or the thread should stop.
    std::condition_variable_any wake_;
    /// Developed jobs of shots whose settings were just saved.
    std::deque<std::filesystem::path> urgent_;
    /// Shots whose embedded preview is still to do.
    std::deque<std::filesystem::path> embedded_;
    /// Shots whose developed thumbnail is still to do.
    std::deque<std::filesystem::path> developed_;
    /// Shots queued in this generation, so that adding twice does nothing.
    std::unordered_set<std::filesystem::path> known_;
    /// Shots on screen.
    std::unordered_set<std::filesystem::path> visible_;
    /// Whether a job is being made.
    bool running_ = false;

    /// Declared last so that everything above exists before they start.
    std::jthread pruner_;
    std::jthread worker_;
};

} // namespace arraw::app
