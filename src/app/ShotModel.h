#pragma once

#include "FolderResult.h"

#include <ExifInfo.h>
#include <PhotoMarks.h>
#include <Shot.h>

#include <QAbstractListModel>
#include <QImage>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arraw::app {

struct FolderDiff;

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
/// The model reads no directory itself (but for the check that a new folder opens): its rows
/// come from walks of the folder that a ::arraw::app::FolderWatcher runs on a thread of its own
/// and hands over directory by directory (::arraw::app::ShotModel::applyFolders), so a tree of
/// fifty thousand shots does not block the window. The model asks for a walk with
/// ::arraw::app::ShotModel::walkRequested, and numbers each one; batches of another walk are
/// stale and dropped. After its walk, the same number stamps each directory the watcher reads
/// again because it changed: that directory's result replaces its rows, a new subfolder comes as
/// a walk of its own subtree, and one that went is removed with its subtree in one go
/// (::arraw::app::ShotModel::removeSubtree). Opening no file is part of listing (ADR 029). The
/// marks of each sidecar and the EXIF of each photograph are read afterwards on one background
/// thread, marks of every shot before any EXIF, and handed back to this thread. EXIF is read by
/// that loader rather than on the first tooltip request: a tooltip is drawn on the GUI thread,
/// which must not open a RAW. Changing the folder drops what the loader has not done and ignores
/// what it is still doing.
///
/// The rows are kept as one contiguous span per folder, folders in ::arraw::folderPrecedes
/// order, which is the order a walk comes in, so the rows of a first listing are only ever
/// appended. Each directory a walk reads replaces its span: the model changes by inserting and
/// removing runs of rows rather than by resetting, so views keep their selection; only a change
/// too scattered for that, past ::arraw::app::maxFolderRuns runs in a folder, resets it.
///
/// What a walk streams in is queued (::arraw::app::ShotModel::queueFolders) and applied from the
/// event loop a slice at a time, about ::arraw::app::ShotModel::applyBudget of work per turn,
/// so that neither a folder of fifty thousand shots nor thousands of tiny folders stall the
/// window. Rows that go to one place are inserted together, at most
/// ::arraw::app::ShotModel::maxInsertRows at a time: the new folders of a batch share one insert,
/// and a large one is inserted in parts.
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

    /// @brief Starts to show the shots of a folder.
    ///
    /// Checks that the folder opens, empties the model with a reset, and starts the first
    /// listing of it: ::arraw::app::ShotModel::isListing turns true, and
    /// ::arraw::app::ShotModel::walkRequested asks for the walk whose results fill the rows.
    /// Anything the loader was doing for the previous folder is dropped, and so is the walk of
    /// it that may still be under way.
    /// @param folder Folder to list.
    /// @param recursive Whether to list its subfolders too (see ::arraw::listShots). Setting the
    /// folder again resets the model like a new folder does; to change only this flag without
    /// a reset, see ::arraw::app::ShotModel::setRecursive.
    /// @throws std::runtime_error (a std::filesystem::filesystem_error) if the folder cannot be
    /// opened; the model is left as it was.
    void setFolder(const std::filesystem::path& folder, bool recursive = false);

    /// @brief Includes the subfolders of the folder, or drops them, without a reset.
    ///
    /// Under a number of its own (the walk under way is abandoned, and what is queued from it
    /// dropped), asked for with ::arraw::app::ShotModel::walkRequested. The rows of the folder
    /// itself stay, with their marks, EXIF and thumbnails, and so do the loader's reads owed to
    /// them. Switching on, the walk reads the root once more (its listing is where the
    /// subfolders are found), merges it, and brings the subfolders in after it;
    /// ::arraw::app::ShotModel::isListing is true meanwhile, also when it replaces a switch off
    /// whose walk had not ended. Switching off removes all the rows of the subfolders with one
    /// signal at once, and drops the loader's reads still queued for them (they are queued again
    /// when the subfolders come back); the walk (one read of the root) only has the watches of
    /// the subfolders released, and no listing is shown for it. Nothing changes if the flag is
    /// the one set, or no folder is.
    /// @param recursive Whether to list the subfolders.
    void setRecursive(bool recursive);

    /// @brief Numbers the walk that results are wanted from.
    /// @return The number of the last walk asked for, 0 before any.
    [[nodiscard]] std::uint64_t walk() const noexcept {
        return walk_;
    }

    /// @brief Tells whether the first listing of the folder is under way.
    /// @return True from ::arraw::app::ShotModel::setFolder until its walk is finished.
    [[nodiscard]] bool isListing() const noexcept {
        return walking_ == Walking::Listing;
    }

    /// @brief Tells whether a refresh walk is under way.
    /// @return True from ::arraw::app::ShotModel::refresh until its walk is finished; a directory
    /// read again after a change is no refresh.
    [[nodiscard]] bool isRefreshing() const noexcept {
        return walking_ == Walking::Refresh;
    }

    /// @brief Most rows one insert brings.
    static constexpr std::size_t maxInsertRows = 2000;

    /// @brief Work done on queued results in one turn of the event loop, before the rest waits
    /// for the next turn.
    static constexpr std::chrono::milliseconds applyBudget{12};

    /// @brief Queues a batch of a walk, to be applied from the event loop.
    ///
    /// Applied as ::arraw::app::ShotModel::applyFolders does, in the order queued, a slice of
    /// ::arraw::app::ShotModel::applyBudget per turn of the event loop. A batch of a walk that is
    /// no longer the last one asked for when its turn comes is dropped.
    /// @param walk Number of the walk.
    /// @param batch Directories read, in the order read.
    void queueFolders(std::uint64_t walk, std::vector<FolderResult> batch);

    /// @brief Queues the end of a walk, to be taken once the batches before it are applied.
    ///
    /// Taken as ::arraw::app::ShotModel::finishListing does.
    /// @param walk Number of the walk.
    /// @param summary What it saw.
    void queueFinish(std::uint64_t walk, ListingSummary summary);

    /// @brief Takes a batch of a walk at once: each directory's shots replace its rows.
    ///
    /// Whatever is queued is applied first. A directory that has no rows yet gets its span where
    /// the folder order puts it, sharing one insert with the new directories next to it (up to
    /// ::arraw::app::ShotModel::maxInsertRows rows each); one that has rows is merged with them in
    /// linear time (see
    /// ::arraw::app::diffFolder): rows of shots that are still there stay, with their marks,
    /// EXIF and thumbnail; shots that went are removed and new ones inserted, in order, one
    /// insert or remove per run of adjacent rows, so a view's selection follows its shot. A
    /// shot whose companions changed is updated in place. If a directory needs more than
    /// ::arraw::app::maxFolderRuns runs, the model is reset instead (rows still keep their
    /// data), and views must restore their selection.
    ///
    /// A directory marked ::arraw::app::FolderResult::gone is removed with its subtree, as
    /// ::arraw::app::ShotModel::removeSubtree does.
    ///
    /// New rows are queued for their marks and EXIF. The loaded rows of a merged directory are
    /// queued for a check of their sidecars as well when the directory is marked
    /// ::arraw::app::FolderResult::recheck, when its rows changed, or in a refresh of all (see
    /// ::arraw::app::ShotModel::refresh); the rows of other directories are not.
    /// @param walk Number of the walk; a batch of any but the last one asked for is dropped. It
    /// is taken after the walk has finished too: the directories read again after a change.
    /// @param batch Directories read, in the order read.
    void applyFolders(std::uint64_t walk, const std::vector<FolderResult>& batch);

    /// @brief Removes the rows of a folder and of every folder below it, with one signal.
    ///
    /// For a subfolder that went, or can no longer be read. Whatever is queued is applied first.
    /// @param walk Number of the walk; any but the last one asked for is ignored.
    /// @param folder Folder, without a trailing separator; one without rows changes nothing.
    void removeSubtree(std::uint64_t walk, const std::filesystem::path& folder);

    /// @brief Takes the end of a walk at once.
    ///
    /// Whatever is queued is applied first. The rows of every folder the walk did not read (a
    /// folder that went, or could not be read) are removed. Then the walk is over:
    /// ::arraw::app::ShotModel::listingChanged reports the end of a first listing, a refresh that
    /// was put off starts, and
    /// ::arraw::app::ShotModel::loadingFinished follows once the loader is done too.
    /// @param walk Number of the walk; any but the last one asked for is ignored.
    /// @param summary What it saw. The rows of the folders it lists as stale, and of everything
    /// below them, are kept.
    void finishListing(std::uint64_t walk, const ListingSummary& summary);

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

    /// @brief Asks for the folder to be walked again, and merged in folder by folder.
    ///
    /// The manual refresh of all, for what no watch reports. Asynchronous:
    /// ::arraw::app::ShotModel::walkRequested asks for a walk, whose batches are merged by
    /// ::arraw::app::ShotModel::applyFolders and whose end removes the folders it did not find
    /// (::arraw::app::ShotModel::finishListing). While a walk is under way (a first listing, which
    /// reads everything anyway, or another refresh) the call does nothing: it is folded into
    /// that walk, not queued as another.
    ///
    /// A call made while another walk runs is folded into that walk and does not start one. That
    /// walk then does not check the sidecars of the rows it already passed, and a folder it has
    /// read and does not watch (beyond the cap, see ::arraw::app::FolderWatcher) is not read
    /// again: what changed there after its read waits for the next refresh.
    ///
    /// The sidecar of every loaded row is checked on the loader thread, and re-read when the
    /// modification time is not the one last seen; ::arraw::app::ShotModel::sidecarRefreshed
    /// reports each of those. A folder that is gone, or refused for lack of permission, loses its
    /// rows at the walk's end; one that cannot be read for another reason (the root, or a share
    /// that does not answer) keeps them (see ::arraw::app::ListingSummary::stale).
    void refresh();

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
    /// @brief Announces that the loader has nothing left to do, and no walk is under way.
    void loadingFinished();

    /// @brief Asks for a walk of the folder.
    ///
    /// For ::arraw::app::ShotModel::folder, ::arraw::app::ShotModel::recursive and
    /// ::arraw::app::ShotModel::walk as they are when it is sent; a
    /// ::arraw::app::FolderWatcher answers it.
    void walkRequested();

    /// @brief Announces that a first listing of the folder began or ended.
    /// @param listing The new ::arraw::app::ShotModel::isListing.
    void listingChanged(bool listing);

    /// @brief Announces that a refresh walk began or ended (or was dropped for a new folder).
    /// @param refreshing The new ::arraw::app::ShotModel::isRefreshing.
    void refreshingChanged(bool refreshing);

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

    /// @brief Rows of each folder, in folder order.
    using Spans = std::map<std::filesystem::path, Span, FolderOrder>;

    /// @brief What a walk is for.
    enum class Walking {
        None,    ///< No walk is under way.
        Listing, ///< The first listing of the folder.
        Refresh, ///< A refresh, merged into the rows there are.
        Toggle,  ///< The walk of a switch of the subfolders off, silent.
    };

    struct Job;

    /// @brief A batch or the end of a walk, waiting to be applied.
    struct Incoming {
        /// Number of the walk.
        std::uint64_t walk = 0;
        /// Directories read; empty for an end.
        std::vector<FolderResult> batch;
        /// What the walk saw, for an end.
        std::optional<ListingSummary> end;
    };

    /// @brief Point in time a slice of queued work must stop at.
    using Deadline = std::chrono::steady_clock::time_point;

    /// @brief Applies queued results, in order, until the queue is empty or a deadline passed.
    ///
    /// At least one step is taken. What is left waits for the next turn of the event loop.
    /// @param deadline When to stop; nothing to apply everything.
    void applyQueued(std::optional<Deadline> deadline);

    /// @brief Drops the first queued item.
    void dropIncoming();

    /// @brief Applies the next step of the first queued batch: the merge of one directory that
    /// has rows, the removal of one that went, or one insert of new ones.
    /// @param added Receives the primaries that were not rows before.
    /// @param rechecks Receives the sidecar checks of the rows that stay.
    void applyStep(std::vector<std::filesystem::path>& added, std::vector<Job>& rechecks);

    /// @brief Inserts the rows of the next new directories of the first queued batch that go to
    /// one place, at most ::arraw::app::ShotModel::maxInsertRows, with one insert.
    ///
    /// The directories of one insert each follow the one before in folder order; one that does
    /// not (read twice in the batch, or sorting before the last) waits for the next call. A
    /// directory whose rows do not all fit is continued by the next call.
    /// @param added Receives the primaries inserted.
    void insertRun(std::vector<std::filesystem::path>& added);

    /// @brief Ends the walk under way: the rows of the folders it did not read go, but for those
    /// it could not read.
    /// @param summary What the walk saw.
    void endWalk(const ListingSummary& summary);

    /// @brief Checks, in a debug build, that the spans tile the rows in order.
    void checkSpans() const;

    /// @brief Points the path index of the rows from a row on at their places.
    /// @param from First row that moved.
    void reindexFrom(int from);

    /// @brief Moves the first row of the spans from one on.
    /// @param from First span to move.
    /// @param delta Rows to move them by.
    void shiftSpans(Spans::iterator from, int delta);

    /// @brief Replaces the rows of one folder that has rows by its shots now.
    /// @param folder Folder, without a trailing separator.
    /// @param fresh Its shots now.
    /// @param recheck Whether the sidecars of its rows are checked whatever the diff.
    /// @param added Receives the primaries that were not rows before.
    /// @param rechecks Receives the sidecar checks of the rows that stay.
    void mergeFolder(const std::filesystem::path& folder, std::span<const Shot> fresh, bool recheck,
                     std::vector<std::filesystem::path>& added, std::vector<Job>& rechecks);

    /// @brief Applies the diff of a folder with a signal per run.
    /// @param first First row of the folder.
    /// @param diff What turns its rows into @p fresh.
    /// @param fresh Its shots now.
    /// @param added Receives the primaries that were not rows before.
    void applyDiff(int first, const FolderDiff& diff, std::span<const Shot> fresh,
                   std::vector<std::filesystem::path>& added);

    /// @brief Replaces the rows of a folder with a reset, keeping the data of the shots that stay.
    /// @param span Its rows now.
    /// @param fresh Its shots now.
    /// @param added Receives the primaries that were not rows before.
    void resetFolder(Span span, std::span<const Shot> fresh,
                     std::vector<std::filesystem::path>& added);

    /// @brief Removes the rows of a folder and of its subtree, with one remove.
    /// @param folder Folder, without a trailing separator.
    void removeTree(const std::filesystem::path& folder);

    /// @brief Removes the rows of a run of spans, with one remove.
    /// @param first First span to remove.
    /// @param last Span after the last; erased with the run.
    void removeRange(Spans::iterator first, Spans::iterator last);

    /// @brief Announces the end of the loading, if nothing is owed and no walk is under way.
    void reportIfLoaded();

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
    Spans spans_;

    /// Row of each primary.
    std::unordered_map<std::filesystem::path, int> index_;

    /// Reads still owed for the current folder.
    int pending_ = 0;

    /// Folder generation, for the loader; results of another one are stale.
    std::uint64_t generation_ = 0;

    /// Number of the last walk asked for; batches of another one are stale.
    std::uint64_t walk_ = 0;

    /// What the walk under way is for.
    Walking walking_ = Walking::None;

    /// Folders the walk under way has read, spelled as span keys.
    std::unordered_set<std::filesystem::path> seen_;

    /// Batches and ends of walks not applied yet, in the order they came.
    std::deque<Incoming> incoming_;

    /// Directory of the first queued batch to apply next.
    std::size_t nextFolder_ = 0;

    /// Shots of that directory inserted already, when it is inserted in parts.
    std::size_t nextShot_ = 0;

    /// Timer that applies the next slice of the queue on the next turn of the event loop.
    QTimer applyTimer_;

    /// Declared last, so that the thread stops before anything it hands results to is gone.
    std::unique_ptr<Loader> loader_;
};

} // namespace arraw::app
