#pragma once

#include "FolderResult.h"
#include "WatchBudget.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

class QFileSystemWatcher;
class QTimer;

namespace arraw::app {

/// @brief Reader and watcher of a folder tree, on a thread of its own, one directory per turn of
/// its event loop.
///
/// Lives on the thread its owner (::arraw::app::FolderWatcher) moves it to, and keeps there the
/// tree of the directories it listed, the system watches on them, the directories that reported a
/// change and the debounce of those reports.
///
/// **Walks.** A walk reads the root with ::arraw::listFolder and, when it is recursive, every
/// listed subfolder, in pre-order with the subfolders of each directory in
/// ::arraw::folderPrecedes order: the order of the film strip's rows, so the rows of a first
/// listing are always appended. The directories read are handed over in batches: at once when the
/// first shots are found, however deep, then whenever ::arraw::app::FolderLister::batchShots shots
/// have gathered, or ::arraw::app::FolderLister::batchInterval has passed since the last batch, or
/// there is nothing more to read. A walk of the root the tree already holds (a refresh, or the
/// same folder with or without its subfolders) keeps the watches there are.
///
/// **Watch, then read** (plan, section 4). Each directory is watched before it is read, so a
/// change made after the read is reported, and one made before is in the read. A directory that
/// gets its watch only after its read (a watch that could not be had at first, or that the system
/// dropped) is read once more. A ::arraw::app::WatchBudget decides which directories are watched:
/// at most the cap, the shallowest first. A directory that is shallower than one watched takes
/// its place; one that gains a watch when another goes is read once more. When the system refuses
/// a watch (its limit is shared with other programs) the cap shrinks to what it accepted, until
/// the next walk tries again. What is watched, and how many directories there are, is reported
/// as a ::arraw::app::WatchStatus when a walk ends and whenever it changes.
///
/// **Changes.** A directory that reports a change is read again, alone, once its reports have been
/// quiet for the debounce time (but no later than ::arraw::app::FolderLister::maxLatency after the
/// first): its result is merged into its rows and asks for its sidecars to be checked. A
/// subfolder that is new is walked with its subtree; one that went is handed over as gone, with
/// its subtree, and its watches are released. A directory that can no longer be read counts as
/// gone. A report for a directory still waiting to be read is folded into that read.
///
/// Results are stamped with the number of the walk of the tree they belong to, the last one
/// started. Starting one abandons the walk under way, and what was to be read for the walk before:
/// nothing more of it is delivered once the next directory begins, and it is never reported
/// finished.
class FolderLister : public QObject {
public:
    /// @brief Receiver of a batch, on the lister thread: the walk's number and its directories,
    /// in the order read.
    using BatchReceiver = std::function<void(std::uint64_t, std::vector<FolderResult>)>;

    /// @brief Receiver of the end of a walk, on the lister thread: its number and what it saw.
    using FinishReceiver = std::function<void(std::uint64_t, ListingSummary)>;

    /// @brief Receiver of the watch status, on the lister thread: the walk's number and the status.
    using StatusReceiver = std::function<void(std::uint64_t, WatchStatus)>;

    /// @brief Function called on the lister thread before each directory is read, once it is
    /// watched; a seam for tests, which may block in it.
    using ReadHook = std::function<void(const std::filesystem::path&)>;

    /// @brief Function called on the lister thread before the system is asked for a watch; a seam
    /// for tests, which return false to have the watch refused as the system's limit would.
    using WatchGate = std::function<bool(const std::filesystem::path&)>;

    /// @brief Shots after which a batch is handed over.
    static constexpr std::size_t batchShots = 512;

    /// @brief Time after which a batch is handed over, however few shots it holds.
    static constexpr std::chrono::milliseconds batchInterval{100};

    /// @brief Most directories watched at once, unless set otherwise.
    static constexpr int maxWatchedFolders = defaultMaxWatchedFolders;

    /// @brief Longest wait from a directory's first report of a change to its reading, however
    /// busy it stays (plan, answer 3).
    static constexpr std::chrono::milliseconds maxLatency{2000};

    /// @brief Makes an idle lister.
    /// @param onBatch Receives each batch; it must marshal to its own thread, and not throw.
    /// @param onFinished Receives the end of each walk that was not abandoned; the same.
    /// @param onStatus Receives the watch status; the same.
    /// @param debounce Quiet time to wait after a directory reported a change before reading it.
    FolderLister(BatchReceiver onBatch, FinishReceiver onFinished, StatusReceiver onStatus,
                 std::chrono::milliseconds debounce);

