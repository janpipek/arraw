#include "MainWindow.h"
#include "ImageViewport.h"
#include "AdjustmentPanel.h"
#include "ExifPanel.h"
#include "FileBrowser.h"
#include "RawProcessor.h"
#include "StandardImageLoader.h"
#include "ThumbnailCache.h"
#include "XmpSidecar.h"
#include "ExportDialog.h"
#include <QMenuBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QStatusBar>
#include <QLabel>
#include <QToolButton>
#include <QMessageBox>
#include <QScrollArea>
#include <QTabWidget>
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
#include <algorithm>
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
    setupStatusBar();

    connect(&loadWatcher, &QFutureWatcher<LoadResult>::finished,
            this, &MainWindow::onLoadFinished);

    connect(viewport, &ImageViewport::fullResNeeded,
            this, &MainWindow::onFullResNeeded);

    connect(viewport, &ImageViewport::zoomChanged,
            this, &MainWindow::updateZoomStatus);

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

    connect(adjPanel, &AdjustmentPanel::straightenActive,
            viewport, &ImageViewport::setStraightenActive);

    // Restore window geometry
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("windowState").toByteArray());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue("geometry",    saveGeometry());
    s.setValue("windowState", saveState());
    QString lastDir = fileBrowser->directory();
    if (!lastDir.isEmpty()) {
        s.setValue("lastDir", lastDir);
    }
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
    view->addAction(filmStripDock->toggleViewAction());
    view->addSeparator();
    view->addAction("Reset Zoom", Qt::CTRL | Qt::Key_0,
                    viewport, &ImageViewport::resetView);
}

void MainWindow::setupStatusBar() {
    statusLabel = new QLabel("No image loaded", this);
    statusBar()->addWidget(statusLabel, 1);

    zoomButton = new QToolButton(this);
    zoomButton->setPopupMode(QToolButton::InstantPopup);
    zoomButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    zoomButton->setAutoRaise(true);

    auto* zoomMenu = new QMenu(zoomButton);
    zoomMenu->addAction("Fit", viewport, &ImageViewport::resetView);
    zoomMenu->addSeparator();
    zoomMenu->addAction("50 %", this, [this] { viewport->setPixelZoom(0.5f); });
    zoomMenu->addAction("100 %", this, [this] { viewport->setPixelZoom(1.0f); });
    zoomMenu->addAction("200 %", this, [this] { viewport->setPixelZoom(2.0f); });
    zoomButton->setMenu(zoomMenu);

    statusBar()->addPermanentWidget(zoomButton);
    updateZoomStatus(viewport->pixelZoom());
}

void MainWindow::setupDocks() {
    // Film strip (left)
    filmStripDock = new QDockWidget("Film Strip", this);
    filmStripDock->setObjectName("FilmStripDock");
    filmStripDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    filmStripDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    fileBrowser = new FileBrowser(filmStripDock);
    fileBrowser->setMinimumWidth(148);
    filmStripDock->setWidget(fileBrowser);
    addDockWidget(Qt::LeftDockWidgetArea, filmStripDock);

    auto* toggleFilmStrip = filmStripDock->toggleViewAction();
    toggleFilmStrip->setText("Film Strip");
    toggleFilmStrip->setShortcut(Qt::Key_F9);

    connect(fileBrowser, &FileBrowser::fileSelected,
            this, &MainWindow::loadImage);

    // Adjustments + EXIF (right)
    auto* rightDock = new QDockWidget("Adjustments", this);
    rightDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    rightDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* tabs = new QTabWidget(rightDock);
    tabs->setMinimumWidth(280);

    auto* adjScroll = new QScrollArea(tabs);
    adjPanel = new AdjustmentPanel(adjScroll);
    adjScroll->setWidget(adjPanel);
    adjScroll->setWidgetResizable(true);
    adjScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addTab(adjScroll, "Adjustments");

    exifPanel = new ExifPanel(tabs);
    tabs->addTab(exifPanel, "EXIF");

    rightDock->setWidget(tabs);
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
        this, "Open Image", startDir,
        "All Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2 *.pef *.srw "
                    "*.jpg *.jpeg *.png *.tiff *.tif *.webp *.bmp);;"
        "RAW Images (*.cr2 *.cr3 *.nef *.arw *.dng *.raf *.orf *.rw2 *.pef *.srw);;"
        "Standard Images (*.jpg *.jpeg *.png *.tiff *.tif *.webp *.bmp);;"
        "All Files (*)");
    if (!path.isEmpty())
        loadImage(path);
}

void MainWindow::loadImage(const QString& path) {
    // Cancel any in-progress load so it stops before dcraw_process.
    if (loadCancel)
        loadCancel->store(true);
    loadCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = loadCancel;

    currentPath = path;
    exifPanel->clear();
    viewport->setOriginalImageSize(0, 0);
    setLoadingState(true);

    // Populate browser from the file's directory
    fileBrowser->setDirectory(QFileInfo(path).absolutePath());
    fileBrowser->setCurrentFile(path);

    // Sync: show cached thumbnail immediately while the background task runs
    if (QImage cached = ThumbnailCache::loadFromDisk(path); !cached.isNull())
        viewport->setImage(srgbToLinearBuffer(cached));

    // Single background task: extract embedded preview on the same open file
    // handle (unpack_thumb only), dispatch it to the main thread, then
    // continue with the full demosaic.  Sequential I/O avoids contention.
    loadWatcher.setFuture(
        QtConcurrent::run([this, path, cancel]() -> LoadResult {
            auto onPreview = [this, path, cancel](ImageBuffer buf) {
                if (cancel->load()) return;
                QMetaObject::invokeMethod(this,
                    [this, path, buf = std::move(buf)]() mutable {
                        if (currentPath == path)
                            viewport->setImage(buf);
                    }, Qt::QueuedConnection);
            };
            if (StandardImageLoader::canLoad(path))
                return StandardImageLoader::load(path, cancel);
            return RawProcessor::load(path, std::move(onPreview), cancel);
        })
    );
}

