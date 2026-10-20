#include "FolderWatcher.h"

#include "ShotModel.h"

#include <Shot.h>
#include <Sidecar.h>

#include <QStringList>

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

/// @brief Lists a folder and its sub-directories, shallowest first, to a limit.
///
/// Enters the folders ::arraw::isListedFolder accepts, as ::arraw::listShots does.
QStringList directoriesToWatch(const std::filesystem::path& root, int limit) {
    QStringList result;
    std::vector<std::filesystem::path> level{root};
    while (!level.empty() && result.size() < limit) {
        std::vector<std::filesystem::path> next;
        for (const auto& directory : level) {
            if (result.size() >= limit) {
                break;
            }
            result << QString::fromStdU16String(directory.u16string());
            std::error_code error;
            std::filesystem::directory_iterator entries(
                directory, std::filesystem::directory_options::skip_permission_denied, error);
            for (const std::filesystem::directory_iterator end; !error && entries != end;
                 entries.increment(error)) {
                if (isListedFolder(*entries)) {
                    next.push_back(entries->path());
                }
            }
        }
        level = std::move(next);
    }
    return result;
}

} // namespace

FolderWatcher::FolderWatcher(ShotModel& model, std::chrono::milliseconds debounce, QObject* parent)
    : QObject(parent), model_(model) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(debounce);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        dirty_.push_back(std::filesystem::path(path.toStdU16String()));
        debounce_.start();
    });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        // The folders that changed are the ones whose sidecars are worth a look.
        model_.refresh(std::exchange(dirty_, {}));
        // A folder that was replaced loses its watch; new sub-directories need one.
        if (watcher_.directories().isEmpty() || model_.recursive()) {
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
    dirty_.clear();
    if (const QStringList old = watcher_.directories(); !old.isEmpty()) {
        watcher_.removePaths(old);
    }
    if (model_.folder().empty()) {
        return;
    }
    if (model_.recursive()) {
        watcher_.addPaths(directoriesToWatch(model_.folder(), maxWatchedFolders));
    } else {
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
