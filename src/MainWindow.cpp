#include "MainWindow.h"
#include "ImageViewport.h"
#include "AdjustmentPanel.h"
#include "RawProcessor.h"
#include "XmpSidecar.h"
#include <QMenuBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QStatusBar>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QFileInfo>
#include <QApplication>
#include <QtConcurrent/QtConcurrent>
#include <cmath>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("arraw");
    resize(1400, 900);

    viewport = new ImageViewport(this);
    setCentralWidget(viewport);

    setupDocks();
    setupMenus();

    statusLabel = new QLabel("No image loaded", this);
    statusBar()->addWidget(statusLabel);

    connect(&loadWatcher, &QFutureWatcher<LoadResult>::finished,
            this, &MainWindow::onLoadFinished);
}

void MainWindow::setupMenus() {
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&Open...",          this, &MainWindow::openFile,        QKeySequence::Open);
    file->addSeparator();
    file->addAction("&Save Adjustments", this, &MainWindow::saveAdjustments, QKeySequence::Save);
    file->addAction("&Export...",        this, &MainWindow::exportFile,      Qt::CTRL | Qt::Key_E);
    file->addSeparator();
    file->addAction("&Quit", qApp, &QCoreApplication::quit, QKeySequence::Quit);

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Reset Zoom", viewport, [this] {
        // TODO: expose zoom reset on viewport
    }, Qt::CTRL | Qt::Key_0);
}

void MainWindow::setupDocks() {
    auto* dock = new QDockWidget("Adjustments", this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* scroll = new QScrollArea(dock);
    adjPanel = new AdjustmentPanel(scroll);
    scroll->setWidget(adjPanel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(280);
    dock->setWidget(scroll);

    addDockWidget(Qt::RightDockWidgetArea, dock);

    connect(adjPanel, &AdjustmentPanel::paramsChanged,
            viewport, &ImageViewport::setAdjustments);
}

void MainWindow::openFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Open RAW Image", {},
        "RAW Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2 *.pef *.srw);;"
        "All Files (*)");
    if (!path.isEmpty())
        loadImage(path);
}

void MainWindow::loadImage(const QString& path) {
    if (loadWatcher.isRunning()) return;

    currentPath = path;
    setLoadingState(true);

    loadWatcher.setFuture(
        QtConcurrent::run([path]() -> LoadResult {
            return RawProcessor::load(path);
        })
    );
}

void MainWindow::onLoadFinished() {
    setLoadingState(false);
    LoadResult result = loadWatcher.result();

    if (!result.error.isEmpty()) {
        QMessageBox::critical(this, "Load Error", result.error);
        statusLabel->setText("Load failed.");
        return;
    }

    fullRes = std::move(result.fullRes);
    preview = std::move(result.preview);

    viewport->setImage(preview);
    adjPanel->setHistogramImage(preview);

    AdjustmentParams saved = XmpSidecar::load(currentPath);
    adjPanel->setParams(saved);

    statusLabel->setText(QString("%1  —  %2 × %3  (preview: %4 × %5)")
        .arg(QFileInfo(currentPath).fileName())
        .arg(fullRes.width).arg(fullRes.height)
        .arg(preview.width).arg(preview.height));
}

void MainWindow::setLoadingState(bool loading) {
    menuBar()->setEnabled(!loading);
    adjPanel->setEnabled(!loading);
    statusLabel->setText(loading
        ? QString("Loading %1...").arg(QFileInfo(currentPath).fileName())
        : QString());
}

void MainWindow::saveAdjustments() {
    if (currentPath.isEmpty()) return;
    if (XmpSidecar::save(currentPath, adjPanel->params()))
        statusLabel->setText("Saved: " + XmpSidecar::pathFor(currentPath));
    else
        QMessageBox::warning(this, "Save Error",
            "Could not write " + XmpSidecar::pathFor(currentPath));
}

void MainWindow::exportFile() {
    if (!fullRes.valid()) {
        QMessageBox::information(this, "Export", "No image loaded.");
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, "Export Image", {},
        "JPEG (*.jpg *.jpeg);;PNG (*.png);;TIFF (*.tif *.tiff)");
    if (path.isEmpty()) return;

    // CPU export: exposure + gamma 2.2 only. FBO readback (WYSIWYG) is next step.
    const int w = fullRes.width;
    const int h = fullRes.height;
    const AdjustmentParams p = adjPanel->params();

    QImage out(w, h, QImage::Format_RGB888);
    const float expMul = std::pow(2.0f, p.exposure);

    for (int y = 0; y < h; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int idx = (y * w + x) * 3;
            auto tonemap = [&](float v) -> uchar {
                v = std::clamp(v * expMul, 0.0f, 1.0f);
                return uchar(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f);
            };
            row[x * 3 + 0] = tonemap(fullRes.data[idx + 0]);
            row[x * 3 + 1] = tonemap(fullRes.data[idx + 1]);
            row[x * 3 + 2] = tonemap(fullRes.data[idx + 2]);
        }
    }

    if (!out.save(path))
        QMessageBox::critical(this, "Export Error", "Failed to save " + path);
    else
        statusLabel->setText("Exported: " + QFileInfo(path).fileName());
}
