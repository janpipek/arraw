#pragma once

#include "FilmStripRules.h"
#include "FolderLister.h"
#include "FolderWatcher.h"
#include "ShotFilterModel.h"
#include "ShotModel.h"
#include "ThumbnailCache.h"
#include "ThumbnailWorker.h"

#include <PhotoMarks.h>

#include <QElapsedTimer>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <vector>

class QComboBox;
class QEvent;
class QLabel;
class QListView;
class QMenu;
class QToolButton;

namespace arraw::app {

class FilmStripDelegate;

/// @brief Lists the keys of Refresh Folder: F5 on every platform and desktop, then the
/// platform's own `QKeySequence::Refresh` bindings (such as Ctrl+R or the keyboard's Refresh key).
/// @return The keys, F5 first, without repeats.
[[nodiscard]] QList<QKeySequence> refreshFolderKeys();

/// @brief Strip of the shots of a folder, to cull and to pick the photograph to develop.
///
/// A view over a ::arraw::app::ShotModel and a ::arraw::app::ShotFilterModel that
/// also keeps the selection and the *active* shot, the one the develop view shows.
/// The strip never opens a photograph itself: a click or a step asks for one with
/// ::arraw::app::FilmStrip::activationRequested, and the window answers with
/// ::arraw::app::FilmStrip::setActive, or with nothing when the user cancels, so the
/// active shot and the selection stay as they were. Selecting with Ctrl or Shift
/// changes the selection and nothing else.
///
/// Marks are not written by the strip's own code path but through a writer the window
/// supplies (see ::arraw::app::FilmStrip::setMarksWriter), since only it knows which
/// photograph has a session. The filter is the session's and is not saved.
class FilmStrip : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(FilmStrip)
public:
    /// @brief Writes the marks of one shot, and throws what it cannot write.
    using MarksWriter = std::function<void(const std::filesystem::path&, const PhotoMarks&)>;

    /// @brief Builds an empty strip with its title bar, and starts its thumbnail worker.
    /// @param parent Owning widget.
    /// @param cache Where thumbnails are kept; the application's cache directory by default.
    /// @param lensDatabase Gives the lens database for thumbnails of photographs with lens
    /// corrections on, called on the thumbnail thread; empty for the process's own.
    explicit FilmStrip(QWidget* parent = nullptr,
                       ThumbnailCache cache = ThumbnailCache(ThumbnailCache::defaultRoot()),
                       LensDatabaseSource lensDatabase = {});

    /// @brief Stops watching the folder.
    ~FilmStrip() override;

    /// @brief Returns the title bar, to give to the dock that holds the strip.
    ///
    /// The folder button, the "Include subfolders" toggle, the folder's name, and the filter
    /// controls. Owned by the strip until a dock takes it.
    /// @return The widget.
    [[nodiscard]] QWidget* titleBar() const noexcept {
        return titleBar_;
    }

    /// @brief Shows the shots of a folder, with none of them active.
    ///
    /// The folder is only checked here; its shots arrive as a walk on another thread reads them
    /// (see ::arraw::app::FilmStrip::isListing).
    /// @param folder Folder to list.
    /// @param activate A file (a primary or a companion) whose shot becomes active when the
    /// listing brings it, without ::arraw::app::FilmStrip::activationRequested: the photograph
    /// is open already. Nothing becomes active if the listing ends without it.
    /// @throws std::runtime_error if the folder cannot be opened; the strip is left as it was.
    void setFolder(const std::filesystem::path& folder,
                   std::optional<std::filesystem::path> activate = std::nullopt);

    /// @brief Tells whether the first listing of the folder is under way.
    /// @return True from ::arraw::app::FilmStrip::setFolder until the walk has read everything.
    [[nodiscard]] bool isListing() const noexcept {
        return model_.isListing();
    }

    /// @brief Tells whether a refresh of the folder is under way.
    /// @return True from ::arraw::app::FilmStrip::refreshFolder until its walk has ended.
    [[nodiscard]] bool isRefreshing() const noexcept {
        return model_.isRefreshing();
    }

