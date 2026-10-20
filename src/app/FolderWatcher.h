#pragma once

#include "FolderLister.h"
#include "FolderResult.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

namespace arraw::app {

class ShotModel;

/// @brief Keeps a ::arraw::app::ShotModel in step with its folder on disk.
///
/// Owns the ::arraw::app::FolderLister and its thread, which walks the folder, watches its
/// directories and reads again each directory that reports a change. Each walk the model asks
/// for (a new folder, see ::arraw::app::ShotModel::setFolder, or a refresh of all, see
/// ::arraw::app::FolderWatcher::refreshAll) runs on that thread; its batches, the directories
/// read again after a change, and the end of each walk are queued on the model on this one
/// (::arraw::app::ShotModel::queueFolders, ::arraw::app::ShotModel::queueFinish).
///
/// A change to a directory's entries is debounced on the lister's thread, because a program that
/// writes a file touches the directory several times, and then only that directory is read again
/// and merged into its rows, with a check of their sidecars. A subfolder that appears is walked;
/// one that goes takes its subtree's rows with it. Every directory is watched before it is read,
/// so no change between the two is missed (plan, section 4). At most
/// ::arraw::app::FolderWatcher::maxWatchedFolders are watched, the shallowest first, because the
/// system's number of watches (inotify on Linux) is finite and shared with other programs; changes
/// in the others show on the next refresh of all.
///
/// The watcher also tells apart sidecar changes the application made from those other programs
/// did. Only changes to a directory's entries are seen: a file added, removed or renamed, and so a
/// sidecar that another program replaces. One that is rewritten in place without changing the
/// directory goes unnoticed until the next event there, as no watch is kept on each file.
class FolderWatcher : public QObject {
    Q_OBJECT

public:
    /// @brief Starts the lister's thread and follows what the model shows.
    ///
    /// Answers the walks the model asks for from now on; a folder the model shows already is
    /// watched from its next walk.
    /// @param model Model to keep up to date; must outlive the watcher.
    /// @param debounce Quiet time to wait after a directory's change before reading it again.
    /// @param parent Owner.
    explicit FolderWatcher(ShotModel& model,
                           std::chrono::milliseconds debounce = std::chrono::milliseconds(300),
                           QObject* parent = nullptr);

    /// @brief Stops the lister, after the directory it is reading, and releases the watches.
    ~FolderWatcher() override;

    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;
    FolderWatcher(FolderWatcher&&) = delete;
    FolderWatcher& operator=(FolderWatcher&&) = delete;

    /// @brief Most directories watched at once, unless set otherwise.
    static constexpr int maxWatchedFolders = FolderLister::maxWatchedFolders;

    /// @brief Sets the most directories to watch, from the next walk on.
    /// @param limit Cap; below 1 counts as 1.
    void setMaxWatchedFolders(int limit) {
        lister_->setMaxWatchedFolders(std::max(limit, 1));
    }

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
        return lister_->watchedDirectories();
    }

    /// @brief Walks the whole folder again and merges what it finds, folder by folder.
    ///
    /// For what no watch reports: a directory beyond
    /// ::arraw::app::FolderWatcher::maxWatchedFolders, or a sidecar rewritten in place. It runs in
    /// the background, keeps the selection and the active shot, and checks every loaded row's
    /// sidecar (see ::arraw::app::ShotModel::refresh).
    void refreshAll();

    /// @brief Sets a function the lister calls on its thread before it asks the system for a
    /// watch.
    ///
    /// A seam for tests, which return false to have the watch refused as the system's limit would.
    /// @param gate Function, or an empty one for none.
    void setWatchGate(FolderLister::WatchGate gate);

    /// @brief Sets a function the lister calls on its thread before it reads each directory.
    ///
    /// A seam for tests, which block in it to hold a walk at a known point.
    /// @param hook Function, or an empty one for none.
    void setReadHook(FolderLister::ReadHook hook);

    /// @brief Takes a directory's report of a change as if the system had sent it.
    ///
    /// A seam for tests, which do not depend on the system's timing this way.
    /// @param directory Directory, spelled as the listing spells it.
    /// @param watchDropped Whether the system watcher first drops its watch, as Qt does when a
    /// watched directory is removed, moved or unmounted.
    void simulateDirectoryChanged(const std::filesystem::path& directory,
                                  bool watchDropped = false);

signals:
    /// @brief Announces that the watch status changed: when a walk ends, and when a change in the
    /// tree or the system's refusal of a watch alters it.
    /// @param status What is watched and what is not.
    void watchStatusChanged(const arraw::app::WatchStatus& status);

    /// @brief Announces that another program changed a shot's sidecar.
    ///
    /// The marks of the model are already refreshed. Whether the photograph
    /// open for editing must reload is for the window to decide.
    /// @param primary Path of the shot's primary file.
    void sidecarChangedExternally(const QString& primary);

private:
    /// @brief Starts the walk the model asks for.
    void startWalk();

    /// @brief Hands a finished walk to the model.
    /// @param walk Number of the walk.
    /// @param summary What it saw.
    void finishWalk(std::uint64_t walk, ListingSummary summary);

    /// @brief Relays the lister's watch status.
    /// @param walk Number of the walk it belongs to.
    /// @param status The status.
    void takeStatus(std::uint64_t walk, const WatchStatus& status);

    /// @brief Judges a refreshed sidecar.
    /// @param primary Shot whose sidecar was re-read.
    /// @param stamp Modification stamp of the sidecar now.
    void onSidecarRefreshed(const QString& primary, qint64 stamp);

    /// Model kept up to date.
    ShotModel& model_;
    /// Stamp of each sidecar the application wrote, by normalised path.
    std::map<std::filesystem::path, std::int64_t> ownStamps_;
    /// Thread the lister runs on.
    QThread thread_;
    /// Lister; lives on thread_, deleted once that has stopped.
    std::unique_ptr<FolderLister> lister_;
};

} // namespace arraw::app
