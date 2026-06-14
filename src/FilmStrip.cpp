#include "FilmStrip.h"
#include "FilmStripModel.h"
#include "FilmStripLayout.h"
#include "ThumbnailCache.h"

#include <QListView>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QTimer>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QEvent>
#include <QSignalBlocker>
#include <algorithm>

namespace {

constexpr int kCellPad = 4;       // padding around each thumbnail
constexpr int kBorderWidth = 2;   // current-item highlight border

// Paints aspect-correct thumbnails at the strip's content height, with a
// highlight border on the current item. Cell width comes from FilmStripLayout
// so the geometry stays in one tested place.
class ThumbnailDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setThumbHeight(int h) { thumbHeight = h; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const QImage img = index.data(Qt::DecorationRole).value<QImage>();
        const int w = filmstrip::cellWidth(thumbHeight, img.size());
        return {w + 2 * kCellPad, thumbHeight + 2 * kCellPad};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        const bool selected = option.state & QStyle::State_Selected;
        if (selected)
            painter->fillRect(option.rect, option.palette.highlight());

        const QRect inner = option.rect.adjusted(kCellPad, kCellPad, -kCellPad, -kCellPad);
        const QImage img = index.data(Qt::DecorationRole).value<QImage>();
        if (img.isNull()) {
            painter->fillRect(inner, option.palette.mid());  // placeholder
        } else {
            const QPixmap pm = QPixmap::fromImage(img).scaled(
                inner.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QRect target(QPoint(), pm.size());
            target.moveCenter(inner.center());
            painter->drawPixmap(target, pm);
        }

        if (selected) {
            painter->setPen(QPen(option.palette.highlight().color(), kBorderWidth));
            const int o = kBorderWidth / 2;
            painter->drawRect(option.rect.adjusted(o, o, -o - 1, -o - 1));
        }
        painter->restore();
    }

    int thumbHeight = 96;
};

} // namespace

FilmStrip::FilmStrip(QWidget* parent) : QWidget(parent) {
    model  = new FilmStripModel(this);
    thumbs = new ThumbnailCache(this);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    list = new QListView(this);
    list->setModel(model);
    list->setItemDelegate(new ThumbnailDelegate(list));
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(false);
    list->setMovement(QListView::Static);
    list->setUniformItemSizes(false);          // cell width varies with aspect
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list->viewport()->installEventFilter(this);
    layout->addWidget(list, 1);

    connect(list->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                if (current.isValid())
                    emit fileSelected(current.data(FilmStripModel::PathRole).toString());
            });

    connect(list->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { requestVisibleThumbnails(); });

    // A loaded thumbnail changes a cell's size, so re-check visibility after.
    connect(thumbs, &ThumbnailCache::thumbnailReady, this,
            [this](const QString& path, const QImage& image) {
                model->setThumbnail(path, image);
                QTimer::singleShot(0, this, [this] { requestVisibleThumbnails(); });
            });
}

void FilmStrip::setDirectory(const QString& dir) {
    QString clean = QDir(dir).canonicalPath();
    if (clean.isEmpty())
        clean = QDir(dir).absolutePath();
    if (clean == currentDir)
        return;

    currentDir = clean;
    emit directoryChanged(clean);
    model->setFiles(scanImageFiles(clean));

    // Deferred: cell geometry isn't laid out yet, so visibility tests would be
    // wrong if run synchronously here.
    QTimer::singleShot(0, this, [this] { requestVisibleThumbnails(); });
}

void FilmStrip::setCurrentFile(const QString& path) {
    const QModelIndex idx = model->indexForPath(path);
    if (!idx.isValid())
        return;
    // Block selection signals: this is called from MainWindow::loadImage, and
    // letting currentChanged fire would re-emit fileSelected and re-enter load.
    QSignalBlocker block(list->selectionModel());
    list->setCurrentIndex(idx);
    list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    requestVisibleThumbnails();
}

void FilmStrip::selectFirst() {
    if (model->rowCount() > 0)
        list->setCurrentIndex(model->index(0));  // fires currentChanged → fileSelected
}

bool FilmStrip::navigateBy(int delta) {
    const QModelIndex cur = list->currentIndex();
    const int next = (cur.isValid() ? cur.row() : -1) + delta;
    if (next < 0 || next >= model->rowCount())
        return false;
    list->setCurrentIndex(model->index(next));   // fires currentChanged → fileSelected
    return true;
}

void FilmStrip::promptForDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Folder"), currentDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        setDirectory(dir);
        selectFirst();
    }
}

bool FilmStrip::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list->viewport() && event->type() == QEvent::Resize)
        updateThumbHeight();
    return QWidget::eventFilter(watched, event);
}

void FilmStrip::updateThumbHeight() {
    auto* delegate = static_cast<ThumbnailDelegate*>(list->itemDelegate());
    const int h = std::max(32, list->viewport()->height() - 2 * kCellPad);
    if (h == delegate->thumbHeight)
        return;
    delegate->setThumbHeight(h);
    list->doItemsLayout();
    requestVisibleThumbnails();
}

void FilmStrip::requestVisibleThumbnails() {
    const QRect visible = list->viewport()->rect();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex idx = model->index(row);
        if (!idx.data(Qt::DecorationRole).value<QImage>().isNull())
            continue;
        if (!visible.intersects(list->visualRect(idx)))
            continue;
        thumbs->request(idx.data(FilmStripModel::PathRole).toString());
    }
}

QStringList FilmStrip::scanImageFiles(const QString& dir) {
    static const QStringList exts = {
        "cr2", "cr3", "nef", "arw", "dng", "raf", "orf", "rw2", "pef", "srw",
        "jpg", "jpeg", "png", "tiff", "tif", "webp", "bmp"
    };
    QDir d(dir);
    QStringList filters;
    for (const auto& ext : exts)
        filters << "*." + ext << "*." + ext.toUpper();

    QStringList result;
    const QStringList names = d.entryList(filters, QDir::Files);
    result.reserve(names.size());
    for (const auto& name : names)
        result << d.filePath(name);
    return result;  // FilmStripModel natural-sorts on assignment
}
