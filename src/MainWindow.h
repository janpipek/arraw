#pragma once
#include "ImagePipeline.h"
#include <atomic>
#include <memory>
#include <QMainWindow>
#include <QFutureWatcher>

class ImageViewport;
class AdjustmentPanel;
class ProofingPanel;
class ExifPanel;
class FilmStrip;
class QDockWidget;
class QUndoStack;
class QLabel;
class QToolButton;
class QToolBar;
class QActionGroup;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Called after show() with the command-line argument, if any.
    // Accepts a RAW file path or a directory.
    void openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private slots:
    void openFile();
    void saveAdjustments();
    void exportFile();
    void onLoadFinished();
    void onFullResNeeded();

private:
    void setupMenus();
    void setupDocks();
    void setupStatusBar();
    void setupToolbar();
    void syncToolActions();            // reflect viewport->activeTool() in the buttons
    void setToolsEnabled(bool on);     // image-dependent toolbar items
    void loadImage(const QString& path);
    void setLoadingState(bool loading);
    void updateZoomStatus(float zoom);

    // Rebuild the viewport's display LUT from the current soft-proofing
    // settings and monitor profile (no LUT when both are off).
    void rebuildDisplayLut();

    ImageViewport*   viewport;
    AdjustmentPanel* adjPanel;
    ProofingPanel*   proofPanel;
    ExifPanel*       exifPanel;
    FilmStrip*       filmStrip;
    QDockWidget*     filmStripDock;
    QUndoStack*      undoStack;
    QLabel*          statusLabel;
    QLabel*          proofLabel;
    QToolButton*     zoomButton;

    // Toolbar: modal tools (left) + immediate actions (right).
    QActionGroup*    toolGroup;
    QAction*         cropAction;
    QAction*         straightenAction;
    QAction*         wbAction;
    QAction*         saveAction;
    QAction*         exportAction;

    QString monitorProfilePath;   // empty = assume sRGB

    ImageBuffer fullRes;
    ImageBuffer preview;
    QString     currentPath;

    std::shared_ptr<std::atomic<bool>> loadCancel;
    QFutureWatcher<LoadResult>         loadWatcher;
};
