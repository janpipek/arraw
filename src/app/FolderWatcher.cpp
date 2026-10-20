#include "FolderWatcher.h"

#include "ShotModel.h"

#include <Sidecar.h>

#include <QMetaObject>

#include <exception>
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

FolderWatcher::FolderWatcher(ShotModel& model, std::chrono::milliseconds debounce, QObject* parent)
    : QObject(parent), model_(model),
      lister_(std::make_unique<FolderLister>(
          [this](std::uint64_t walk, std::vector<FolderResult> batch) {
              // On the lister thread. Dropped if the watcher is gone by the time it would run.
              // The model applies it a slice at a time (ShotModel::queueFolders).
              QMetaObject::invokeMethod(
                  this,
                  [this, walk, batch = std::move(batch)]() mutable {
                      model_.queueFolders(walk, std::move(batch));
                  },
                  Qt::QueuedConnection);
          },
          [this](std::uint64_t walk, ListingSummary summary) {
              QMetaObject::invokeMethod(
                  this,
                  [this, walk, summary = std::move(summary)]() mutable {
                      finishWalk(walk, std::move(summary));
                  },
                  Qt::QueuedConnection);
          },
          [this](std::uint64_t walk, WatchStatus status) {
              QMetaObject::invokeMethod(
                  this, [this, walk, status] { takeStatus(walk, status); }, Qt::QueuedConnection);
          },
          debounce)) {
    thread_.setObjectName(QStringLiteral("FolderLister"));
    lister_->moveToThread(&thread_);
    // The system watcher and the debounce timer live on the lister's thread, and go there.
    connect(
        &thread_, &QThread::finished, lister_.get(),
        [lister = lister_.get()] { lister->shutDown(); }, Qt::DirectConnection);
    thread_.start();

    connect(&model_, &ShotModel::walkRequested, this, &FolderWatcher::startWalk);
    connect(&model_, &ShotModel::sidecarRefreshed, this, &FolderWatcher::onSidecarRefreshed);
}

FolderWatcher::~FolderWatcher() {
    lister_->cancel();
    thread_.quit();
    // Waits for the directory being read; a share that hangs hangs here too (plan, section 6).
    thread_.wait();
    lister_.reset();
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