    /// @brief Destroys the lister; its thread must have stopped (see
    /// ::arraw::app::FolderLister::shutDown).
    ~FolderLister() override;

    FolderLister(const FolderLister&) = delete;
    FolderLister& operator=(const FolderLister&) = delete;
    FolderLister(FolderLister&&) = delete;
    FolderLister& operator=(FolderLister&&) = delete;

    /// @brief Starts a walk, abandoning the one under way. Safe from any thread.
    /// @param root Folder to walk; a trailing separator is dropped. Another root than the last
    /// one drops the tree, its watches and its pending changes.
    /// @param recursive Whether to enter its subfolders.
    /// @param walk Number of the walk, never 0, stamped on what it delivers and on every change
    /// read after it.
    void start(const std::filesystem::path& root, bool recursive, std::uint64_t walk);

    /// @brief Stops all work: the walk under way and the reading of changes. Safe from any thread.
    void cancel();

    /// @brief Releases the watches and the timer, on the lister thread as it ends.
    ///
    /// Connect it to the thread's `finished` signal with a direct connection, so that what lives
    /// on that thread is destroyed there.
    void shutDown();

    /// @brief Sets the most directories to watch, taken up at the start of the next walk. Safe
    /// from any thread.
    /// @param limit Cap; below 0 counts as 0.
    void setMaxWatchedFolders(int limit);

    /// @brief Sets the function called before the system is asked for a watch. Safe from any
    /// thread.
    /// @param gate Function, or an empty one for none.
    void setWatchGate(WatchGate gate);

    /// @brief Sets the function called before each directory is read. Safe from any thread.
    /// @param hook Function, or an empty one for none.
    void setReadHook(ReadHook hook);

    /// @brief Lists the directories under watch. Safe from any thread.
    /// @return Their paths, as Qt spells them, in no particular order.
    [[nodiscard]] QStringList watchedDirectories() const;

    /// @brief Takes a directory's report of a change as if the system had sent it. Safe from any
    /// thread; a seam for tests, which do not depend on the system's timing this way.
    /// @param directory Directory, spelled as the lister spells it.
    void simulateDirectoryChanged(const std::filesystem::path& directory);

private:
    /// @brief A directory of the tree.
    struct Dir {
        /// Depth below the root; 0 for the root.
        int depth = 0;
        /// Whether it has a watch.
        bool watched = false;
        /// Whether it waits in the queue to be read.
        bool queued = false;
        /// Whether its read is to read its known subfolders too (a walk).
        bool enter = false;
        /// Whether its read is for a change it reported.
        bool recheck = false;
    };

    /// @brief Directories of the tree, in folder order.
    using Tree = std::map<std::filesystem::path, Dir, FolderOrder>;

    /// @brief Makes the system watcher and the debounce timer, on the lister thread, once.
    void ensureWatcher();

    /// @brief Takes up a walk on the lister thread, unless a later one was started since.
    void begin(const std::filesystem::path& root, bool recursive, std::uint64_t walk);

    /// @brief Reads the next directory and hands over what is due.
    void step();

    /// @brief Queues the next step on the lister thread, unless one is queued already.
    void scheduleStep();

    /// @brief Puts a directory of the tree at the front of the queue, or folds what is asked into
    /// its queued read.
    /// @param directory Directory.
    /// @param enter Whether to read its known subfolders too.
    /// @param recheck Whether the read is for a change.
    /// @return Whether it went into the queue (false when it was there already).
    bool enqueue(Tree::iterator directory, bool enter, bool recheck);

    /// @brief Compares a directory's subfolders now with the tree, and queues what is to be read.
    /// @param directory Directory just read.
    /// @param folders Its listed subfolders, in folder order; none when not recursive.
    /// @param enter Whether its known subfolders are read again too.
    void followSubfolders(Tree::iterator directory,
                          const std::vector<std::filesystem::path>& folders, bool enter);

    /// @brief Drops a directory and its subtree from the tree, releasing their watches, and hands
    /// it over as gone.
    /// @param directory Directory, in the tree.
    void dropSubtree(Tree::iterator directory);

    /// @brief Adds a directory to the tree, and to the budget if it is new.
    /// @param path Directory.
    /// @param depth Depth below the root.
    /// @return Its entry.
    Tree::iterator track(const std::filesystem::path& path, int depth);