    /// @brief Sets a function the lister calls on its thread before it reads each directory.
    ///
    /// A seam for tests, which block in it to hold a listing at a known point.
    /// @param hook Function, or an empty one for none.
    void setListingHook(FolderLister::ReadHook hook) {
        watcher_.setReadHook(std::move(hook));
    }

    /// @brief Sets whether the strip lists the subfolders of its folder.
    ///
    /// Updates the "Include subfolders" button. For a folder that is shown this lists it again,
    /// keeping the active shot if the listing brings it again (it is active again once it
    /// arrives), and announces
    /// ::arraw::app::FilmStrip::includeSubfoldersToggled. A folder that cannot be read leaves the
    /// strip as it was. The flag applies to every folder set afterwards.
    /// @param include Whether to include the subfolders.
    void setIncludeSubfolders(bool include);

    /// @brief Sets the question asked before the button drops the subfolders.
    ///
    /// Asked when the user switches "Include subfolders" off while the active shot lies in a
    /// subfolder, which would leave the strip. Answering false cancels the switch: the button
    /// is checked again and the strip stays as it is. Without a guard the switch goes ahead.
    /// @param guard Returns whether to go ahead.
    void setSubfoldersLeaveGuard(std::function<bool()> guard) {
        subfoldersLeaveGuard_ = std::move(guard);
    }

    /// @brief Tells whether the strip lists the subfolders.
    /// @return The flag; off by default.
    [[nodiscard]] bool includeSubfolders() const noexcept {
        return includeSubfolders_;
    }

    /// @brief Names the folder shown.
    /// @return The folder, empty before ::arraw::app::FilmStrip::setFolder.
    [[nodiscard]] const std::filesystem::path& folder() const noexcept {
        return model_.folder();
    }

    /// @brief Makes a shot the active one and the only one selected.
    ///
    /// Scrolls it into the middle of the strip. A shot the filter hides clears the
    /// filter first: the photograph is open, so it must be seen.
    /// @param primary Primary file of a shot of this folder. An unknown one is ignored, unless
    /// the folder is still being listed: then no shot is active or selected until the shot that
    /// holds it, as its primary or a companion, arrives and becomes active (as with
    /// ::arraw::app::FilmStrip::setFolder's file to activate, which this replaces).
    void setActive(const std::filesystem::path& primary);

    /// @brief Leaves no shot active, and none selected, and none waiting to be.
    void clearActive();

    /// @brief Names the active shot.
    /// @return Its primary file, or nothing before one is activated.
    [[nodiscard]] std::optional<std::filesystem::path> activePrimary() const;

    /// @brief Finds the shot a file belongs to.
    /// @param file A file of this folder, as the primary or as a companion.
    /// @return The shot's primary file, or nothing.
    [[nodiscard]] std::optional<std::filesystem::path>
    shotContaining(const std::filesystem::path& file) const;

    /// @brief Finds the first shot the filter shows.
    /// @return Its primary file, or nothing when the strip shows none.
    [[nodiscard]] std::optional<std::filesystem::path> firstVisible() const;

    /// @brief Returns the marks of the active shot.
    /// @return The marks the strip knows, defaults without an active shot.
    [[nodiscard]] PhotoMarks activeMarks() const;

    /// @brief Lists the shots that marks act on.
    /// @return The selected shots in strip order, or the active one if none is selected;
    /// empty without either.
    [[nodiscard]] std::vector<std::filesystem::path> targets() const;

    /// @brief Sets the function that writes marks.
    /// @param writer It must write the sidecar of the shot's primary file, and throw on failure.
    void setMarksWriter(MarksWriter writer);

    /// @brief Tells the watcher that the application wrote a photograph's sidecar.
    /// @param photo The photograph whose sidecar was just written.
    void noteOwnWrite(const std::filesystem::path& photo);