void MainWindow::onLoadFinished() {
    LoadResult result = loadWatcher.result();

    // Empty result means the task was cancelled — another load is already running.
    if (!result.fullRes.valid() && result.error.isEmpty())
        return;

    setLoadingState(false);

    if (!result.error.isEmpty()) {
        QMessageBox::critical(this, "Load Error", result.error);
        statusLabel->setText("Load failed.");
        exifPanel->clear();
        return;
    }

    fullRes = std::move(result.fullRes);
    preview = std::move(result.preview);

    viewport->setOriginalImageSize(fullRes.width, fullRes.height);
    viewport->setImage(preview, true);
    adjPanel->setHistogramImage(preview);
    exifPanel->setMetadata(result.metadata);
    undoStack->clear();

    AdjustmentParams saved = XmpSidecar::load(currentPath);
    if (!QFileInfo::exists(XmpSidecar::pathFor(currentPath)))
        saved.cropRect = result.defaultCrop;
    adjPanel->setParams(saved);

    statusLabel->setText(QString("%1  —  %2 × %3")
        .arg(QFileInfo(currentPath).fileName())
        .arg(fullRes.width).arg(fullRes.height));
}

void MainWindow::onFullResNeeded() {
    if (fullRes.valid())
        viewport->setFullResImage(fullRes);
}

void MainWindow::updateZoomStatus(float zoom) {
    zoomButton->setVisible(zoom > 0.0f);
    if (zoom <= 0.0f)
        return;
    zoomButton->setText(QString("%1 %").arg(qRound(zoom * 100.0f)));
}

void MainWindow::setLoadingState(bool loading) {
    menuBar()->setEnabled(!loading);
    adjPanel->setEnabled(!loading);
    exifPanel->setEnabled(!loading);
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

// Simple unsharp mask: radius ≈ 1% of image width, amount 0..100.
static QImage applyUnsharpMask(QImage img, int amount) {
    if (amount == 0) return img;
    const float a = amount / 100.0f;
    int r = std::max(1, img.width() / 100);
    QImage blurred = img.scaled(img.width()  / (r + 1),
                                img.height() / (r + 1),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .scaled(img.width(), img.height(),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < img.height(); ++y) {
        const uchar* src = img.constScanLine(y);
        const uchar* blr = blurred.constScanLine(y);
        uchar* dst = img.scanLine(y);
        for (int x = 0; x < img.width() * 3; ++x) {
            int v = int(src[x]) + int(a * (int(src[x]) - int(blr[x])));
            dst[x] = uchar(std::clamp(v, 0, 255));
        }
    }
    return img;
}

void MainWindow::exportFile() {
    if (!fullRes.valid()) {
        QMessageBox::information(this, "Export", "No image loaded.");
        return;
    }

    const AdjustmentParams p = adjPanel->params();

    // Natural output size = full-res pixels inside the crop rect
    const int naturalW = int(fullRes.width  * p.cropRect.width()  + 0.5);
    const int naturalH = int(fullRes.height * p.cropRect.height() + 0.5);

    ExportDialog optDlg(naturalW, naturalH, this);
    if (optDlg.exec() != QDialog::Accepted)
        return;

    const ExportOptions opts = optDlg.options();

    QString suffix, filter;
    switch (opts.format) {
    case ExportOptions::Format::JPEG: suffix = "jpg";  filter = "JPEG (*.jpg *.jpeg)"; break;
    case ExportOptions::Format::PNG:  suffix = "png";  filter = "PNG (*.png)";         break;
    case ExportOptions::Format::TIFF: suffix = "tif";  filter = "TIFF (*.tif *.tiff)"; break;
    }

    QFileDialog fileDlg(this, "Export Image", QFileInfo(currentPath).absolutePath());
    fileDlg.setAcceptMode(QFileDialog::AcceptSave);
    fileDlg.setNameFilter(filter);
    fileDlg.setDefaultSuffix(suffix);
    if (fileDlg.exec() != QDialog::Accepted)
        return;

    QString path = fileDlg.selectedFiles().constFirst();
    if (QFileInfo(path).suffix().isEmpty())
        path += "." + suffix;

    statusLabel->setText("Exporting…");
    QApplication::processEvents();

    QImage out = viewport->renderToImage(fullRes, p, opts.width, opts.height);
    out = applyUnsharpMask(std::move(out), opts.sharpening);

    bool ok;
    if (opts.format == ExportOptions::Format::JPEG)
        ok = out.save(path, "JPEG", opts.quality);
    else
        ok = out.save(path);

    if (!ok)
        QMessageBox::critical(this, "Export Error", "Failed to save " + path);
    else
        statusLabel->setText("Exported: " + QFileInfo(path).fileName());
}
