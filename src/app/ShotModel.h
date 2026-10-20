#pragma once

#include <ExifInfo.h>
#include <PhotoMarks.h>
#include <Shot.h>

#include <QAbstractListModel>
#include <QImage>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace arraw::app {

/// @brief Tells the stamp of a file's modification time.
///
/// What the model compares to notice that a sidecar changed on disk.
/// @param path File to look at.
/// @return Its modification time as a count of the file clock's ticks, or nothing if it is not
/// there.
[[nodiscard]] std::optional<std::int64_t> fileStamp(const std::filesystem::path& path);

/// @brief Writes the tooltip of a shot.
///
/// The file name, the format label, then what the EXIF records, each line only
/// when there is something to say: the capture time, the camera, the lens, and
/// the exposure as `ISO 200  1/250 s  f/2.8  35 mm`, in the order main's film
/// strip used.
/// @param shot Shot to describe.
/// @param exif What its primary records, or nothing while that is not read yet.
/// @param hasSidecar Whether to say that the shot has a sidecar.
/// @param shownName What to call the primary in the first line, such as its path below the
/// folder; empty for its file name.
/// @return The lines, joined by newlines.
[[nodiscard]] QString shotTooltip(const Shot& shot, const std::optional<ExifInfo>& exif,
                                  bool hasSidecar = false,
                                  const std::filesystem::path& shownName = {});

/// @brief Rows of the shots of one folder, with their marks and capture information.
///
/// The listing is made at once and opens no file (ADR 029). The marks of each
/// sidecar and the EXIF of each photograph are read afterwards on one
/// background thread, marks of every shot before any EXIF, and handed back to
/// this thread, so a folder of a thousand files does not block the window. EXIF
/// is read by that loader rather than on the first tooltip request: a tooltip
/// is drawn on the GUI thread, which must not open a RAW. Changing the folder
/// drops what the loader has not done and ignores what it is still doing.
///
/// The rows are kept as one contiguous span per folder, folders in
/// ::arraw::folderPrecedes order. The model changes by inserting and removing runs of rows rather
/// than by resetting (see ::arraw::app::ShotModel::refresh), so views keep their selection; only
/// a change too scattered for that, past ::arraw::app::maxFolderRuns runs in a folder, resets it.
class ShotModel : public QAbstractListModel {
    Q_OBJECT

public:
    /// @brief What a row tells a view, beside Qt's own roles.
    enum Role {
        PathRole = Qt::UserRole + 1, ///< Path of the primary file, as a `QString`.
        CompanionsRole,              ///< Paths of the companions, as a `QStringList`.
        FormatLabelRole,             ///< Label such as `ARW+JPEG`, as a `QString`.
        RatingRole,                  ///< Rating, -1 to 5, as an `int`; 0 until read.
        LabelRole,       ///< Colour label as an `int` of ::arraw::ColorLabel, -1 for none.
        MarksLoadedRole, ///< Whether the sidecar has been read, as a `bool`.
        HasSidecarRole,  ///< Whether a sidecar file exists, as a `bool`; false until read.
        ThumbnailRole,   ///< Thumbnail, as a `QImage`; null until one is set.
    };

    /// @brief Makes an empty model and starts its loader thread.
    /// @param parent Owner.
    explicit ShotModel(QObject* parent = nullptr);

    /// @brief Stops the loader and waits for the file it is reading.
    ~ShotModel() override;

    /// @brief Shows the shots of a folder.
    ///
    /// Lists them synchronously, resets the model, and queues the marks and EXIF
    /// reads. Anything the loader was doing for the previous folder is dropped.
    /// @param folder Folder to list.
    /// @param recursive Whether to list its subfolders too (see ::arraw::listShots); remembered
    /// for ::arraw::app::ShotModel::refresh. Changing it for the same folder resets the model
    /// like a new folder does.
    /// @throws std::runtime_error if the folder cannot be read; the model is left as it was.
    void setFolder(const std::filesystem::path& folder, bool recursive = false);

    /// @brief Tells whether the subfolders are listed.
    /// @return The flag of the last ::arraw::app::ShotModel::setFolder.
    [[nodiscard]] bool recursive() const noexcept {
        return recursive_;
    }

