#pragma once
#include "ImagePipeline.h"
#include <QMainWindow>
#include <QFutureWatcher>

class ImageViewport;
class AdjustmentPanel;
class ExifPanel;
class FileBrowser;
class QUndoStack;
class QLabel;

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
    void loadImage(const QString& path);
    void setLoadingState(bool loading);

    ImageViewport*   viewport;
    AdjustmentPanel* adjPanel;
    ExifPanel*       exifPanel;
    FileBrowser*     fileBrowser;
    QUndoStack*      undoStack;
    QLabel*          statusLabel;

    ImageBuffer fullRes;
    ImageBuffer preview;
    QString     currentPath;

    QFutureWatcher<LoadResult> loadWatcher;
};
