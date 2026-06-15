#include "MainWindow.h"
#include "ColorManagement.h"
#include "ImageViewport.h"
#include "AdjustmentPanel.h"
#include "ProofingPanel.h"
#include "ExifPanel.h"
#include "FilmStrip.h"
#include "RawProcessor.h"
#include "StandardImageLoader.h"
#include "ThumbnailCache.h"
#include "XmpSidecar.h"
#include "ExportDialog.h"
#include <QActionGroup>
#include <QAction>
#include <QToolBar>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QMenuBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QStatusBar>
#include <QLabel>
#include <QToolButton>
#include <QMessageBox>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStyle>
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

    monitorProfilePath = QSettings().value("display/monitorProfile").toString();

    setupDocks();
    setupMenus();
    setupStatusBar();
    setupToolbar();

    connect(proofPanel, &ProofingPanel::proofingChanged,
            this, &MainWindow::rebuildDisplayLut);
    rebuildDisplayLut();

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

    connect(viewport, &ImageViewport::rotationCommitted,
            this, [this](float degrees) {
                AdjustmentParams before = adjPanel->params();
                AdjustmentParams after  = before;
                after.rotation = degrees;
                if (after != before)
                    undoStack->push(new AdjustmentCommand(adjPanel, before, after));
            });

    connect(viewport, &ImageViewport::whiteBalanceCommitted,
            this, [this](float kelvin, float tint) {
                AdjustmentParams before = adjPanel->params();
                AdjustmentParams after  = before;
                after.temperature = kelvin;
                after.tint        = tint;
                if (after != before)
                    undoStack->push(new AdjustmentCommand(adjPanel, before, after));
            });

    connect(viewport, &ImageViewport::activeToolChanged,
            this, [this](ImageViewport::ActiveTool) { syncToolActions(); });

    connect(adjPanel, &AdjustmentPanel::adjustmentCommitted,
            this, [this](const AdjustmentParams& before, const AdjustmentParams& after) {
                undoStack->push(new AdjustmentCommand(adjPanel, before, after));
            });

    connect(adjPanel, &AdjustmentPanel::paramsChanged,
            viewport, &ImageViewport::setAdjustments);

    connect(viewport, &ImageViewport::histogramsReady,
            adjPanel, &AdjustmentPanel::setHistogramSamples);

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
    QString lastDir = filmStrip->directory();
    if (!lastDir.isEmpty()) {
        s.setValue("lastDir", lastDir);
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Left)       filmStrip->navigateBy(-1);
    else if (e->key() == Qt::Key_Right) filmStrip->navigateBy(+1);
    else if (e->key() == Qt::Key_S && e->modifiers() == Qt::NoModifier)
        proofPanel->setProofingEnabled(!proofPanel->proofingEnabled());
    else QMainWindow::keyPressEvent(e);
}

void MainWindow::setupMenus() {
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&Open...",          QKeySequence::Open,    this, &MainWindow::openFile);
    file->addAction("Open &Folder...",   QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
                    filmStrip, &FilmStrip::promptForDirectory);
    file->addSeparator();
    file->addAction("&Save Adjustments", QKeySequence::Save,    this, &MainWindow::saveAdjustments);
    file->addAction("&Export...",        Qt::CTRL | Qt::Key_E,  this, &MainWindow::exportFile);
    file->addSeparator();
    file->addAction("&Quit", QKeySequence::Quit, qApp, &QCoreApplication::quit);

    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction(undoStack->createUndoAction(this));
    edit->addAction(undoStack->createRedoAction(this));

    auto* view = menuBar()->addMenu("&View");
    view->addAction(filmStripDock->toggleViewAction());
    view->addSeparator();
    view->addAction("Reset Zoom", Qt::CTRL | Qt::Key_0,
                    viewport, &ImageViewport::resetView);
    view->addSeparator();

    // Monitor profile: how the preview is encoded for this screen.
    // "sRGB" keeps the fast in-shader path; a profile switches to the LUT path.
    auto* monitorMenu = view->addMenu("Monitor Profile");
    auto* monitorGroup = new QActionGroup(this);
    monitorGroup->setExclusive(true);

    auto addMonitorAction = [&](const QString& text, const QString& path) {
        QAction* a = monitorMenu->addAction(text);
        a->setCheckable(true);
        a->setActionGroup(monitorGroup);
        a->setChecked(path == monitorProfilePath);
        connect(a, &QAction::triggered, this, [this, path] {
            monitorProfilePath = path;
            QSettings().setValue("display/monitorProfile", path);
            rebuildDisplayLut();
        });
        return a;
    };

    addMonitorAction("sRGB (assume)", QString())->setChecked(monitorProfilePath.isEmpty());
    for (const IccProfileInfo& info : scanSystemProfiles())
        if (info.isDisplayClass)
            addMonitorAction(info.description, info.path);
}