    /// @brief Asks the system for the watch the budget granted a directory.
    ///
    /// A refusal for a directory that can be read is the system's limit: the budget shrinks to
    /// the watches there are. One for a directory that cannot be read frees its place.
    /// @param directory Directory the budget put among the watched.
    /// @param promoted Receives the directories that a freed place goes to.
    /// @return Whether it is watched now.
    bool watch(Tree::iterator directory, std::vector<std::filesystem::path>& promoted);

    /// @brief Carries out the changes of places a budget reports: unwatches the evicted, watches
    /// the promoted and queues a read of each (plan, section 4, rule 2).
    /// @param moves Changes.
    void applyMoves(WatchBudget::Moves moves);

    /// @brief Releases the watches of some directories.
    /// @param paths Their paths.
    void unwatch(const QStringList& paths);

    /// @brief Releases every watch.
    void unwatchAll();

    /// @brief Hands the watch status over if it differs from the last one, or if asked to.
    /// @param force Whether to hand it over whatever the last one was.
    void publishStatus(bool force);

    /// @brief Takes a report of a change from the system watcher.
    void onDirectoryChanged(const QString& path);

    /// @brief Queues the reads of the directories that reported a change.
    void readChanged();

    /// @brief Hands the gathered directories over.
    void flush();

    /// @brief Drops the work at hand: what was to be read and what was not handed over yet.
    void abandon();

    /// @brief Tells whether the walk at hand is still the one wanted.
    [[nodiscard]] bool current() const noexcept {
        return walk_ != 0 && walk_ == wanted_.load();
    }

    /// Receiver of batches.
    BatchReceiver onBatch_;
    /// Receiver of finished walks.
    FinishReceiver onFinished_;
    /// Receiver of the watch status.
    StatusReceiver onStatus_;
    /// Quiet time before a changed directory is read.
    std::chrono::milliseconds debounce_;
    /// Walk last started, written by any thread; 0 for none, or after a cancel.
    std::atomic<std::uint64_t> wanted_{0};
    /// Guard of the hook.
    std::mutex hookMutex_;
    /// Function called before each read.
    ReadHook hook_;
    /// Function called before each watch is asked for.
    WatchGate gate_;
    /// Cap wanted for the next walk, written by any thread.
    std::atomic<int> maxWatched_{maxWatchedFolders};
    /// Guard of the copy of the watched set.
    mutable std::mutex watchedMutex_;
    /// Directories watched, as Qt spells them, for other threads to read.
    QSet<QString> watchedCopy_;

    // State touched on the lister thread only.

    /// System watcher; made on the lister thread.
    std::unique_ptr<QFileSystemWatcher> watcher_;
    /// Timer that waits for the changed directories to be quiet.
    std::unique_ptr<QTimer> debounceTimer_;
    /// Root of the tree; empty before the first walk.
    std::filesystem::path root_;
    /// Whether the tree enters subfolders.
    bool recursive_ = false;
    /// Every directory listed or about to be, empty ones included. A directory's subtree is the
    /// range from it to the first entry not within it.
    Tree tree_;
    /// Directories to read, the next one first; entries no longer queued in the tree are skipped.
    std::deque<std::filesystem::path> work_;
    /// Directories that reported a change, waiting for the debounce.
    std::set<std::filesystem::path, FolderOrder> dirty_;
    /// When the first of them reported.
    std::chrono::steady_clock::time_point firstDirty_;
    /// Which directories are to be watched.
    WatchBudget budget_{maxWatchedFolders};
    /// Directories the last walk could not read.
    int unreadable_ = 0;
    /// Status handed over last.
    std::optional<WatchStatus> lastStatus_;
    /// Number of the tree's walk, stamped on what is delivered; 0 when idle.
    std::uint64_t walk_ = 0;
    /// Whether a walk is under way, to be reported finished once nothing is left to read.
    bool walking_ = false;
    /// Whether a step is queued.
    bool stepQueued_ = false;
    /// Directories read and not handed over yet.
    std::vector<FolderResult> batch_;
    /// Shots in the batch.
    std::size_t batchShotCount_ = 0;
    /// Whether a batch with a shot was handed over in this walk.
    bool flushed_ = false;
    /// When the last batch was handed over, or the walk began.
    std::chrono::steady_clock::time_point lastFlush_;
    /// What the walk saw so far.
    ListingSummary summary_;
};

} // namespace arraw::app
