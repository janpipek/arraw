#pragma once
#include "ImagePipeline.h"
#include <atomic>
#include <memory>
#include <QMainWindow>
#include <QFutureWatcher>

class ImageViewport;
class AdjustmentPanel;
class LocalAdjustmentPanel;
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
    void setupImageMenu();
    void setupDocks();
    void setupStatusBar();
    void setupToolbar();
    void syncToolActions();            // reflect viewport->activeTool() in the buttons
    void setToolsEnabled(bool on);     // image-dependent toolbar items
    void loadImage(const QString& path);
    void setLoadingState(bool loading);
    void updateZoomStatus(float zoom);

    // The full develop params = global edits (adjPanel) + local adjustments
    // (localPanel) merged into one GlobalAdjustment for render, save, export.
    GlobalAdjustment currentParams() const;
    // Feed currentParams() to the viewport (after a global or local change).
    void pushParamsToViewport();

    // Rebuild the viewport's display LUT from the current soft-proofing
    // settings and monitor profile (no LUT when both are off).
    void rebuildDisplayLut();

    // Push the clipping-overlay actions' state to the viewport and persist it
    // (docs/adr/0009); toggleClipping flips both at once for the J key.
    void applyClipping();
    void toggleClipping();

    ImageViewport*        viewport;
    AdjustmentPanel*      adjPanel;
    LocalAdjustmentPanel* localPanel;
    ProofingPanel*        proofPanel;
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
    QAction*         clipHighlightsAction;   // View → Show Highlight Clipping
    QAction*         clipShadowsAction;      // View → Show Shadow Clipping

    QString monitorProfilePath;   // empty = assume sRGB

    ImageBuffer fullRes;
    ImageBuffer preview;
    QString     currentPath;

    std::shared_ptr<std::atomic<bool>> loadCancel;
    QFutureWatcher<LoadResult>         loadWatcher;
};
