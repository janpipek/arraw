#pragma once
#include "ImagePipeline.h"
#include <QMainWindow>
#include <QFutureWatcher>

class ImageViewport;
class AdjustmentPanel;
class QDockWidget;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void openFile();
    void saveAdjustments();
    void exportFile();
    void onLoadFinished();

private:
    void setupMenus();
    void setupDocks();
    void loadImage(const QString& path);
    void setLoadingState(bool loading);

    ImageViewport*   viewport;
    AdjustmentPanel* adjPanel;
    QLabel*          statusLabel;

    ImageBuffer fullRes;    // export only
    ImageBuffer preview;    // viewport + histogram
    QString     currentPath;

    QFutureWatcher<LoadResult> loadWatcher;
};
