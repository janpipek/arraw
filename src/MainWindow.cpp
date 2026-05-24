#include "MainWindow.h"
#include "ImageViewport.h"
#include "AdjustmentPanel.h"
#include "FileBrowser.h"
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
#include <QCloseEvent>
#include <QDir>
#include <QKeyEvent>
#include <QSettings>
#include <QUndoStack>
#include <QUndoCommand>
#include <QColorSpace>
#include <QtConcurrent/QtConcurrent>
#include <cmath>

// ---------------------------------------------------------------------------
// Undo command: captures before/after AdjustmentParams for a single gesture.
// ---------------------------------------------------------------------------
class AdjustmentCommand : public QUndoCommand {
public:
    AdjustmentCommand(AdjustmentPanel* panel,
                      const AdjustmentParams& before,
                      const AdjustmentParams& after)
        : panel(panel), before(before), after(after) {}

    void undo() override { panel->setParams(before); }
    void redo() override { panel->setParams(after);  }

private:
    AdjustmentPanel* panel;
    AdjustmentParams before, after;
};

// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("arraw");

    viewport    = new ImageViewport(this);
    undoStack   = new QUndoStack(this);
    setCentralWidget(viewport);

    setupDocks();
    setupMenus();

    statusLabel = new QLabel("No image loaded", this);
    statusBar()->addWidget(statusLabel);

    connect(&loadWatcher, &QFutureWatcher<LoadResult>::finished,
            this, &MainWindow::onLoadFinished);

    connect(viewport, &ImageViewport::fullResNeeded,
            this, &MainWindow::onFullResNeeded);

    connect(viewport, &ImageViewport::cropCommitted,
            this, [this](const QRectF& rect) {
                AdjustmentParams before = adjPanel->params();
                AdjustmentParams after  = before;
                after.cropRect = rect;
                undoStack->push(new AdjustmentCommand(adjPanel, before, after));
            });

    connect(adjPanel, &AdjustmentPanel::adjustmentCommitted,
            this, [this](const AdjustmentParams& before, const AdjustmentParams& after) {
                undoStack->push(new AdjustmentCommand(adjPanel, before, after));
            });

    connect(adjPanel, &AdjustmentPanel::paramsChanged,
            viewport, &ImageViewport::setAdjustments);

    // Restore window geometry
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("windowState").toByteArray());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue("geometry",    saveGeometry());
    s.setValue("windowState", saveState());
    s.setValue("lastDir",     QFileInfo(currentPath).absolutePath());
    QMainWindow::closeEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Left)       fileBrowser->navigateBy(-1);
    else if (e->key() == Qt::Key_Right) fileBrowser->navigateBy(+1);
    else QMainWindow::keyPressEvent(e);
}

void MainWindow::setupMenus() {
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&Open...",          this, &MainWindow::openFile,        QKeySequence::Open);
    file->addSeparator();
    file->addAction("&Save Adjustments", this, &MainWindow::saveAdjustments, QKeySequence::Save);
    file->addAction("&Export...",        this, &MainWindow::exportFile,      Qt::CTRL | Qt::Key_E);
    file->addSeparator();
    file->addAction("&Quit", qApp, &QCoreApplication::quit, QKeySequence::Quit);

    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction(undoStack->createUndoAction(this));
    edit->addAction(undoStack->createRedoAction(this));

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Reset Zoom", viewport, [this] {
        // TODO: expose zoom reset
    }, Qt::CTRL | Qt::Key_0);
}

void MainWindow::setupDocks() {
    // File browser (left)
    auto* leftDock = new QDockWidget("Files", this);
    leftDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    leftDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    fileBrowser = new FileBrowser(leftDock);
    fileBrowser->setMinimumWidth(180);
    leftDock->setWidget(fileBrowser);
    addDockWidget(Qt::LeftDockWidgetArea, leftDock);

    connect(fileBrowser, &FileBrowser::fileSelected,
            this, &MainWindow::loadImage);

    // Adjustments (right)
    auto* rightDock = new QDockWidget("Adjustments", this);
    rightDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    rightDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* scroll = new QScrollArea(rightDock);
    adjPanel = new AdjustmentPanel(scroll);
    scroll->setWidget(adjPanel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(280);
    rightDock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);
    // adjPanel → viewport paramsChanged wired in constructor (after both are created)
}

void MainWindow::openPath(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists()) return;

    if (fi.isDir()) {
        fileBrowser->setDirectory(fi.absoluteFilePath());
        fileBrowser->selectFirst();
    } else {
        loadImage(fi.absoluteFilePath());
    }
}

void MainWindow::openFile() {
    QSettings s;
    const QString startDir = s.value("lastDir", QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, "Open RAW Image", startDir,
        "RAW Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2 *.pef *.srw);;"
        "All Files (*)");
    if (!path.isEmpty())
        loadImage(path);
}

void MainWindow::loadImage(const QString& path) {
    if (loadWatcher.isRunning()) return;

    currentPath = path;
    setLoadingState(true);

    // Populate browser from the file's directory
    fileBrowser->setDirectory(QFileInfo(path).absolutePath());
    fileBrowser->setCurrentFile(path);

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
    undoStack->clear();

    AdjustmentParams saved = XmpSidecar::load(currentPath);
    adjPanel->setParams(saved);

    statusLabel->setText(QString("%1  —  %2 × %3")
        .arg(QFileInfo(currentPath).fileName())
        .arg(fullRes.width).arg(fullRes.height));
}

void MainWindow::onFullResNeeded() {
    if (fullRes.valid())
        viewport->setFullResImage(fullRes);
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
        this, "Export Image", QFileInfo(currentPath).absolutePath(),
        "JPEG (*.jpg *.jpeg);;PNG (*.png);;TIFF (*.tif *.tiff)");
    if (path.isEmpty()) return;

    // CPU export: exposure + gamma 2.2. FBO readback (WYSIWYG) is next milestone.
    const int w = fullRes.width;
    const int h = fullRes.height;
    const AdjustmentParams p = adjPanel->params();
    const float expMul = std::pow(2.0f, p.exposure);

    QImage out(w, h, QImage::Format_RGB888);
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

    out.setColorSpace(QColorSpace(QColorSpace::SRgb));

    if (!out.save(path))
        QMessageBox::critical(this, "Export Error", "Failed to save " + path);
    else
        statusLabel->setText("Exported: " + QFileInfo(path).fileName());
}
