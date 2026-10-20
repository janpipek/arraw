#pragma once

#include "FolderResult.h"

#include <QObject>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace arraw::app {

/// @brief Walker of a folder tree on a thread of its own, one directory per turn of its event
/// loop.
///
/// Lives on the thread its owner (::arraw::app::FolderWatcher) moves it to. A walk reads the root
/// with ::arraw::listFolder and, when it is recursive, every listed subfolder, in pre-order with
/// the subfolders of each directory in ::arraw::folderPrecedes order: the order of the film
/// strip's rows, so the rows a walk brings are always appended. Each directory is one queued
/// call, so a new walk is taken up between two directories, never in the middle of one.
///
/// The directories read are handed over in batches: the root's at once, then whenever
/// ::arraw::app::FolderLister::batchShots shots have gathered, or
/// ::arraw::app::FolderLister::batchInterval has passed since the last batch, or the walk is
/// done. A directory that cannot be read (gone, no permission, an I/O error) is counted and
/// skipped with its subtree; its siblings are still read.
///
/// Walks are numbered by the caller. Starting one abandons the walk under way: nothing more of
/// it is delivered once the next directory begins, and it is never reported finished.
class FolderLister : public QObject {
public:
    /// @brief Receives a batch, on the lister thread: the walk's number and its directories, in
    /// walk order.
    using BatchReceiver = std::function<void(std::uint64_t, std::vector<FolderResult>)>;

    /// @brief Receives the end of a walk, on the lister thread: its number and what it saw.
    using FinishReceiver = std::function<void(std::uint64_t, ListingSummary)>;

    /// @brief Is called on the lister thread before each directory is read; a seam for tests,
    /// which may block in it.
    using ReadHook = std::function<void(const std::filesystem::path&)>;

    /// @brief Shots after which a batch is handed over.
    static constexpr std::size_t batchShots = 512;

    /// @brief Time after which a batch is handed over, however few shots it holds.
    static constexpr std::chrono::milliseconds batchInterval{100};

    /// @brief Makes an idle lister.
    /// @param onBatch Receives each batch; it must marshal to its own thread, and not throw.
    /// @param onFinished Receives the end of each walk that was not abandoned; the same.
    FolderLister(BatchReceiver onBatch, FinishReceiver onFinished);

    /// @brief Starts a walk, abandoning the one under way. Safe from any thread.
    /// @param root Folder to walk; a trailing separator is dropped.
    /// @param recursive Whether to enter its subfolders.
    /// @param walk Number of the walk, never 0, stamped on what it delivers.
    void start(const std::filesystem::path& root, bool recursive, std::uint64_t walk);

    /// @brief Abandons the walk under way, if any. Safe from any thread.
    void cancel();

    /// @brief Sets the function called before each directory is read. Safe from any thread.
    /// @param hook Function, or an empty one for none.
    void setReadHook(ReadHook hook);

private:
    /// @brief Takes up a walk on the lister thread, unless a later one was started since.
    void begin(const std::filesystem::path& root, bool recursive, std::uint64_t walk);

    /// @brief Reads the next directory and hands over what is due.
    void step();

    /// @brief Queues the next step on the lister thread.
    void scheduleStep();

    /// @brief Hands the gathered directories over.
    void flush();

    /// @brief Drops the walk's state.
    void abandon();

    /// @brief Tells whether the walk at hand is still the one wanted.
    [[nodiscard]] bool current() const noexcept {
        return walk_ != 0 && walk_ == wanted_.load();
    }

    /// Receiver of batches.
    BatchReceiver onBatch_;
    /// Receiver of finished walks.
    FinishReceiver onFinished_;
    /// Walk last started, written by any thread; 0 for none.
    std::atomic<std::uint64_t> wanted_{0};
    /// Guard of the hook.
    std::mutex hookMutex_;
    /// Function called before each read.
    ReadHook hook_;

    // State of the walk at hand, touched on the lister thread only.

    /// Number of the walk at hand; 0 when idle.
    std::uint64_t walk_ = 0;
    /// Number of the chain of queued steps that serves the walk at hand.
    std::uint64_t chain_ = 0;
    /// Whether it enters subfolders.
    bool recursive_ = false;
    /// Directories still to read, the next one first.
    std::deque<ListedDirectory> work_;
    /// Directories read and not handed over yet.
    std::vector<FolderResult> batch_;
    /// Shots in the batch.
    std::size_t batchShotCount_ = 0;
    /// Whether a batch was handed over in this walk.
    bool flushed_ = false;
    /// When the last batch was handed over, or the walk began.
    std::chrono::steady_clock::time_point lastFlush_;
    /// What the walk saw so far.
    ListingSummary summary_;
};

} // namespace arraw::app
