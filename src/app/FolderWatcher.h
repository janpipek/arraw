#pragma once

#include "FolderLister.h"
#include "FolderResult.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace arraw::app {

class ShotModel;

/// @brief Keeps a ::arraw::app::ShotModel in step with its folder on disk.
///
/// Owns the ::arraw::app::FolderLister and its thread. Each walk the model asks for (a new
/// folder, see ::arraw::app::ShotModel::setFolder, or a refresh) runs on that thread, and its
/// batches and its end are handed to the model on this one
/// (::arraw::app::ShotModel::applyFolders, ::arraw::app::ShotModel::finishListing).
///
/// Watches the model's folder. A change to the folder's entries is debounced, because a program
/// that writes a file touches the folder several times, and then handed to
/// ::arraw::app::ShotModel::refresh, together with the directories that reported a change. The
/// watcher also tells apart sidecar changes the application made from those other programs did.
///
/// For a model that lists subfolders, every sub-directory is watched too. The directories to
/// watch are the ones the last finished walk read, so the tree is walked once, not twice; the
/// watch set is changed by what differs, not dropped and made again. At most
/// ::arraw::app::FolderWatcher::maxWatchedFolders are watched, the folder itself and the
/// shallowest sub-directories first, because the system's number of watches (inotify on Linux)
/// is finite and shared with other programs; beyond that the deeper sub-directories are simply
/// not watched, and their changes show on the next refresh caused by something else. A
/// directory is watched only once its walk has finished, so a change made to it between its
/// read and that moment is seen only with the next change.
///
/// Only changes to the folder's entries are seen: a file added, removed or
/// renamed, and so a sidecar that another program replaces. One that is
/// rewritten in place without changing the folder goes unnoticed until the next
/// folder event, as no watch is kept on each file.
class FolderWatcher : public QObject {
    Q_OBJECT

public:
    /// @brief Starts the lister's thread and follows what the model shows.
    ///
    /// Answers the walks the model asks for from now on; a folder the model shows already is
    /// watched from its next walk.
    /// @param model Model to keep up to date; must outlive the watcher.
    /// @param debounce Quiet time to wait after a change before refreshing.
    /// @param parent Owner.
    explicit FolderWatcher(ShotModel& model,
                           std::chrono::milliseconds debounce = std::chrono::milliseconds(300),
                           QObject* parent = nullptr);

    /// @brief Stops the lister, after the directory it is reading.
    ~FolderWatcher() override;

    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;
    FolderWatcher(FolderWatcher&&) = delete;
    FolderWatcher& operator=(FolderWatcher&&) = delete;

    /// @brief Most directories watched at once.
    static constexpr int maxWatchedFolders = 1000;

    /// @brief Records a sidecar the application has just written.
    ///
    /// Call it right after the write, before control returns to the event loop.
    /// The sidecar's present modification time is remembered, and a change to
    /// the file that leaves that time is not reported as external.
    /// @param sidecar Path of the sidecar written (see ::arraw::sidecarPath).
    void noteOwnWrite(const std::filesystem::path& sidecar);

    /// @brief Lists the directories under watch.
    /// @return Their paths, as Qt spells them.
    [[nodiscard]] QStringList watchedDirectories() const {
        return watcher_.directories();
    }

    /// @brief Sets a function the lister calls on its thread before it reads each directory.
    ///
    /// A seam for tests, which block in it to hold a walk at a known point.
    /// @param hook Function, or an empty one for none.
    void setReadHook(FolderLister::ReadHook hook);

signals:
    /// @brief Announces that another program changed a shot's sidecar.
    ///
    /// The marks of the model are already refreshed. Whether the photograph
    /// open for editing must reload is for the window to decide.
    /// @param primary Path of the shot's primary file.
    void sidecarChangedExternally(const QString& primary);

private:
    /// @brief Starts the walk the model asks for.
    void startWalk();

    /// @brief Hands a finished walk to the model and watches what it read.
    /// @param walk Number of the walk.
    /// @param summary What it saw.
    void finishWalk(std::uint64_t walk, const ListingSummary& summary);

    /// @brief Watches the shallowest of the directories a walk read, and no others.
    /// @param directories Directories of the walk, in walk order.
    void rewatch(const std::vector<ListedDirectory>& directories);

    /// @brief Judges a refreshed sidecar.
    /// @param primary Shot whose sidecar was re-read.
    /// @param stamp Modification stamp of the sidecar now.
    void onSidecarRefreshed(const QString& primary, qint64 stamp);

    /// Model kept up to date.
    ShotModel& model_;
    /// Watch on the folder.
    QFileSystemWatcher watcher_;
    /// Timer that waits for the folder to be quiet.
    QTimer debounce_;
    /// Directories that reported a change since the last refresh.
    std::vector<std::filesystem::path> dirty_;
    /// Stamp of each sidecar the application wrote, by normalised path.
    std::map<std::filesystem::path, std::int64_t> ownStamps_;
    /// Folder the watches were last set for.
    std::filesystem::path watchedRoot_;
    /// Thread the lister runs on.
    QThread thread_;
    /// Lister; lives on thread_, deleted once that has stopped.
    std::unique_ptr<FolderLister> lister_;
};

} // namespace arraw::app
