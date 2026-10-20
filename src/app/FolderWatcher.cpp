#include "FolderWatcher.h"

#include "ShotModel.h"

#include <Sidecar.h>

#include <QMetaObject>
#include <QSet>
#include <QStringList>

#include <algorithm>
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

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
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
                  this, [this, walk, summary = std::move(summary)] { finishWalk(walk, summary); },
                  Qt::QueuedConnection);
          })) {
    thread_.setObjectName(QStringLiteral("FolderLister"));
    lister_->moveToThread(&thread_);
    thread_.start();

    debounce_.setSingleShot(true);
    debounce_.setInterval(debounce);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        dirty_.push_back(std::filesystem::path(path.toStdU16String()));
        debounce_.start();
    });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        // The folders that changed are the ones whose sidecars are worth a look. A walk under
        // way is followed by this one (see ShotModel::refresh); the watches follow its end.
        model_.refresh(std::exchange(dirty_, {}));
    });
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

void FolderWatcher::startWalk() {
    if (model_.isListing()) {
        // A new folder, or the same one listed afresh: what was pending is for the rows that
        // went, and the watches of another folder report nothing of this one.
        debounce_.stop();
        dirty_.clear();
        if (model_.folder() != watchedRoot_) {
            if (const QStringList old = watcher_.directories(); !old.isEmpty()) {
                watcher_.removePaths(old);
            }
            watchedRoot_.clear();
        }
    }
    lister_->start(model_.folder(), model_.recursive(), model_.walk());
}

void FolderWatcher::finishWalk(std::uint64_t walk, const ListingSummary& summary) {
    if (walk != model_.walk()) {
        return; // A later walk replaced it; the model drops it too.
    }
    // Watched before the model may start a refresh it had put off, which reads them again. The
    // model takes the end after the batches it has still queued.
    rewatch(summary.directories);
    model_.queueFinish(walk, summary);
}

void FolderWatcher::rewatch(const std::vector<ListedDirectory>& directories) {
    // The shallowest first; within a depth the walk's order, which is the folder order.
    std::vector<const ListedDirectory*> order;
    order.reserve(directories.size());
    for (const ListedDirectory& directory : directories) {
        order.push_back(&directory);
    }
    std::ranges::stable_sort(order, {}, &ListedDirectory::depth);
    if (order.size() > static_cast<std::size_t>(maxWatchedFolders)) {
        order.resize(static_cast<std::size_t>(maxWatchedFolders));
    }
    QSet<QString> wanted;
    wanted.reserve(static_cast<qsizetype>(order.size()));
    for (const ListedDirectory* directory : order) {
        wanted.insert(toQString(directory->path));
    }
    // Qt forgets a watch whose directory was removed or replaced, so the set it reports is the
    // one to compare with.
    const QStringList present = watcher_.directories();
    QStringList gone;
    for (const QString& path : present) {
        if (!wanted.remove(path)) {
            gone << path;
        }
    }
    if (!gone.isEmpty()) {
        watcher_.removePaths(gone);
    }
    if (!wanted.isEmpty()) {
        QStringList added(wanted.begin(), wanted.end());
        watcher_.addPaths(added);
    }
    watchedRoot_ = model_.folder();
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
