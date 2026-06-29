#pragma once
#include "develop/UserMetadata.h"
#include "ui/FilmStripFilter.h"
#include <functional>
#include <QColor>
#include <QSet>
#include <QString>
#include <QWidget>

// Swatch colour for a label (transparent for None). Shared by the strip's cell
// delegate and the title-bar filter swatches so the palette is defined once
// (ADR 0042).
QColor labelColour(ColourLabel label);

class QListView;
class QLabel;
class FilmStripModel;
class FilmStripFilterModel;
class ThumbnailCache;
class QModelIndex;
class QImage;
class QHelpEvent;
class QMouseEvent;
class QMenu;

/**
 * Horizontal thumbnail strip for the open directory.
 *
 * FilmStrip is a thin QWidget shell over FilmStripModel, QListView, thumbnail
 * loading, and selection gestures. It owns directory-level browser state and may
 * write culling marks for non-active files; when the active file changes,
 * DevelopSession remains the canonical owner of that image's metadata.
 */
class FilmStrip : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(FilmStrip)
public:
    explicit FilmStrip(QWidget* parent = nullptr);

    void setDirectory(const QString& dir);
    void setCurrentFile(const QString& path);
    void selectFirst();

    // Gate run before any user gesture (click or arrow key) moves off the active
    // image. The guard returns true to allow the move, false to veto it; a veto
    // swallows the gesture so the view never leaves the current image. MainWindow
    // wires this to its unsaved-changes prompt so "Cancel" keeps the image put.
    void setLeaveGuard(std::function<bool()> guard);

    // Apply the rating/colour viewing filter (ADR 0042). Hides non-matching
    // cells; if the active image stops matching, jumps to the nearest remaining
    // match. Session-sticky: survives setDirectory, never persisted.
    void setFilter(const FilmStripFilter& filter);
    FilmStripFilter filter() const;

    // Replace a row's thumbnail with a freshly developed one. No-op if the path
    // is not in the current directory listing.
    void setThumbnail(const QString& path, const QImage& image);

    // Persist + display a freshly developed thumbnail asynchronously: the caller
    // renders the linear working-space image on the GUI thread, the encode and
    // disk write happen on a worker, and the strip updates via thumbnailReady
    // once the write is the current generation (docs/adr/0045).
    void cacheDevelopedThumbnail(const QString& path, const QImage& linearWorkingImage);
    void setMarks(const QString& path, const UserMetadata& marks);

    // Navigate ±1 from current selection. Returns false if already at boundary.
    bool navigateBy(int delta);

    // Culling marks for the current item (write-through to its sidecar, and the
    // model). rateCurrent(0) clears; labelCurrent(l) toggles l off if already l.
    // No-op when nothing is selected.
    void rateCurrent(int rating);
    void labelCurrent(ColourLabel label);

    // Marks of the current item (defaults if nothing is selected). Lets a menu
    // reflect the current file's rating/label.
    UserMetadata currentMarks() const;

    // All file paths currently in the selection (may be more than one when
    // the user has Ctrl/Shift-clicked). The active file (currentPath) is
    // always a member of this list when any selection exists.
    QStringList selectedPaths() const;

    QString directory() const { return currentDir; }

    // Modest default height so the dock doesn't inherit QListView's tall hint.
    QSize sizeHint() const override { return {600, 132}; }

public slots:
    // Ask the user for a directory, then load it. Used by the strip's folder
    // button and File ▸ Open Folder.
    void promptForDirectory();

signals:
    // Emitted when the active image changes (last single-clicked item).
    // Always triggers image load in MainWindow.
    void fileSelected(const QString& path);
    // Emitted whenever the multi-selection changes (add/remove with Ctrl/Shift).
    // Batch operations (paste, export) read this to find their target set.
    void selectionChanged(const QStringList& paths);
    void populateContextMenu(const QString& path, const QStringList& targets, QMenu* menu);
    void directoryChanged(const QString& dir);
    void marksChanged(const QString& path, const UserMetadata& marks, bool saved);

protected:
    // Watches the list viewport for resizes (fired after its geometry is final,
    // unlike this widget's own resizeEvent) to keep cell height in sync.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void requestVisibleThumbnails();
    void updateThumbHeight();
    bool handleMarkKey(int key); // maps a culling key to a mark; false if not one
    bool handleTooltip(QHelpEvent* event);
    void requestTooltipMetadata(const QString& path, const QPoint& globalPos);
    // Ctrl/Shift-click only changes the batch selection, never the active image.
    // Returns true when it handled the press (so the view doesn't move current).
    bool handleModifierClick(QMouseEvent* e);
    // Asks the leave-guard whether the active image may be left now. True (allow)
    // when no guard is set, so non-GUI callers and tests navigate freely.
    bool mayLeaveCurrent();
    void loadMarks(const QStringList& paths); // background sidecar scan
    void setRating(const QString& path, int rating);
    void setLabel(const QString& path, ColourLabel label); // toggles off if already set
    void applyMarks(const QString& path, const UserMetadata& marks);
    QStringList contextTargetPaths(const QString& path) const;
    void showContextMenu(const QPoint& pos);
    QString currentPath() const;

    // Selects the nearest still-visible row to a source row, biased forward
    // (ADR 0042 jump/auto-advance). No-op if nothing is visible.
    void selectNearestVisible(int sourceRow);
    // Shows the "no shots match" overlay when an active filter empties the strip.
    void updateEmptyHint();

    QListView* list;
    FilmStripModel* model;       // source: holds every file in the directory
    FilmStripFilterModel* proxy; // hides rows that don't match the filter
    QLabel* emptyHint;           // overlay shown when the filter hides everything
    ThumbnailCache* thumbs;
    std::function<bool()> leaveGuard; // veto for leaving the active image; see setLeaveGuard
    QString currentDir;
    QSet<QString> metadataPending;
    // Guards against re-entrant resize: changing the cell height relays the view,
    // which can toggle the horizontal scrollbar, which resizes the viewport again
    // (see updateThumbHeight). Without this the resize→layout→resize loop recurses
    // until the stack overflows.
    bool updatingThumbHeight = false;

    static QStringList scanImageFiles(const QString& dir);
};
