#include "FileBrowser.h"
#include "ThumbnailCache.h"
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QTimer>

static const QStringList kRawExts = {
    "cr2","cr3","nef","arw","dng","raf","orf","rw2","pef","srw"
};

static const QStringList kSupportedExts = kRawExts + QStringList{
    "jpg","jpeg","png","tiff","tif","webp","bmp"
};

FileBrowser::FileBrowser(QWidget* parent) : QWidget(parent) {
    thumbs = new ThumbnailCache(this);
    connect(thumbs, &ThumbnailCache::thumbnailReady,
            this, &FileBrowser::onThumbnailReady);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* pathLayout = new QHBoxLayout();
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(4);

    pathEdit = new QLineEdit(this);
    pathEdit->setPlaceholderText("Directory...");
    pathEdit->setToolTip("Current directory path (press Enter to change)");
    pathLayout->addWidget(pathEdit);

    auto* browseButton = new QToolButton(this);
    browseButton->setText("...");
    browseButton->setToolTip("Browse directory");
    pathLayout->addWidget(browseButton);

    layout->addLayout(pathLayout);

    list = new QListWidget(this);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(128, 128));
    list->setGridSize(QSize(136, 156));
    list->setMovement(QListView::Static);
    list->setResizeMode(QListView::Adjust);
    list->setSpacing(6);
    list->setUniformItemSizes(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setWordWrap(true);
    layout->addWidget(list);

    connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < files.size())
            emit fileSelected(files[row]);
    });

    connect(list->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { requestVisibleThumbnails(); });

    connect(browseButton, &QToolButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this, tr("Select Directory"), currentDir,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!dir.isEmpty()) {
            setDirectory(dir);
            selectFirst();
        }
    });

    connect(pathEdit, &QLineEdit::returnPressed, this, [this]() {
        QString pathStr = pathEdit->text().trimmed();
        if (pathStr.isEmpty()) {
            pathEdit->setText(QDir::toNativeSeparators(currentDir));
            return;
        }
        if (pathStr.startsWith("~")) {
            pathStr = QDir::homePath() + pathStr.mid(1);
        }
        QFileInfo fi(pathStr);
        if (!fi.isAbsolute()) {
            fi = QFileInfo(QDir(currentDir).absoluteFilePath(pathStr));
        }
        if (fi.exists() && fi.isDir()) {
            setDirectory(fi.absoluteFilePath());
            selectFirst();
        } else {
            pathEdit->setText(QDir::toNativeSeparators(currentDir));
        }
    });
}

void FileBrowser::setDirectory(const QString& dir) {
    QString cleanDir = QDir(dir).canonicalPath();
    if (cleanDir.isEmpty()) {
        cleanDir = QDir(dir).absolutePath();
    }

    if (pathEdit) {
        pathEdit->setText(QDir::toNativeSeparators(cleanDir));
    }

    if (cleanDir == currentDir) return;
    currentDir = cleanDir;
    files = scanRawFiles(cleanDir);

    list->blockSignals(true);
    list->clear();
    for (const auto& f : files) {
        auto* item = new QListWidgetItem(QFileInfo(f).fileName(), list);
        item->setData(Qt::UserRole, f);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(136, 156));

        if (QImage cached = ThumbnailCache::loadFromDisk(f); !cached.isNull())
            item->setIcon(QPixmap::fromImage(cached));
    }
    list->blockSignals(false);

    QTimer::singleShot(0, this, [this] { requestVisibleThumbnails(); });
}

void FileBrowser::setCurrentFile(const QString& path) {
    int idx = files.indexOf(path);
    if (idx >= 0) {
        list->blockSignals(true);
        list->setCurrentRow(idx);
        list->scrollToItem(list->item(idx), QAbstractItemView::PositionAtCenter);
        list->blockSignals(false);
        requestVisibleThumbnails();
    }
}

void FileBrowser::selectFirst() {
    if (!files.isEmpty())
        list->setCurrentRow(0);  // triggers currentRowChanged → fileSelected
}

bool FileBrowser::navigateBy(int delta) {
    int current = list->currentRow();
    int next    = current + delta;
    if (next < 0 || next >= files.size()) return false;
    list->setCurrentRow(next);   // triggers currentRowChanged → fileSelected
    return true;
}

void FileBrowser::requestVisibleThumbnails() {
    const QRect visible = list->viewport()->rect();
    for (int row = 0; row < list->count(); ++row) {
        auto* item = list->item(row);
        if (!item || !item->icon().isNull())
            continue;
        const QRect itemRect = list->visualItemRect(item);
        if (!visible.intersects(itemRect))
            continue;
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            thumbs->request(path);
    }
}

void FileBrowser::onThumbnailReady(const QString& path, const QImage& image) {
    if (image.isNull())
        return;
    const int idx = files.indexOf(path);
    if (idx < 0 || idx >= list->count())
        return;
    auto* item = list->item(idx);
    if (!item || !item->icon().isNull())
        return;
    item->setIcon(QPixmap::fromImage(image));
}

QStringList FileBrowser::scanRawFiles(const QString& dir) {
    QDir d(dir);
    QStringList filters;
    for (const auto& ext : kSupportedExts) {
        filters << "*." + ext << "*." + ext.toUpper();
    }
    QStringList names = d.entryList(filters, QDir::Files, QDir::Name);
    QStringList result;
    result.reserve(names.size());
    for (const auto& name : names)
        result << d.filePath(name);
    return result;
}