    /// @brief Sets the rating of the target shots.
    /// @param rating 0 to 5, or -1 for a reject.
    void rate(int rating);

    /// @brief Toggles a colour label on the target shots (see ::arraw::app::toggledLabel).
    /// @param label Label of the key.
    void toggleLabel(ColorLabel label);

    /// @brief Gives the target shots one colour label, or none.
    /// @param label Label to set; empty removes it.
    void setLabel(std::optional<ColorLabel> label);

    /// @brief Asks for the shot a step from the active one among the shots shown.
    ///
    /// Emits ::arraw::app::FilmStrip::activationRequested; nothing happens at either end.
    /// @param delta Steps, -1 for the previous shot and 1 for the next.
    void navigate(int delta);

    /// @brief Shows a thumbnail that follows the edits of the open photograph.
    ///
    /// Not cached: it may show what is not saved. While a shot has a live thumbnail the worker's
    /// results for it are dropped, so they cannot undo the edits it shows. It lasts until
    /// ::arraw::app::FilmStrip::releaseLiveThumbnail or a new folder.
    /// @param primary Primary file of the shot; an unknown one is ignored.
    /// @param thumbnail Image to show, at most ::arraw::app::ThumbnailCache::maxEdge on a side.
    void setLiveThumbnail(const std::filesystem::path& primary, QImage thumbnail);

    /// @brief Ends the live thumbnail, and brings back the one of the saved settings.
    ///
    /// Asks the worker for the shot's developed thumbnail again, which the cache usually has.
    void releaseLiveThumbnail();

    /// @brief Gives the thumbnail a shot's cell shows: the camera's preview, the developed one or
    /// a live one.
    /// @param primary Primary file of the shot.
    /// @return The thumbnail, or a null image for an unknown shot or one without a thumbnail yet.
    [[nodiscard]] QImage thumbnail(const std::filesystem::path& primary) const;

    /// @brief Tells the strip that a shot's settings were saved, so that its cached thumbnail is
    /// made afresh.
    /// @param primary Primary file of the shot.
    void noteSettingsSaved(const std::filesystem::path& primary);

    /// @brief Gives the filter that is applied.
    /// @return The filter; one that is not active shows everything.
    [[nodiscard]] const MarksFilter& filter() const noexcept {
        return proxy_.filter();
    }

    /// @brief Switches the filter off and resets its controls.
    void clearFilter();

    /// @brief Sets the most folders watched for changes at once, from the next listing on.
    /// @param limit Cap; see ::arraw::app::restoreMaxWatchedFolders.
    void setMaxWatchedFolders(int limit);

    /// @brief Walks the folder again in the background and merges what it finds, for the changes
    /// no watch reported (a folder beyond the limit on watches, a sidecar rewritten in place).
    ///
    /// The selection and the active shot stay, and every sidecar is checked. The folder label
    /// says so while it runs. Does nothing before a folder is shown, and while a listing or a
    /// refresh is under way (that walk takes the request in).
    void refreshFolder();

    /// @brief Tells whether some folders are not watched, so that the notice shows.
    /// @return True while the title bar's notice is visible.
    [[nodiscard]] bool hasWatchNotice() const;

    /// @brief Suggests a size for the dock.
    /// @return A strip as high as main's initial one.
    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// @brief Announces that the strip was given a folder (not a change of the subfolder flag).
    void folderChanged();

    /// @brief Asks the window to make a shot the active one.
    /// @param primary Primary file of the shot.
    void activationRequested(const QString& primary);

    /// @brief Announces that the subfolders are now included or not.
    /// @param include The new flag.
    void includeSubfoldersToggled(bool include);

    /// @brief Announces that the folder button was pressed.
    void folderRequested();

    /// @brief Asks for the active shot to be exported, from its context menu.
    void exportRequested();

    /// @brief Announces that the active shot changed.
    void activeChanged();

