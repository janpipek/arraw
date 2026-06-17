#pragma once
#include "UserMetadata.h"
#include <QString>
#include <QWidget>

class QListView;
class FilmStripModel;
class ThumbnailCache;
class QModelIndex;
class QImage;

// Horizontal thumbnail strip for one directory. A thin shell over FilmStripModel
// + QListView; the testable logic lives in FilmStripModel / FilmStripLayout.
// Public API mirrors the former FileBrowser so MainWindow wiring is unchanged.
class FilmStrip : public QWidget {
    Q_OBJECT
public:
    explicit FilmStrip(QWidget* parent = nullptr);

    void setDirectory(const QString& dir);
    void setCurrentFile(const QString& path);
    void selectFirst();

    // Replace a row's thumbnail with a freshly developed one. No-op if the path
    // is not in the current directory listing.
    void setThumbnail(const QString& path, const QImage& image);

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

    QString directory() const { return currentDir; }

    // Modest default height so the dock doesn't inherit QListView's tall hint.
    QSize sizeHint() const override { return {600, 132}; }

public slots:
    // Ask the user for a directory, then load it. Used by the strip's folder
    // button and File ▸ Open Folder.
    void promptForDirectory();

signals:
    void fileSelected(const QString& path);
    void directoryChanged(const QString& dir);

protected:
    // Watches the list viewport for resizes (fired after its geometry is final,
    // unlike this widget's own resizeEvent) to keep cell height in sync.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void requestVisibleThumbnails();
    void updateThumbHeight();
    bool handleMarkKey(int key);              // maps a culling key to a mark; false if not one
    void loadMarks(const QStringList& paths); // background sidecar scan
    void setRating(const QString& path, int rating);
    void setLabel(const QString& path, ColourLabel label); // toggles off if already set
    void applyMarks(const QString& path, const UserMetadata& marks);
    void showContextMenu(const QPoint& pos);
    QString currentPath() const;

    QListView* list;
    FilmStripModel* model;
    ThumbnailCache* thumbs;
    QString currentDir;

    static QStringList scanImageFiles(const QString& dir);
};