void MainWindow::setupStatusBar() {
    statusLabel = new QLabel("No image loaded", this);
    statusBar()->addWidget(statusLabel, 1);

    proofLabel = new QLabel(this);
    proofLabel->setVisible(false);
    statusBar()->addPermanentWidget(proofLabel);

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

void MainWindow::setupToolbar() {
    auto* tb = new QToolBar("Tools", this);
    tb->setObjectName("ToolsToolBar");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::TopToolBarArea, tb);

    // Modal tools (left): mutually exclusive, but all may be off — clicking the
    // active tool again deselects it. The viewport owns the actual tool state.
    toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    auto addTool = [&](const QString& text, const QKeySequence& sc) {
        QAction* a = tb->addAction(text);
        a->setCheckable(true);
        a->setActionGroup(toolGroup);
        if (!sc.isEmpty()) a->setShortcut(sc);
        return a;
    };
    cropAction       = addTool("Crop",        Qt::Key_C);
    straightenAction = addTool("Straighten",  {});
    wbAction         = addTool("White Bal.",  {});

    connect(toolGroup, &QActionGroup::triggered, this, [this](QAction* a) {
        using T = ImageViewport::ActiveTool;
        T t = T::None;
        if (a->isChecked())
            t = a == cropAction       ? T::Crop
              : a == straightenAction ? T::Straighten
                                      : T::WhiteBalance;
        viewport->setActiveTool(t);
    });

    // Spacer pushes the action group to the right edge.
    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    // Immediate actions (right): reuse the existing slots; Open stays usable
    // with no image loaded, the rest are image-dependent.
    tb->addAction("Open", this, &MainWindow::openFile);
    saveAction   = tb->addAction("Save",   this, &MainWindow::saveAdjustments);
    exportAction = tb->addAction("Export", this, &MainWindow::exportFile);

    setToolsEnabled(false);
}

void MainWindow::syncToolActions() {
    const ImageViewport::ActiveTool t = viewport->activeTool();
    // setChecked doesn't emit QActionGroup::triggered, but block toggled too.
    const QSignalBlocker b1(cropAction), b2(straightenAction), b3(wbAction);
    cropAction->setChecked(      t == ImageViewport::ActiveTool::Crop);
    straightenAction->setChecked(t == ImageViewport::ActiveTool::Straighten);
    wbAction->setChecked(        t == ImageViewport::ActiveTool::WhiteBalance);
}

void MainWindow::setToolsEnabled(bool on) {
    cropAction->setEnabled(on);
    straightenAction->setEnabled(on);
    wbAction->setEnabled(on);
    saveAction->setEnabled(on);
    exportAction->setEnabled(on);
}

void MainWindow::setupDocks() {
    // Film strip (bottom): a horizontal thumbnail strip under the viewport.
    filmStripDock = new QDockWidget("Film Strip", this);
    // New object name so a window state saved with the old left-side dock
    // doesn't restore the strip to the side.
    filmStripDock->setObjectName("FilmStripDockBottom");
    filmStripDock->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    filmStripDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    filmStrip = new FilmStrip(filmStripDock);
    filmStrip->setMinimumHeight(80);
    filmStripDock->setWidget(filmStrip);
    addDockWidget(Qt::BottomDockWidgetArea, filmStripDock);
    resizeDocks({filmStripDock}, {132}, Qt::Vertical);  // sensible initial height

    // Custom title bar: folder icon + current path, instead of "Film Strip".
    auto* stripTitle = new QWidget(filmStripDock);
    auto* stripTitleLayout = new QHBoxLayout(stripTitle);
    stripTitleLayout->setContentsMargins(6, 2, 6, 2);
    stripTitleLayout->setSpacing(6);

    auto* folderButton = new QToolButton(stripTitle);
    folderButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    folderButton->setAutoRaise(true);
    folderButton->setToolTip("Open folder…");
    connect(folderButton, &QToolButton::clicked, filmStrip, &FilmStrip::promptForDirectory);
    stripTitleLayout->addWidget(folderButton);

    auto* pathLabel = new QLabel("No folder", stripTitle);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stripTitleLayout->addWidget(pathLabel, 1);
    filmStripDock->setTitleBarWidget(stripTitle);

    connect(filmStrip, &FilmStrip::directoryChanged, this, [pathLabel](const QString& dir) {
        const QString native = QDir::toNativeSeparators(dir);
        pathLabel->setText(native);
        pathLabel->setToolTip(native);
    });

    auto* toggleFilmStrip = filmStripDock->toggleViewAction();
    toggleFilmStrip->setText("Film Strip");
    toggleFilmStrip->setShortcut(Qt::Key_F9);

    connect(filmStrip, &FilmStrip::fileSelected,
            this, &MainWindow::loadImage);

    // Adjustments + EXIF (right)
    auto* rightDock = new QDockWidget("Adjustments", this);
    rightDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    rightDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* tabs = new QTabWidget(rightDock);
    tabs->setMinimumWidth(120);   // let the panel follow its content; don't pin it wide

    auto* adjScroll = new QScrollArea(tabs);
    adjPanel   = new AdjustmentPanel;
    proofPanel = new ProofingPanel;
    auto* adjColumn = new QWidget(adjScroll);
    auto* adjLayout = new QVBoxLayout(adjColumn);
    adjLayout->setContentsMargins(0, 0, 0, 0);
    adjLayout->addWidget(adjPanel);
    adjLayout->addWidget(proofPanel);
    adjScroll->setWidget(adjColumn);
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
        filmStrip->setDirectory(fi.absoluteFilePath());
        filmStrip->selectFirst();
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
    viewport->cancelActiveTool();   // discard any in-progress tool from the last image
    viewport->setOriginalImageSize(0, 0);
    setLoadingState(true);

    // Populate the strip from the file's directory
    filmStrip->setDirectory(QFileInfo(path).absolutePath());
    filmStrip->setCurrentFile(path);

    // Sync: show cached thumbnail immediately while the background task runs
    if (QImage cached = ThumbnailCache::loadFromDisk(path); !cached.isNull())
        viewport->setImage(toWorkingSpaceBuffer(cached));

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
    exifPanel->setMetadata(result.metadata);
    undoStack->clear();

    AdjustmentParams saved = XmpSidecar::load(currentPath);
    if (!QFileInfo::exists(XmpSidecar::pathFor(currentPath)))
        saved.cropRect = result.defaultCrop;
    adjPanel->setParams(saved);

    statusLabel->setText(QString("%1  —  %2 × %3")
        .arg(QFileInfo(currentPath).fileName())
        .arg(fullRes.width).arg(fullRes.height));

    setToolsEnabled(true);
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
    if (loading)
        setToolsEnabled(false);   // re-enabled in onLoadFinished on success
    statusLabel->setText(loading
        ? QString("Loading %1...").arg(QFileInfo(currentPath).fileName())
        : QString());
}