    /// @brief Announces that another program changed a shot's sidecar.
    /// @param primary Primary file of the shot; its marks are already refreshed.
    void sidecarChangedExternally(const QString& primary);

    /// @brief Announces the first shot the strip shows of a folder being listed.
    ///
    /// Sent at most once per ::arraw::app::FilmStrip::setFolder (or change of the subfolder
    /// flag), when the listing brings the first shot the filter lets through; after the shot
    /// waiting to be active, if it came in the same batch, has become active.
    /// @param primary Primary file of the shot.
    void firstShotListed(const QString& primary);

    /// @brief Announces that the first listing of a folder is complete.
    ///
    /// Sent from the event loop after the listing ended, so after its
    /// ::arraw::app::FilmStrip::firstShotListed, if any; not sent for a listing that a later
    /// ::arraw::app::FilmStrip::setFolder (or change of the subfolder flag) replaced.
    void listingEnded();

protected:
    /// @brief Handles clicks, context menus and the size of the cells.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Builds the title bar.
    void buildTitleBar();

    /// @brief Lists a folder in the model and resets what depends on the rows.
    /// @param folder Folder to list.
    /// @param activate File whose shot to make active when it arrives.
    void listFolder(const std::filesystem::path& folder,
                    std::optional<std::filesystem::path> activate);

    /// @brief Shows the folder's name, a note when its subfolders are included, and one while
    /// it is listed.
    void updateFolderLabel();

    /// @brief Takes rows the model inserted: thumbnails, the shot waiting to be active, and the
    /// first shot shown.
    /// @param first First row inserted.
    /// @param last Last row inserted.
    void takeInsertedRows(int first, int last);

    /// @brief Adds the "Refresh Folder" action to a context menu.
    /// @param menu Menu to add it to.
    void addRefreshAction(QMenu& menu);

    /// @brief Takes the watch status of the folder: shows or hides the notice.
    /// @param status What is watched and what is not.
    void onWatchStatus(const WatchStatus& status);

    /// @brief Shows the notice for the unwatched folders, or hides it.
    void updateNotice();

    /// @brief Follows the start and the end of a listing.
    /// @param listing Whether a listing is under way now.
    void onListingChanged(bool listing);

    /// @brief Follows the start and the end of a refresh: the busy note says so.
    /// @param refreshing Whether a refresh is under way now.
    void onRefreshingChanged(bool refreshing);

    /// @brief Takes a thumbnail from the worker.
    void receiveThumbnail(const ThumbnailResult& result);

    /// @brief Tells the worker which shots are on screen.
    void reportVisible();

    /// @brief Builds the filter's controls into the title bar.
    /// @param into The title bar.
    void buildFilterControls(QWidget* into);

    /// @brief Rebuilds the filter from the controls and applies it.
    void applyFilter();

    /// @brief Makes the cells as high as the strip allows.
    void updateCellSide();

    /// @brief Shows or hides the note on an empty strip: listing, no photographs, or the filter.
    void updateEmptyHint();

    /// @brief Handles a press of the left button on a cell.
    /// @param index Cell under the pointer.
    /// @param modifiers Keyboard modifiers of the press.
    void clickCell(const QModelIndex& index, Qt::KeyboardModifiers modifiers);

    /// @brief Lists the shots a click on a cell would act on.
    /// @param index Cell that was clicked.
    /// @return The selection if it holds the cell, else the cell alone.
    [[nodiscard]] std::vector<std::filesystem::path> contextTargets(const QModelIndex& index) const;

    /// @brief Opens the context menu of a cell.
    /// @param position Position in the strip's viewport.
    void showContextMenu(const QPoint& position);

    /// @brief Sets one rating on shots.
    void rateShots(const std::vector<std::filesystem::path>& shots, int rating);

    /// @brief Toggles a label on shots.
    void toggleLabelOn(const std::vector<std::filesystem::path>& shots, ColorLabel label);

    /// @brief Sets or removes the label of shots.
    void setLabelOn(const std::vector<std::filesystem::path>& shots,
                    std::optional<ColorLabel> label);

