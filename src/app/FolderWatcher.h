#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>

namespace arraw::app {

class ShotModel;

/// @brief Keeps a ::arraw::app::ShotModel in step with its folder on disk.
///
/// Watches the model's folder (following ::arraw::app::ShotModel::setFolder).
/// A change to the folder's entries is debounced, because a program that
/// writes a file touches the folder several times, and then handed to
/// ::arraw::app::ShotModel::refresh. The watcher also tells apart sidecar
/// changes the application made from those other programs did.
///
/// Only changes to the folder's entries are seen: a file added, removed or
/// renamed, and so a sidecar that another program replaces. One that is
/// rewritten in place without changing the folder goes unnoticed until the next
/// folder event, as no watch is kept on each file.
class FolderWatcher : public QObject {
    Q_OBJECT

public:
    /// @brief Starts watching what the model shows.
    /// @param model Model to keep up to date; must outlive the watcher.
    /// @param debounce Quiet time to wait after a change before refreshing.
    /// @param parent Owner.
    explicit FolderWatcher(ShotModel& model,
                           std::chrono::milliseconds debounce = std::chrono::milliseconds(300),
                           QObject* parent = nullptr);

    /// @brief Records a sidecar the application has just written.
    ///
    /// Call it right after the write, before control returns to the event loop.
    /// The sidecar's present modification time is remembered, and a change to
    /// the file that leaves that time is not reported as external.
    /// @param sidecar Path of the sidecar written (see ::arraw::sidecarPath).
    void noteOwnWrite(const std::filesystem::path& sidecar);

signals:
    /// @brief Announces that another program changed a shot's sidecar.
    ///
    /// The marks of the model are already refreshed. Whether the photograph
    /// open for editing must reload is for the window to decide.
    /// @param primary Path of the shot's primary file.
    void sidecarChangedExternally(const QString& primary);

private:
    /// @brief Points the watch at the model's folder.
    void rewatch();

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
    /// Stamp of each sidecar the application wrote, by normalised path.
    std::map<std::filesystem::path, std::int64_t> ownStamps_;
};

} // namespace arraw::app
