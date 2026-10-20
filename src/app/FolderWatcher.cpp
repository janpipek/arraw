#include "FolderWatcher.h"

#include "ShotModel.h"

#include <Sidecar.h>

#include <QDeadlineTimer>
#include <QMetaObject>

#include <exception>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

namespace arraw::app {

namespace {

/// @brief Spells a path one way, so that two ways of naming a file compare equal.
std::filesystem::path normalised(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : result;
}

} // namespace

void FolderWatcher::Link::post(std::function<void(FolderWatcher&)> call) {
    const std::scoped_lock lock(mutex);
    if (target == nullptr) {
        return;
    }
    // Queued while the lock is held, so the watcher cannot be destroyed between the check and
    // the posting; an event still queued when it is destroyed is dropped with it.
    QMetaObject::invokeMethod(
        target, [self = target, call = std::move(call)] { call(*self); }, Qt::QueuedConnection);
}

FolderWatcher::FolderWatcher(ShotModel& model, std::chrono::milliseconds debounce, QObject* parent)
    : QObject(parent), model_(model), worker_(std::make_unique<Worker>()) {
    link_->target = this;
    // The lister keeps only the link, never the watcher or the model: after the thread is
    // orphaned, nothing it does reaches either.
    worker_->lister = std::make_unique<FolderLister>(
        [link = link_](std::uint64_t walk, std::vector<FolderResult> batch) {
            // On the lister thread. The model applies it a slice at a time
            // (ShotModel::queueFolders).
            link->post([walk, batch = std::move(batch)](FolderWatcher& self) mutable {
                self.model_.queueFolders(walk, std::move(batch));
            });
        },
        [link = link_](std::uint64_t walk, ListingSummary summary) {
            link->post([walk, summary = std::move(summary)](FolderWatcher& self) mutable {
                self.finishWalk(walk, std::move(summary));
            });
        },
        [link = link_](std::uint64_t walk, WatchStatus status) {
            link->post([walk, status = std::move(status)](FolderWatcher& self) {
                self.takeStatus(walk, status);
            });
        },
        debounce);
    lister_ = worker_->lister.get();
    worker_->setObjectName(QStringLiteral("FolderLister"));
    lister_->moveToThread(worker_.get());
    // The system watcher and the debounce timer live on the lister's thread, and go there.
    connect(
        worker_.get(), &QThread::finished, lister_, [lister = lister_] { lister->shutDown(); },
        Qt::DirectConnection);
    worker_->start();

    connect(&model_, &ShotModel::walkRequested, this, &FolderWatcher::startWalk);
    connect(&model_, &ShotModel::sidecarRefreshed, this, &FolderWatcher::onSidecarRefreshed);
}

FolderWatcher::~FolderWatcher() {
    {
        // Nothing the lister delivers from now on reaches this watcher.
        const std::scoped_lock lock(link_->mutex);
        link_->target = nullptr;
    }
    lister_->cancel();
    worker_->quit();
    if (worker_->wait(QDeadlineTimer(exitWait_))) {
        return; // Deleted with the worker, the thread having stopped.
    }
    // A directory that does not answer (a hard-mounted share that went away): the thread is let
    // go. It ends when the call returns (the lister then finds its walk cancelled and does
    // nothing), releases its watches, and deletes itself.
    Worker* orphan = worker_.release();
    connect(orphan, &QThread::finished, orphan, &QObject::deleteLater);
    if (orphan->isFinished()) {
        orphan->deleteLater(); // Finished between the wait and the connection.
    }
}

void FolderWatcher::setReadHook(FolderLister::ReadHook hook) {
    lister_->setReadHook(std::move(hook));
}

void FolderWatcher::setWatchGate(FolderLister::WatchGate gate) {
    lister_->setWatchGate(std::move(gate));
}

void FolderWatcher::simulateDirectoryChanged(const std::filesystem::path& directory,
                                             bool watchDropped) {
    lister_->simulateDirectoryChanged(directory, watchDropped);
}

void FolderWatcher::refreshAll() {
    model_.refresh();
}

void FolderWatcher::startWalk() {
    // The lister keeps the tree and its watches for the same root, and drops them for another.
    lister_->start(model_.folder(), model_.recursive(), model_.walk());
}

void FolderWatcher::takeStatus(std::uint64_t walk, const WatchStatus& status) {
    if (walk != model_.walk()) {
        return; // A later walk replaced it.
    }
    emit watchStatusChanged(status);
}

void FolderWatcher::finishWalk(std::uint64_t walk, ListingSummary summary) {
    if (walk != model_.walk()) {
        return; // A later walk replaced it; the model drops it too.
    }
    // The model takes the end after the batches it has still queued.
    model_.queueFinish(walk, std::move(summary));
}

void FolderWatcher::noteOwnWrite(const std::filesystem::path& sidecar) {
    if (const auto stamp = fileStamp(sidecar)) {
        ownStamps_[normalised(sidecar)] = *stamp;
    }
}

void FolderWatcher::onSidecarRefreshed(const QString& primary, qint64 stamp) {
    try {
        const auto sidecar =
            normalised(sidecarPath(std::filesystem::path(primary.toStdU16String())));
        const auto own = ownStamps_.find(sidecar);
        if (own != ownStamps_.end() && own->second == stamp) {
            return;
        }
    } catch (const std::exception&) {
        // A sidecar that cannot be named is not one we wrote.
    }
    emit sidecarChangedExternally(primary);
}

} // namespace arraw::app
