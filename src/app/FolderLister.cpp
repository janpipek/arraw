#include "FolderLister.h"

#include <Shot.h>

#include <QMetaObject>

#include <exception>
#include <utility>

namespace arraw::app {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

FolderLister::FolderLister(BatchReceiver onBatch, FinishReceiver onFinished)
    : onBatch_(std::move(onBatch)), onFinished_(std::move(onFinished)) {}

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

void FolderLister::setReadHook(ReadHook hook) {
    const std::scoped_lock lock(hookMutex_);
    hook_ = std::move(hook);
}

void FolderLister::begin(const fs::path& root, bool recursive, std::uint64_t walk) {
    if (walk != wanted_.load()) {
        return; // Started and superseded before this thread got to it.
    }
    abandon();
    walk_ = walk;
    ++chain_;
    recursive_ = recursive;
    work_.push_back({withoutTrailingSeparator(root), 0});
    lastFlush_ = Clock::now();
    scheduleStep();
}

void FolderLister::scheduleStep() {
    QMetaObject::invokeMethod(
        this,
        [this, chain = chain_] {
            // A step queued for an earlier walk must not run a second chain of this one.
            if (chain == chain_) {
                step();
            }
        },
        Qt::QueuedConnection);
}

void FolderLister::step() {
    if (!current() || work_.empty()) {
        if (walk_ != 0 && !current()) {
            abandon();
        }
        return;
    }
    const ListedDirectory directory = std::move(work_.front());
    work_.pop_front();

    ReadHook hook;
    {
        const std::scoped_lock lock(hookMutex_);
        hook = hook_;
    }
    if (hook) {
        hook(directory.path);
        if (!current()) {
            abandon();
            return;
        }
    }

    try {
        FolderListing listing = listFolder(directory.path);
        if (recursive_) {
            // Pre-order: the subfolders come next, before the directory's later siblings.
            for (auto child = listing.folders.rbegin(); child != listing.folders.rend(); ++child) {
                work_.push_front({std::move(*child), directory.depth + 1});
            }
        }
        summary_.directories.push_back(directory);
        summary_.shots += static_cast<int>(listing.shots.size());
        batchShotCount_ += listing.shots.size();
        batch_.push_back({directory.path, std::move(listing.shots)});
    } catch (const std::exception&) {
        // Gone, no permission or an I/O error: skipped with its subtree (plan, section 6).
        ++summary_.unreadable;
    }
    if (!current()) {
        abandon();
        return;
    }

    const bool done = work_.empty();
    if (!flushed_ || done || batchShotCount_ >= batchShots ||
        Clock::now() - lastFlush_ >= batchInterval) {
        flush();
    }
    if (done) {
        const std::uint64_t walk = walk_;
        ListingSummary summary = std::move(summary_);
        abandon();
        onFinished_(walk, std::move(summary));
        return;
    }
    scheduleStep();
}

void FolderLister::flush() {
    flushed_ = true;
    lastFlush_ = Clock::now();
    if (batch_.empty()) {
        return;
    }
    onBatch_(walk_, std::exchange(batch_, {}));
    batchShotCount_ = 0;
}

void FolderLister::abandon() {
    walk_ = 0;
    work_.clear();
    batch_.clear();
    batchShotCount_ = 0;
    flushed_ = false;
    summary_ = {};
}

} // namespace arraw::app