    /// @brief Names the folder shown.
    /// @return The folder, empty before ::arraw::app::ShotModel::setFolder.
    [[nodiscard]] const std::filesystem::path& folder() const noexcept {
        return folder_;
    }

    /// @brief Lists the folder again and merges the result in, folder by folder.
    ///
    /// Lists synchronously (the whole tree, if the model is recursive). Each folder's shots are
    /// compared in linear time with its span of rows (see ::arraw::app::diffFolder). Rows of
    /// shots that are still there stay, with their marks, EXIF and thumbnail; shots that went are
    /// removed and new ones inserted, in order, one insert or remove per run of adjacent rows, so
    /// a view's selection follows its shot. A shot whose companions changed is updated in place.
    /// If some folder needs more than ::arraw::app::maxFolderRuns runs, the whole model is reset
    /// instead (rows still keep their data), and views must restore their selection.
    ///
    /// Then the sidecars are checked on the loader thread, and re-read when the modification
    /// time is not the one last seen; ::arraw::app::ShotModel::sidecarRefreshed reports each of
    /// those. Only the rows of some folders are checked, not all: with @p dirty, those of the
    /// folders it names and of the folders whose listing changed; without it, every loaded row.
    /// New rows are loaded as for a new folder. A folder that can no longer be read counts as
    /// empty.
    /// @param dirty Folders where something happened (such as a sidecar written, which no
    /// listing shows), spelled as the watcher spells them; nothing to check every row.
    void refresh(const std::optional<std::vector<std::filesystem::path>>& dirty = std::nullopt);

    /// @brief Counts the shots.
    /// @return Number of rows.
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;

    /// @brief Reads one role of one row.
    /// @param index Row.
    /// @param role A ::arraw::app::ShotModel::Role, `Qt::DisplayRole` (the primary's file name) or
    /// `Qt::ToolTipRole` (see ::arraw::app::shotTooltip).
    /// @return The value, or an invalid variant.
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

    /// @brief Returns the shot of a row.
    /// @param row Row, which must exist.
    /// @return The shot.
    [[nodiscard]] const Shot& shot(int row) const;

    /// @brief Finds the row of a shot.
    /// @param primary Path of the shot's primary file.
    /// @return Its row, or -1 when this folder has no such shot.
    [[nodiscard]] int rowOf(const std::filesystem::path& primary) const;

    /// @brief Returns the marks of a row.
    /// @param row Row, which must exist.
    /// @return The marks, defaults until read.
    [[nodiscard]] PhotoMarks marks(int row) const;

    /// @brief Returns the EXIF of a row.
    /// @param row Row, which must exist.
    /// @return What the primary records; nothing until read.
    [[nodiscard]] const std::optional<ExifInfo>& exif(int row) const;

    /// @brief Sets the marks of a shot, once they are written to its sidecar.
    ///
    /// Does not write anything; the application does that through
    /// ::arraw::writeSidecarMarks and tells the watcher (see
    /// ::arraw::app::FolderWatcher::noteOwnWrite). Marks the row as read, so a
    /// reading still under way cannot replace them.
    /// @param primary Path of the shot's primary file; an unknown one is ignored.
    /// @param marks Marks to show.
    void setMarks(const std::filesystem::path& primary, const PhotoMarks& marks);

    /// @brief Takes note that the application wrote or created the sidecar of a shot.
    ///
    /// Looks for the file again (the stamp is left, so a recheck still reads it), so
    /// ::arraw::app::ShotModel::HasSidecarRole is current without waiting for a recheck;
    /// ::arraw::app::ShotModel::setMarks does this itself.
    /// @param primary Path of the shot's primary file; an unknown one is ignored.
    void noteSidecarWritten(const std::filesystem::path& primary);

    /// @brief Sets the thumbnail of a shot; the seam for the thumbnail worker.
    /// @param primary Path of the shot's primary file; an unknown one is ignored.
    /// @param thumbnail Image to show, or a null one to clear it.
    void setThumbnail(const std::filesystem::path& primary, QImage thumbnail);

    /// @brief Counts the reads the loader still owes.
    /// @return The number of queued or running jobs for the current folder.
    [[nodiscard]] int pendingLoads() const noexcept {
        return pending_;
    }

signals:
    /// @brief Announces that the loader has nothing left to do.
    void loadingFinished();

