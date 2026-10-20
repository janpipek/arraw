#include "FolderLister.h"

#include <Shot.h>

#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace arraw::app {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

/// @brief Spells a path as Qt's watcher takes and reports it.
QString toQString(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

} // namespace

FolderLister::FolderLister(BatchReceiver onBatch, FinishReceiver onFinished,
                           StatusReceiver onStatus, std::chrono::milliseconds debounce)
    : onBatch_(std::move(onBatch)), onFinished_(std::move(onFinished)),
      onStatus_(std::move(onStatus)), debounce_(debounce) {}

FolderLister::~FolderLister() = default;

void FolderLister::start(const fs::path& root, bool recursive, std::uint64_t walk) {
    // Written at once, so that the walk under way stops before its next directory even while
    // the queued call below waits behind it.
    wanted_.store(walk);
    QMetaObject::invokeMethod(
        this, [this, root, recursive, walk] { begin(root, recursive, walk); },
        Qt::QueuedConnection);
}

void FolderLister::cancel() {
    wanted_.store(0);
}

void FolderLister::shutDown() {
    debounceTimer_.reset();
    watcher_.reset();
    const std::scoped_lock lock(watchedMutex_);
    watchedCopy_.clear();
}

void FolderLister::setMaxWatchedFolders(int limit) {
    maxWatched_.store(std::max(limit, 0));
}

void FolderLister::setWatchGate(WatchGate gate) {
    const std::scoped_lock lock(hookMutex_);
    gate_ = std::move(gate);
}

void FolderLister::setReadHook(ReadHook hook) {
    const std::scoped_lock lock(hookMutex_);
    hook_ = std::move(hook);
}

QStringList FolderLister::watchedDirectories() const {
    const std::scoped_lock lock(watchedMutex_);
    return {watchedCopy_.begin(), watchedCopy_.end()};
}

void FolderLister::simulateDirectoryChanged(const fs::path& directory, bool watchDropped) {
    QMetaObject::invokeMethod(
        this,
        [this, path = toQString(directory), watchDropped] {
            if (watchDropped && watcher_) {
                // As Qt does for a directory removed, moved or unmounted: its own books only.
                watcher_->removePath(path);
            }
            onDirectoryChanged(path);
        },
        Qt::QueuedConnection);
}

void FolderLister::ensureWatcher() {
    if (watcher_) {
        return;
    }
    watcher_ = std::make_unique<QFileSystemWatcher>();
    connect(watcher_.get(), &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) { onDirectoryChanged(path); });
    debounceTimer_ = std::make_unique<QTimer>();
    debounceTimer_->setSingleShot(true);
    connect(debounceTimer_.get(), &QTimer::timeout, this, [this] { readChanged(); });
}

void FolderLister::begin(const fs::path& root, bool recursive, std::uint64_t walk) {
    if (walk != wanted_.load()) {
        return; // Started and superseded before this thread got to it.
    }
    ensureWatcher();
    abandon();
    const fs::path folder = withoutTrailingSeparator(root);
    if (folder != root_) {
        // The watches and the changes of another folder say nothing of this one.
        unwatchAll();
        tree_.clear();
        failed_.clear();
        rootUnavailable_ = false;
        rootError_.clear();
        budget_.reset(maxWatched_.load());
        dirty_.clear();
        debounceTimer_->stop();
        root_ = folder;
    } else {
        // A watch Qt dropped while its report could not be taken (review R2) is had again: the
        // walk watches before it reads what is not watched.
        forgetDroppedWatches();
        if (!dirty_.empty()) {
            // Reports put off while no walk was wanted.
            debounceTimer_->start(0);
        }
    }
    recursive_ = recursive;
    walk_ = walk;
    walking_ = true;
    flushed_ = false;
    lastFlush_ = Clock::now();
    const auto rootDir = track(root_, 0);
    // A new walk tries again for watches the system refused, and takes up a new cap.
    applyMoves(budget_.setCap(maxWatched_.load()));
    enqueue(rootDir, true, false);
    scheduleStep();
}

