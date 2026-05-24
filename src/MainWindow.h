#pragma once
#include "ImagePipeline.h"
#include <atomic>
#include <memory>
#include <QMainWindow>
#include <QFutureWatcher>

class ImageViewport;
class AdjustmentPanel;
class ExifPanel;
class FileBrowser;
class QDockWidget;
class QUndoStack;
class QLabel;
class QToolButton;

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
    void loadImage(const QString& path);
    void setLoadingState(bool loading);
    void updateZoomStatus(float zoom);

    ImageViewport*   viewport;
    AdjustmentPanel* adjPanel;
    ExifPanel*       exifPanel;
    FileBrowser*     fileBrowser;
    QDockWidget*     filmStripDock;
    QUndoStack*      undoStack;
    QLabel*          statusLabel;
    QToolButton*     zoomButton;

    ImageBuffer fullRes;
    ImageBuffer preview;
    QString     currentPath;

    std::shared_ptr<std::atomic<bool>> loadCancel;
    QFutureWatcher<LoadResult>         loadWatcher;
};