void MainWindow::rebuildDisplayLut() {
    const bool proofing = proofPanel->proofingEnabled();
    if (!proofing && monitorProfilePath.isEmpty()) {
        viewport->clearDisplayLut();
        viewport->setGamutWarning(false);
        proofLabel->setVisible(false);
        return;
    }

    const DisplayLut lut = buildDisplayLut(
        proofing ? proofPanel->profilePath() : QString(),
        proofPanel->intent(), proofPanel->blackPointCompensation(),
        monitorProfilePath);
    if (!lut.valid()) {
        QMessageBox::warning(this, "Color Management",
            "Could not build the display transform — check the selected ICC profiles.");
        viewport->clearDisplayLut();
        viewport->setGamutWarning(false);
        proofLabel->setVisible(false);
        return;
    }

    viewport->setDisplayLut(lut);
    viewport->setGamutWarning(proofing && proofPanel->gamutWarning());
    proofLabel->setText(proofing ? "Proofing: " + proofPanel->profileName() : QString());
    proofLabel->setVisible(proofing);
}

void MainWindow::saveAdjustments() {
    if (currentPath.isEmpty()) return;
    viewport->commitActiveTool();   // fold any pending crop into the params first
    if (XmpSidecar::save(currentPath, adjPanel->params()))
        statusLabel->setText("Saved: " + XmpSidecar::pathFor(currentPath));
    else
        QMessageBox::warning(this, "Save Error",
            "Could not write " + XmpSidecar::pathFor(currentPath));
}

// Simple unsharp mask: radius ≈ 1% of image width, amount 0..100.
// The blur is approximated by a smooth downscale + upscale round-trip,
// which is much cheaper than a real Gaussian at these radii.
// Runs in encoded (output-profile) space — Format_RGB888 or Format_RGBA64.
static QImage applyUnsharpMask(QImage img, int amount) {
    if (amount == 0) return img;
    const float a = amount / 100.0f;
    int r = std::max(1, img.width() / 100);
    QImage blurred = img.scaled(img.width()  / (r + 1),
                                img.height() / (r + 1),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .scaled(img.width(), img.height(),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (img.format() == QImage::Format_RGBA64) {
        for (int y = 0; y < img.height(); ++y) {
            const auto* src = reinterpret_cast<const quint16*>(img.constScanLine(y));
            const auto* blr = reinterpret_cast<const quint16*>(blurred.constScanLine(y));
            auto* dst = reinterpret_cast<quint16*>(img.scanLine(y));
            for (int x = 0; x < img.width() * 4; ++x) {
                if ((x & 3) == 3) continue;   // leave alpha alone
                int v = int(src[x]) + int(a * (int(src[x]) - int(blr[x])));
                dst[x] = quint16(std::clamp(v, 0, 65535));
            }
        }
        return img;
    }
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

    viewport->commitActiveTool();   // fold any pending crop into the params first
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
    QApplication::processEvents();  // repaint the status bar before the render blocks the UI

    // Linear working-space render → output profile (lcms2) → sharpen → save.
    QImage out = viewport->renderToImage(fullRes, p, opts.width, opts.height);
    out = toOutputImage(out, opts.profile, opts.bitDepth == 16);
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
