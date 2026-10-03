#include "FolderWatcher.h"

#include "ShotModel.h"

#include <Sidecar.h>

#include <exception>
#include <system_error>

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
    : QObject(parent), model_(model) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(debounce);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &debounce_,
            qOverload<>(&QTimer::start));
    connect(&debounce_, &QTimer::timeout, this, [this] {
        model_.refresh();
        // A folder that was replaced loses its watch.
        if (watcher_.directories().isEmpty()) {
            rewatch();
        }
    });
    connect(&model_, &QAbstractItemModel::modelReset, this, [this] { rewatch(); });
    connect(&model_, &ShotModel::sidecarRefreshed, this, &FolderWatcher::onSidecarRefreshed);
    rewatch();
}

void FolderWatcher::noteOwnWrite(const std::filesystem::path& sidecar) {
    if (const auto stamp = fileStamp(sidecar)) {
        ownStamps_[normalised(sidecar)] = *stamp;
    }
}

void FolderWatcher::rewatch() {
    debounce_.stop();
    if (const QStringList old = watcher_.directories(); !old.isEmpty()) {
        watcher_.removePaths(old);
    }
    if (!model_.folder().empty()) {
        watcher_.addPath(QString::fromStdU16String(model_.folder().u16string()));
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
