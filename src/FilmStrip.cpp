#include "FilmStrip.h"
#include "FilmStripModel.h"
#include "FilmStripLayout.h"
#include "ThumbnailCache.h"
#include "XmpSidecar.h"

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
#include <QMenu>
#include <QSignalBlocker>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>

namespace {

constexpr int kCellPad = 4;       // padding around each thumbnail
constexpr int kBorderWidth = 2;   // current-item highlight border
constexpr int kMarksMinHeight = 64;  // below this, stars are illegible: swatch only

// Swatch colours for the five labels (None never drawn).
QColor labelColour(ColourLabel label) {
    switch (label) {
    case ColourLabel::Red:    return QColor(0xD6, 0x45, 0x41);
    case ColourLabel::Yellow: return QColor(0xF5, 0xD8, 0x20);
    case ColourLabel::Green:  return QColor(0x4C, 0xAF, 0x50);
    case ColourLabel::Blue:   return QColor(0x3B, 0x82, 0xF6);
    case ColourLabel::Purple: return QColor(0x9B, 0x59, 0xB6);
    case ColourLabel::None:   break;
    }
    return Qt::transparent;
}

// Overlay the culling marks on a thumbnail cell: a colour swatch top-left, and
// (when the cell is tall enough to read them) filled stars on a translucent
// bottom bar. A reject (rating -1) dims the thumbnail and shows a ✗.
void paintMarks(QPainter* painter, const QRect& inner, int thumbHeight,
                int rating, ColourLabel label) {
    if (rating < 0)
        painter->fillRect(inner, QColor(0, 0, 0, 140));  // reject: dim the frame

    if (label != ColourLabel::None) {
        const int s = std::max(8, thumbHeight / 9);
        const QRect sw(inner.left() + 3, inner.top() + 3, s, s);
        painter->setPen(QPen(QColor(0, 0, 0, 160), 1));
        painter->setBrush(labelColour(label));
        painter->drawRoundedRect(sw, 2, 2);
    }

    if (thumbHeight < kMarksMinHeight)
        return;  // swatch stays readable at any size; stars do not

    QString glyphs;
    if (rating < 0)      glyphs = QStringLiteral("✗");            // ✗
    else if (rating > 0) glyphs = QString(rating, QChar(0x2605));      // ★×n
    if (glyphs.isEmpty())
        return;

    const int barH = std::max(14, thumbHeight / 6);
    const QRect bar(inner.left(), inner.bottom() - barH + 1, inner.width(), barH);
    painter->fillRect(bar, QColor(0, 0, 0, 110));
    QFont f = painter->font();
    f.setPixelSize(int(barH * 0.8));
    painter->setFont(f);
    painter->setPen(rating < 0 ? QColor(0xFF, 0x5A, 0x5A) : QColor(0xFF, 0xD7, 0x00));
    painter->drawText(bar, Qt::AlignCenter, glyphs);
}

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