    /// @brief Reads the marks of shots as they stand, the sidecar's where the model has not read
    /// it.
    [[nodiscard]] std::vector<PhotoMarks>
    currentMarks(const std::vector<std::filesystem::path>& shots) const;

    /// @brief Writes new marks for shots and shows them.
    ///
    /// Those that change are written; the ones that fail are reported together, once.
    /// @param shots Shots to change.
    /// @param edit Gives the marks a shot should have from the marks it has.
    void applyMarks(const std::vector<std::filesystem::path>& shots,
                    const std::function<PhotoMarks(const PhotoMarks&)>& edit);

    /// @brief Looks again, soon, for the active shot being hidden by the filter.
    void scheduleActiveCheck();

    /// @brief Asks for the nearest shown shot when the active one is filtered out.
    void ensureActiveShown();

    /// @brief Finds the shown index of a shot.
    [[nodiscard]] QModelIndex proxyIndexOf(const std::filesystem::path& primary) const;

    /// @brief Finds the primary file at a shown index.
    [[nodiscard]] static std::filesystem::path primaryAt(const QModelIndex& index);

    /// @brief What a model reset that merges a refresh must not lose.
    struct ResetStash {
        /// Shots selected.
        std::vector<std::filesystem::path> selected;
        /// Shot with the focus, empty for none.
        std::filesystem::path current;
        /// First shot in view, empty for none.
        std::filesystem::path anchor;
    };

    /// @brief Remembers the selection, the current shot and the scroll position before a reset.
    void stashSelection();

    /// @brief Puts back what ::arraw::app::FilmStrip::stashSelection remembered.
    void restoreSelection();

    ShotModel model_;
    ShotFilterModel proxy_;
    FolderWatcher watcher_;
    QListView* list_ = nullptr;
    FilmStripDelegate* delegate_ = nullptr;
    QLabel* emptyHint_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QLabel* folderLabel_ = nullptr;
    QToolButton* subfoldersButton_ = nullptr;
    /// Title bar's notice of folders that are not watched; hidden when all are.
    QToolButton* noticeButton_ = nullptr;
    /// Watch status of the folder, all zeros before its walk ends.
    WatchStatus watchStatus_;
    bool includeSubfolders_ = false;
    /// Whether the model is being given a new folder, which a reset must not carry a selection
    /// into.
    bool changingFolder_ = false;
    /// Taken when the model starts a reset that merges a refresh; empty otherwise.
    std::optional<ResetStash> stash_;
    std::function<bool()> subfoldersLeaveGuard_;
    QComboBox* starChoice_ = nullptr;
    std::vector<std::pair<QToolButton*, ColorLabel>> swatches_;
    QToolButton* clearButton_ = nullptr;
    MarksWriter writer_;
    std::filesystem::path active_;
    /// File whose shot becomes active once the listing under way brings it.
    std::optional<std::filesystem::path> pendingActive_;
    /// Whether ::arraw::app::FilmStrip::firstShotListed was sent for the listing under way.
    bool firstShotSent_ = false;
    /// Number of the listing under way, so that a queued first shot of an earlier one is dropped.
    std::uint64_t listingNumber_ = 0;
    /// Time since the listing or the refresh under way began.
    QElapsedTimer listingClock_;
    /// Updates the folder label's busy note while a listing or a refresh runs.
    QTimer busyTimer_;
    QTimer activeCheck_;
    bool updatingCells_ = false;
    /// Whether the left button went down on a cell, so that its release is ours too.
    bool pressHandled_ = false;
    /// Shot whose thumbnail follows the edits, empty for none.
    std::filesystem::path live_;
    /// Waits for scrolling to pause before the worker is told what is on screen.
    QTimer visibleTimer_;
    /// Declared last, so that the thread stops before what it hands results to is gone.
    ThumbnailWorker thumbnails_;
};

} // namespace arraw::app