    /// @brief Announces that a sidecar was re-read because its file changed since it was last seen.
    ///
    /// Not sent for the first reading of a shot. The marks of the row are
    /// already updated. Whether the change was the application's own is for
    /// ::arraw::app::FolderWatcher to tell.
    /// @param primary Path of the shot's primary file.
    /// @param stamp ::arraw::app::fileStamp of the sidecar now, 0 when it is gone.
    void sidecarRefreshed(const QString& primary, qint64 stamp);

private:
    /// @brief One shot and what is known of it.
    struct Row {
        /// The shot itself.
        Shot shot;
        /// Marks, defaults until the sidecar is read.
        PhotoMarks marks{};
        /// Whether the sidecar was read, or the marks were set.
        bool marksLoaded = false;
        /// Whether the marks were set by hand, which a pending reading must not undo.
        bool marksSet = false;
        /// Stamp of the sidecar when it was last read; 0 when it was not there.
        std::int64_t sidecarStamp = 0;
        /// Whether the sidecar file was there when last looked at, by a reading or by a write.
        bool sidecarExists = false;
        /// Whether the sidecar is known to exist.
        [[nodiscard]] bool hasSidecar() const noexcept {
            return marksLoaded && sidecarExists;
        }
        /// EXIF of the primary, once read.
        std::optional<ExifInfo> exif{};
        /// Thumbnail, null until one is set.
        QImage thumbnail{};
    };

    class Loader;
    struct Loaded;

    /// @brief Takes a result from the loader thread.
    /// @param result What it read.
    void receive(const Loaded& result);

    /// @brief Names a shot by its path below the folder when it lives in a subfolder.
    /// @param shot Shot to name.
    /// @return The relative path, or an empty one for a shot in the folder itself.
    [[nodiscard]] std::filesystem::path shownName(const Shot& shot) const;

    /// @brief Rows of one folder.
    struct Span {
        /// First row.
        int first = 0;
        /// Number of rows.
        int count = 0;
    };

    /// @brief Orders folders as ::arraw::folderPrecedes does.
    struct FolderOrder {
        bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
            return folderPrecedes(a, b);
        }
    };

    struct FolderPlan;

    /// @brief Finds the spans of the rows as they are.
    /// @return The span of each folder with rows.
    [[nodiscard]] std::map<std::filesystem::path, Span, FolderOrder> spansOfRows() const;

    /// @brief Points the path index of the rows from a row on at their places.
    /// @param from First row that moved.
    void reindexFrom(int from);

    /// @brief Replaces all rows by a listing, keeping the data of the shots that stay.
    /// @param listing Shots in folder order.
    /// @param added Receives the primaries that were not rows before.
    void resetTo(const std::vector<Shot>& listing, std::vector<std::filesystem::path>& added);

    /// @brief Applies the diff of each folder with a signal per run.
    /// @param plans One per folder of the old and the new rows, in folder order.
    /// @param added Receives the primaries that were not rows before.
    void applyPlans(std::vector<FolderPlan>& plans, std::vector<std::filesystem::path>& added);

    /// @brief Queues the first reads of shots.
    /// @param primaries Shots that were just added.
    void queueLoads(const std::vector<std::filesystem::path>& primaries);

    /// @brief Announces a changed row.
    /// @param row Row that changed.
    /// @param roles Roles that did.
    void changed(int row, const QList<int>& roles);

    /// Folder shown.
    std::filesystem::path folder_;

    /// Whether the subfolders are listed too.
    bool recursive_ = false;

    /// Rows, folder by folder, and in natural order of their names within a folder.
    std::vector<Row> rows_;

    /// Rows of each folder that has any; folders in order, and each span follows the one before.
    std::map<std::filesystem::path, Span, FolderOrder> spans_;

    /// Row of each primary.
    std::unordered_map<std::filesystem::path, int> index_;

    /// Reads still owed for the current folder.
    int pending_ = 0;

    /// Folder generation; results of another one are stale.
    std::uint64_t generation_ = 0;

    /// Declared last, so that the thread stops before anything it hands results to is gone.
    std::unique_ptr<Loader> loader_;
};

} // namespace arraw::app