        paintMarks(painter, inner, thumbHeight,
                   index.data(FilmStripModel::RatingRole).toInt(),
                   ColourLabel(index.data(FilmStripModel::LabelRole).toInt()));

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
    list->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list->viewport(), &QWidget::customContextMenuRequested,
            this, &FilmStrip::showContextMenu);
    layout->addWidget(list, 1);

    connect(list->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                if (current.isValid())
                    emit fileSelected(current.data(FilmStripModel::PathRole).toString());
            });

    // Centre the clicked thumbnail — but only after the click finishes
    // (clicked fires on release), so the scroll never moves content out from
    // under the cursor mid-click and re-selects a neighbour.
    connect(list, &QListView::clicked, this, [this](const QModelIndex& idx) {
        list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
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
    const QStringList files = scanImageFiles(clean);
    model->setFiles(files);
    loadMarks(files);

    // Deferred: cell geometry isn't laid out yet, so visibility tests would be
    // wrong if run synchronously here.
    QTimer::singleShot(0, this, [this] { requestVisibleThumbnails(); });
}

void FilmStrip::setCurrentFile(const QString& path) {
    const QModelIndex idx = model->indexForPath(path);
    if (!idx.isValid())
        return;
    // Called from MainWindow::loadImage on every selection. When it already
    // matches the view's current item — the common case, a click or keyboard
    // nav that originated here — there's nothing to do, and scrolling now would
    // yank the strip out from under the mouse mid-click. Only external opens
    // (File ▸ Open, command line) land here with a different current item.
    if (list->currentIndex() == idx)
        return;
    QSignalBlocker block(list->selectionModel());  // don't re-enter loadImage
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
    const QModelIndex idx = model->index(next);
    list->setCurrentIndex(idx);   // fires currentChanged → fileSelected
    list->scrollTo(idx, QAbstractItemView::PositionAtCenter);  // keyboard nav centres
    return true;
}

QString FilmStrip::currentPath() const {
    const QModelIndex cur = list->currentIndex();
    return cur.isValid() ? cur.data(FilmStripModel::PathRole).toString() : QString();
}

UserMetadata FilmStrip::currentMarks() const {
    return model->marksFor(currentPath());   // empty path → default marks
}

void FilmStrip::rateCurrent(int rating) {
    setRating(currentPath(), rating);
}

void FilmStrip::labelCurrent(ColourLabel label) {
    setLabel(currentPath(), label);
}

void FilmStrip::setRating(const QString& path, int rating) {
    if (path.isEmpty())
        return;
    UserMetadata m = model->marksFor(path);
    m.rating = rating;
    applyMarks(path, m);
}

void FilmStrip::setLabel(const QString& path, ColourLabel label) {
    if (path.isEmpty())
        return;
    UserMetadata m = model->marksFor(path);
    m.label = (m.label == label) ? ColourLabel::None : label;  // same key clears
    applyMarks(path, m);
}

// The single sink for a mark change: update the model (repaints the cell) and
// write it straight through to the sidecar (docs/adr/0007 — marks persist
// immediately, no Ctrl-S).
void FilmStrip::applyMarks(const QString& path, const UserMetadata& marks) {
    model->setMarks(path, marks);
    XmpSidecar::saveMetadata(path, marks);
}

void FilmStrip::loadMarks(const QStringList& paths) {
    using Marks = QList<QPair<QString, UserMetadata>>;
    auto* watcher = new QFutureWatcher<Marks>(this);
    connect(watcher, &QFutureWatcher<Marks>::finished, this, [this, watcher] {
        for (const auto& [path, m] : watcher->result())
            model->setMarks(path, m);   // no-ops paths no longer in the model
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([paths] {
        Marks out;
        for (const QString& p : paths) {
            const UserMetadata m = XmpSidecar::loadMetadata(p);
            if (!(m == UserMetadata{}))   // only carry files that actually have marks
                out.append({p, m});
        }
        return out;
    }));
}

void FilmStrip::showContextMenu(const QPoint& pos) {
    const QModelIndex idx = list->indexAt(pos);
    if (!idx.isValid())
        return;
    const QString path = idx.data(FilmStripModel::PathRole).toString();
    const UserMetadata cur = model->marksFor(path);

    QMenu menu(this);
    QMenu* rate = menu.addMenu(tr("Rating"));
    for (int n = 5; n >= 1; --n) {
        QAction* a = rate->addAction(QString(n, QChar(0x2605)));
        a->setCheckable(true);
        a->setChecked(cur.rating == n);
        connect(a, &QAction::triggered, this, [this, path, n] { setRating(path, n); });
    }
    rate->addSeparator();
    QAction* unrated = rate->addAction(tr("Unrated"));
    connect(unrated, &QAction::triggered, this, [this, path] { setRating(path, 0); });
    QAction* reject = rate->addAction(tr("Reject"));
    reject->setCheckable(true);
    reject->setChecked(cur.rating == -1);
    connect(reject, &QAction::triggered, this, [this, path] { setRating(path, -1); });

    QMenu* lab = menu.addMenu(tr("Label"));
    struct LabelEntry { const char* name; ColourLabel value; };
    for (auto [name, value] : {
             LabelEntry{"Red", ColourLabel::Red}, {"Yellow", ColourLabel::Yellow},
             {"Green", ColourLabel::Green}, {"Blue", ColourLabel::Blue},
             {"Purple", ColourLabel::Purple} }) {
        QAction* a = lab->addAction(tr(name));
        a->setCheckable(true);
        a->setChecked(cur.label == value);
        connect(a, &QAction::triggered, this, [this, path, value] { setLabel(path, value); });
    }
    QAction* none = lab->addAction(tr("None"));
    connect(none, &QAction::triggered, this, [this, path] {
        UserMetadata m = model->marksFor(path);
        m.label = ColourLabel::None;
        applyMarks(path, m);
    });

    menu.exec(list->viewport()->mapToGlobal(pos));
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
