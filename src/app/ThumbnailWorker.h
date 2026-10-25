#pragma once

#include "LensSupport.h"
#include "ThumbnailCache.h"

#include <SnapshotThumbnails.h>
#include <Snapshots.h>

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
#include <string>
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

/// @brief One snapshot thumbnail the worker made.
struct SnapshotThumbnailResult {
    /// Shot's primary file.
    std::filesystem::path primary;
    /// Snapshot it is of.
    SnapshotId id;
    /// The thumbnail: validated, within the size and byte limits, with its render key.
    SnapshotThumbnail thumbnail;
    /// ::arraw::app::ThumbnailWorker::snapshotGeneration when the job was queued.
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
///
/// Snapshot thumbnails (ADR 056, decision 9) are a third kind of job, apart from the shots above.
/// Each is made from an explicit, immutable ::arraw::SnapshotRecipe the caller hands over, never
/// from the sidecar, and is served after the embedded jobs and the developed jobs of the visible
/// shots, and before the developed jobs of the rest (ADR 056, decision 14). The order is: urgent,
/// embedded (visible first), developed of visible shots, snapshots, the other developed. They
/// belong to a *snapshot generation* the caller starts with
/// ::arraw::app::ThumbnailWorker::beginSnapshots when it opens another photograph, and a result
/// of an older one is never delivered. A new folder leaves them alone.
///
/// Threading: one mutex guards the queues; the two generations are atomics that the worker thread
/// reads between the steps of a job. The result callbacks run on the worker thread and own their
/// data; the caller marshals them to the thread that owns its session.
class ThumbnailWorker {
public:
    /// @brief Starts the thread, and the one-off pruning of the cache beside it.
    /// @param cache Where thumbnails are kept; copied.
    /// @param onResult Receives each thumbnail, on the worker thread; the caller marshals it. A
    /// result whose generation is not ::arraw::app::ThumbnailWorker::generation any more is stale.
    /// Must not throw.
    /// @param pruneAtStart Whether to prune the cache to its cap on a second background thread.
    /// @param lensDatabase Gives the lens database for a shot whose saved settings turn a lens
    /// correction on; called on the worker thread, where the first load may take a second. Empty
    /// for ::arraw::app::systemLensDatabase.
    /// @param onSnapshotResult Receives each snapshot thumbnail, on the worker thread, only while
    /// its snapshot generation is current at the check, which is not atomic with the delivery: a
    /// consumer that posts the result to another thread must compare
    /// ::arraw::app::SnapshotThumbnailResult::generation with
    /// ::arraw::app::ThumbnailWorker::snapshotGeneration and the primary with its session's path
    /// there. It then hands the thumbnail to the snapshot writer
    /// (::arraw::EditSession::prepareSnapshotThumbnail, ::arraw::app::SnapshotWriter), which
    /// reports the stamp of its own write. Must not throw. Empty to drop them.
    ThumbnailWorker(ThumbnailCache cache, std::function<void(ThumbnailResult)> onResult,
                    bool pruneAtStart = true, LensDatabaseSource lensDatabase = {},
                    std::function<void(SnapshotThumbnailResult)> onSnapshotResult = {});

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

    /// @brief Drops the jobs still queued for shots that are no longer shown; the running one
    /// finishes. Adding them again queues them anew.
    /// @param primaries Primary files.
    void removeShots(const std::vector<std::filesystem::path>& primaries);

    /// @brief Names the shots on screen, to serve first.
    /// @param primaries Primary files; replaces the previous list.
    void setVisible(std::vector<std::filesystem::path> primaries);

    /// @brief Queues a developed thumbnail afresh, because the shot's saved settings changed.
    ///
    /// Served before every other job. Its result is delivered like any other, so the caller
    /// decides whether it still wants it.
    /// @param primary Primary file.
    void invalidate(const std::filesystem::path& primary);

    /// @brief Starts a new snapshot generation, for another photograph or a fresh start.
    ///
    /// Drops every queued snapshot job, and the one running is not delivered.
    /// @return The new snapshot generation.
    std::uint64_t beginSnapshots();

    /// @brief Queues the thumbnail of a snapshot, rendered from the recipe given here.
    ///
    /// Asking again for a snapshot that is still queued replaces the earlier request. The worker
    /// works out the render key (::arraw::snapshotThumbnailKey, which reads the file, so not on
    /// the caller's thread) and drops the job, with no result and no render, when it equals
    /// @p storedKey; otherwise the thumbnail is looked up in the cache by the key first.
    /// @param primary Photograph the snapshot belongs to.
    /// @param recipe Immutable recipe to render; copied.
    /// @param generation Snapshot generation the caller got from
    /// ::arraw::app::ThumbnailWorker::beginSnapshots.
    /// @param storedKey Render key of the thumbnail the sidecar holds for the snapshot; empty for
    /// none.
    /// @return False, with nothing queued, when @p generation is not the current one.
    bool requestSnapshotThumbnail(const std::filesystem::path& primary, SnapshotRecipe recipe,
                                  std::uint64_t generation, std::string storedKey = {});

    /// @brief Gives the current snapshot generation.
    /// @return The generation.
    [[nodiscard]] std::uint64_t snapshotGeneration() const noexcept {
        return snapshotGeneration_.load();
    }

    /// @brief Gives the current generation, which a new folder increments.
    /// @return The generation.
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load();
    }

    /// @brief Counts the jobs queued or running.
    /// @return Jobs still owed.
    [[nodiscard]] std::size_t pendingJobs() const;

private:
    /// @brief A snapshot thumbnail to make.
    struct SnapshotJob {
        std::filesystem::path primary;
        SnapshotRecipe recipe;
        std::uint64_t generation = 0;
        /// Key of the thumbnail the sidecar holds; empty for none.
        std::string storedKey;
    };

    /// @brief One thing to make.
    struct Job {
        std::filesystem::path primary;
        ThumbnailKind kind = ThumbnailKind::Embedded;
        /// Set for a snapshot job, which then ignores @ref kind.
        std::optional<SnapshotJob> snapshot;
    };

    /// @brief Serves jobs until asked to stop.
    void run(const std::stop_token& stop);

    /// @brief Takes the next job by priority, or waits for one.
    /// @return Nothing when asked to stop.
    [[nodiscard]] std::optional<Job> next(const std::stop_token& stop, std::uint64_t& generation);

    /// @brief Makes one job's thumbnail and delivers it.
    void execute(const Job& job, std::uint64_t generation, const std::stop_token& stop);

    /// @brief Makes one snapshot thumbnail and delivers it.
    void executeSnapshot(const SnapshotJob& job, const std::stop_token& stop);

    /// @brief Pops the first job of a queue that is visible, else the first.
    /// @pre The queue is not empty.
    Job takeFrom(std::deque<std::filesystem::path>& queue, ThumbnailKind kind);

    /// Where thumbnails are kept.
    ThumbnailCache cache_;
    /// Gives the lens database; called on the worker thread.
    LensDatabaseSource lensDatabase_;
    /// Receiver of results, called on the worker thread.
    std::function<void(ThumbnailResult)> onResult_;
    /// Receiver of snapshot results, called on the worker thread; may be empty.
    std::function<void(SnapshotThumbnailResult)> onSnapshotResult_;
    /// Snapshot generation.
    std::atomic<std::uint64_t> snapshotGeneration_{0};
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
    /// Snapshot thumbnails still to do, of the current snapshot generation.
    std::deque<SnapshotJob> snapshots_;
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