void FolderLister::scheduleStep() {
    if (stepQueued_) {
        return;
    }
    stepQueued_ = true;
    QMetaObject::invokeMethod(this, [this] { step(); }, Qt::QueuedConnection);
}

bool FolderLister::enqueue(Tree::iterator directory, bool enter, bool recheck) {
    Dir& dir = directory->second;
    const bool wasQueued = dir.queued;
    const bool deeper = enter && !dir.enter;
    dir.queued = true;
    dir.enter = dir.enter || enter;
    dir.recheck = dir.recheck || recheck;
    if (wasQueued && !deeper) {
        return false; // Its read is still to come, and covers this one (plan, section 4, rule 4).
    }
    // A read that now enters its subfolders moves to the front; the old place is skipped.
    work_.push_front(directory->first);
    return true;
}

void FolderLister::step() {
    stepQueued_ = false;
    if (!current()) {
        abandon();
        return;
    }
    auto directory = tree_.end();
    while (!work_.empty() && directory == tree_.end()) {
        const auto found = tree_.find(work_.front());
        work_.pop_front();
        if (found != tree_.end() && found->second.queued) {
            directory = found;
        }
    }
    if (directory == tree_.end()) {
        // Nothing left to read: what is gathered goes, then the watch status.
        flush();
        const bool finishing = walking_;
        publishStatus(finishing);
        if (finishing) {
            walking_ = false;
            onFinished_(walk_, std::exchange(summary_, {}));
        }
        return;
    }

    Dir& dir = directory->second;
    const bool enter = dir.enter;
    const bool recheck = dir.recheck;
    dir.queued = false;
    dir.enter = false;
    dir.recheck = false;
    // Watch, then read: a change after the read is reported, one before it is in the read.
    const fs::path path = directory->first;
    // A watch refused for a directory that did not open, kept for after the read.
    bool retry = false;
    if (!dir.watched && budget_.isWatched(path)) {
        retry = !watch(directory) && budget_.isWatched(path);
        // A watch on a dead mount can block as long as a read: a walk cancelled meanwhile does
        // not go on to read the same path (review R3).
        if (!current()) {
            abandon();
            return;
        }
    }

    ReadHook hook;
    {
        const std::scoped_lock lock(hookMutex_);
        hook = hook_;
    }

    std::optional<FolderListing> listing;
    FolderError error = FolderError::Other;
    std::string message;
    try {
        if (hook) {
            hook(path);
            if (!current()) {
                abandon();
                return;
            }
        }
        listing = listFolder(path);
    } catch (const fs::filesystem_error& failure) {
        error = classifyFolderError(failure.code());
        message = failure.code().message();
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    if (!current()) {
        abandon();
        return;
    }
    if (!listing) {
        handleFailure(directory, error, std::move(message));
    } else {
        // Back after a failure: what lies below it was not read meanwhile (the root's
        // subfolders are not read while it is unavailable), and its watches may be gone. Its
        // subtree is walked again, so the notice clears only once all of it is read (review R1).
        const bool recovered = (path == root_ && rootUnavailable_) || failed_.contains(path);
        if (path == root_) {
            rootUnavailable_ = false;
            rootError_.clear();
        } else if (rootUnavailable_) {
            // Read while the root is not (renamed away and back, say): the root may be back too,
            // and is read to see, with its subtree if it is.
            enqueue(tree_.find(root_), true, true);
        }
        failed_.erase(path);
        if (recovered) {
            forgetDroppedWatches();
        }
        if (retry && watch(directory)) {
            // It opens after all (replaced between the two, or a share's hiccup): watched now,
            // after the read, so read once more (plan, section 4, rule 2; review R4).
            enqueue(directory, false, true);
        }
        batchShotCount_ += listing->shots.size();
        batch_.push_back({.folder = path,
                          .shots = std::move(listing->shots),
                          .gone = false,
                          .recheck = recheck || recovered});
        const std::vector<fs::path> folders =
            recursive_ ? std::move(listing->folders) : std::vector<fs::path>{};
        forgetVanishedFailures(path, folders);
        followSubfolders(directory, folders, enter || recovered, recovered);
    }

    // The first shots go at once, however deep the walk had to go to find them.
    if ((!flushed_ && batchShotCount_ > 0) || batchShotCount_ >= batchShots ||
        Clock::now() - lastFlush_ >= batchInterval) {
        flush();
    }
    scheduleStep();
}

void FolderLister::followSubfolders(Tree::iterator directory, const std::vector<fs::path>& folders,
                                    bool enter, bool recheck) {
    const fs::path& parent = directory->first;
    const int depth = directory->second.depth + 1;
    // The subfolders known: the direct children within the directory's range of the tree.
    std::vector<fs::path> known;
    for (auto entry = std::next(directory); entry != tree_.end() && isWithin(entry->first, parent);
         ++entry) {
        if (entry->second.depth == depth) {
            known.push_back(entry->first);
        }
    }
    // Both lists are in folder order: one pass tells the new, the gone and the kept.
    const FolderOrder precedes;
    std::vector<Tree::iterator> toRead;
    std::vector<fs::path> gone;
    std::size_t k = 0;
    for (const fs::path& folder : folders) {
        while (k < known.size() && precedes(known[k], folder)) {
            gone.push_back(known[k++]);
        }
        if (k < known.size() && known[k] == folder) {
            ++k;
            if (enter) {
                toRead.push_back(tree_.find(folder));
            }
            continue;
        }
        toRead.push_back(track(folder, depth));
    }
    gone.insert(gone.end(), known.begin() + static_cast<std::ptrdiff_t>(k), known.end());
    for (const fs::path& folder : gone) {
        dropSubtree(tree_.find(folder));
    }
    // Pre-order: the subfolders come next, before the directory's later siblings.
    for (auto child = toRead.rbegin(); child != toRead.rend(); ++child) {
        enqueue(*child, true, recheck);
    }
}

void FolderLister::dropSubtree(Tree::iterator directory) {
    fs::path folder = directory->first;
    auto end = std::next(directory);
    while (end != tree_.end() && isWithin(end->first, folder)) {
        ++end;
    }
    QStringList released;
    std::vector<fs::path> removed;
    for (auto entry = directory; entry != end; ++entry) {
        if (entry->second.watched) {
            released << toQString(entry->first);
        }
        removed.push_back(entry->first);
    }
    unwatch(released);
    forgetFailures(folder);
    tree_.erase(directory, end);
    // The places the subtree leaves go to directories that waited.
    applyMoves({.promoted = budget_.remove(removed), .evicted = {}});
    batch_.push_back({.folder = std::move(folder), .shots = {}, .gone = true, .recheck = false});
}

void FolderLister::handleFailure(Tree::iterator directory, FolderError kind, std::string message) {
    const fs::path path = directory->first;
    const auto keepStale = [&] {
        if (walking_) {
            summary_.stale.push_back(path);
        }
    };
    if (path == root_) {
        // Unmounted, renamed, a share asleep: the rows stay as they were, and the next report or
        // refresh that can read it merges it normally (plan, answer 1). Its subfolders are not
        // read, and not swept as gone either.
        rootUnavailable_ = true;
        rootError_ = std::move(message);
        keepStale();
        return;
    }
    if (kind != FolderError::Other && !rootOpens()) {
        // Under a root unmounted (every watch reports) or renamed away (the subfolders' watches
        // follow them and report their old paths), every subfolder fails as gone. It is kept as
        // the root's rows are, and read again with the root's subtree when the root reads
        // (review B1).
        keepStale();
        return;
    }
    switch (kind) {
    case FolderError::Gone:
        dropSubtree(directory);
        break;
    case FolderError::NoPermission:
        // Treated as gone, as a fresh listing would show it (answer 2); counted until a read
        // of its parent finds it readable again.
        dropSubtree(directory);
        failed_.insert_or_assign(path, std::move(message));
        break;
    case FolderError::Other:
        // Perhaps transient: the rows stay, and its next report or refresh reads it again.
        failed_.insert_or_assign(path, std::move(message));
        keepStale();
        break;
    }
}

bool FolderLister::rootOpens() {
    std::error_code error;
    const fs::directory_iterator probe(root_, error);
    if (error) {
        rootUnavailable_ = true;
        rootError_ = error.message();
    }
    return !error;
}

void FolderLister::forgetFailures(const fs::path& folder) {
    if (failed_.empty()) {
        return;
    }
    const auto first = failed_.lower_bound(folder);
    auto last = first;
    while (last != failed_.end() && isWithin(last->first, folder)) {
        ++last;
    }
    failed_.erase(first, last);
}

void FolderLister::forgetVanishedFailures(const fs::path& directory,
                                          const std::vector<fs::path>& folders) {
    if (failed_.empty()) {
        return;
    }
    std::vector<fs::path> vanished;
    for (auto entry = failed_.lower_bound(directory);
         entry != failed_.end() && isWithin(entry->first, directory); ++entry) {
        if (entry->first.parent_path() == directory &&
            std::ranges::find(folders, entry->first) == folders.end()) {
            vanished.push_back(entry->first);
        }
    }
    for (const fs::path& folder : vanished) {
        forgetFailures(folder);
    }
}

FolderLister::Tree::iterator FolderLister::track(const fs::path& path, int depth) {
    const auto [entry, added] = tree_.try_emplace(path, Dir{.depth = depth});
    if (added) {
        // It is watched when it is read; a shallower one than a watched directory takes its place.
        if (const auto grant = budget_.add(path, depth); grant.evict) {
            applyMoves({.promoted = {}, .evicted = {*grant.evict}});
        }
    }
    return entry;
}

bool FolderLister::watch(Tree::iterator directory) {
    const QString path = toQString(directory->first);
    WatchGate gate;
    {
        const std::scoped_lock lock(hookMutex_);
        gate = gate_;
    }
    if ((!gate || gate(directory->first)) && watcher_->addPath(path)) {
        directory->second.watched = true;
        const std::scoped_lock lock(watchedMutex_);
        watchedCopy_.insert(path);
        return true;
    }
    // Refused. A directory that opens is out of watches (ENOSPC, shared with other programs):
    // the budget shrinks to what the system gave. One that does not is going, or gone: it keeps
    // its place until its read settles it, dropped with its subtree or watched after all. Its
    // place is not handed on now, as under a vanished root or mount the next one would fail too,
    // and the next, through every waiting directory (review R4, R5).
    std::error_code error;
    const fs::directory_iterator probe(directory->first, error);
    if (!error) {
        budget_.refused(directory->first);
    }
    return false;
}

void FolderLister::applyMoves(WatchBudget::Moves moves) {
    QStringList released;
    for (const fs::path& path : moves.evicted) {
        if (const auto entry = tree_.find(path); entry != tree_.end() && entry->second.watched) {
            entry->second.watched = false;
            released << toQString(path);
        }
    }
    unwatch(released);
    for (const fs::path& path : moves.promoted) {
        if (!current()) {
            // Each watch may block on a dead mount (review R3). Those not had yet are had before
            // their next read, which a later walk does.
            break;
        }
        const auto entry = tree_.find(path);
        if (entry == tree_.end() || entry->second.watched) {
            continue;
        }
        // A watch got after the read: read once more, so nothing between the two is missed
        // (plan, section 4, rule 2). One refused is not handed on (see watch).
        if (watch(entry)) {
            enqueue(entry, false, true);
        }
    }
    scheduleStep();
}

void FolderLister::unwatch(const QStringList& paths) {
    if (paths.isEmpty()) {
        return;
    }
    // A watch Qt dropped already is simply not removed again.
    watcher_->removePaths(paths);
    const std::scoped_lock lock(watchedMutex_);
    for (const QString& path : paths) {
        watchedCopy_.remove(path);
    }
}

void FolderLister::unwatchAll() {
    if (const QStringList present = watcher_->directories(); !present.isEmpty()) {
        watcher_->removePaths(present);
    }
    for (auto& [path, dir] : tree_) {
        dir.watched = false;
    }
    const std::scoped_lock lock(watchedMutex_);
    watchedCopy_.clear();
}

void FolderLister::forgetDroppedWatches() {
    const QStringList listed = watcher_->directories();
    const QSet<QString> present(listed.begin(), listed.end());
    const std::scoped_lock lock(watchedMutex_);
    for (auto& [path, dir] : tree_) {
        if (dir.watched) {
            if (const QString spelled = toQString(path); !present.contains(spelled)) {
                dir.watched = false;
                watchedCopy_.remove(spelled);
            }
        }
    }
}

void FolderLister::publishStatus(bool force) {
    constexpr std::size_t listedFailures = 5;
    WatchStatus status{.watched = budget_.watched(),
                       .directories = static_cast<int>(tree_.size()),
                       .unreadable = static_cast<int>(failed_.size()),
                       .unreadableFolders = {},
                       .rootUnavailable = rootUnavailable_,
                       .rootError = rootError_,
                       .limit = budget_.cap()};
    for (const auto& [folder, message] : failed_) {
        if (status.unreadableFolders.size() == listedFailures) {
            break;
        }
        status.unreadableFolders.push_back(folder);
    }
    if (force || lastStatus_ != status) {
        lastStatus_ = status;
        onStatus_(walk_, status);
    }
}

void FolderLister::onDirectoryChanged(const QString& path) {
    fs::path directory(path.toStdU16String());
    if (!tree_.contains(directory)) {
        return; // Dropped from the tree since; its parent's read said so.
    }
    if (!current()) {
        // Between a walk's start and its begin: kept for that walk, which reads it first if it
        // is of this tree, and drops it if not (review R2). It may be Qt's report of a watch it
        // dropped, which nothing else would tell.
        dirty_.insert(std::move(directory));
        return;
    }
    const auto now = Clock::now();
    if (dirty_.empty()) {
        firstDirty_ = now;
    }
    dirty_.insert(std::move(directory));
    // Trailing, but never later than the longest wait after the first report.
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(maxLatency - (now - firstDirty_));
    debounceTimer_->start(std::max(std::chrono::milliseconds(0), std::min(debounce_, left)));
}

void FolderLister::readChanged() {
    if (!current()) {
        return; // Kept for the walk that is to begin (see begin).
    }
    // Qt drops the watch of a directory removed, moved or unmounted, and reports it. One that
    // lists still gets its watch again before its read (plan, section 4, rule 5).
    std::optional<QSet<QString>> present;
    std::vector<Tree::iterator> changed;
    for (const fs::path& directory : dirty_) {
        const auto entry = tree_.find(directory);
        if (entry == tree_.end()) {
            continue;
        }
        if (entry->second.watched) {
            if (!present) {
                const QStringList watched = watcher_->directories();
                present.emplace(watched.begin(), watched.end());
            }
            if (const QString path = toQString(directory); !present->contains(path)) {
                entry->second.watched = false;
                const std::scoped_lock lock(watchedMutex_);
                watchedCopy_.remove(path);
            }
        }
        changed.push_back(entry);
    }
    dirty_.clear();
    // Ahead of a walk under way, in folder order.
    for (auto entry = changed.rbegin(); entry != changed.rend(); ++entry) {
        enqueue(*entry, false, true);
    }
    scheduleStep();
}

void FolderLister::flush() {
    lastFlush_ = Clock::now();
    if (batch_.empty()) {
        return;
    }
    flushed_ = flushed_ || batchShotCount_ > 0;
    onBatch_(walk_, std::exchange(batch_, {}));
    batchShotCount_ = 0;
}

void FolderLister::abandon() {
    work_.clear();
    for (auto& [path, dir] : tree_) {
        dir.queued = false;
        dir.enter = false;
        dir.recheck = false;
    }
    batch_.clear();
    batchShotCount_ = 0;
    walking_ = false;
    summary_ = {};
}

} // namespace arraw::app
