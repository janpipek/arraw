#include "ui/FilmStrip.h"
#include "ThumbnailCache.h"
#include "core/ImageMetadata.h"
#include "develop/UserMetadata.h"
#include "io/XmpSidecar.h"
#include "ui/FilmStripFilterModel.h"
#include "ui/FilmStripLayout.h"
#include "ui/FilmStripModel.h"
#include "ui/ImageGrouping.h"

#include <algorithm>
#include <libraw/libraw.h>
#include <memory>
#include <optional>
#include <QContextMenuEvent>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QtConcurrent>

namespace {

constexpr int kCellPad = 6;         // padding around each thumbnail
constexpr int kBorderWidth = 4;     // current-item highlight border
constexpr int kMarksMinHeight = 64; // below this, stars are illegible: swatch only

// Overlay the culling marks on a thumbnail cell: a colour swatch top-left, and
// (when the cell is tall enough to read them) filled stars on a translucent
// bottom bar. A reject (rating -1) dims the thumbnail and shows a ✗.
void paintMarks(
    QPainter* painter, const QRect& inner, int thumbHeight, int rating, ColourLabel label) {
    if (rating < 0)
        painter->fillRect(inner, QColor(0, 0, 0, 140)); // reject: dim the frame

    if (label != ColourLabel::None) {
        const int s = std::max(8, thumbHeight / 9);
        const QRect sw(inner.left() + 3, inner.top() + 3, s, s);
        painter->setPen(QPen(QColor(0, 0, 0, 160), 1));
        painter->setBrush(labelColour(label));
        painter->drawRoundedRect(sw, 2, 2);
    }

    if (thumbHeight < kMarksMinHeight)
        return; // swatch stays readable at any size; stars do not

    QString glyphs;
    if (rating < 0)
        glyphs = QStringLiteral("✗"); // ✗
    else if (rating > 0)
        glyphs = QString(rating, QChar(0x2605)); // ★×n
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

// A small chip in the top-right corner naming the formats present in the shot
// (e.g. "ARW", "JPEG", or "ARW+JPEG"). See formatLabelText. Drawn on every cell,
// but suppressed by the caller when the cell is too short to read text.
void paintFormatLabel(QPainter* painter, const QRect& inner, const QString& text) {
    if (text.isEmpty())
        return;

    QFont f = painter->font();
    f.setPixelSize(std::max(9, inner.height() / 12));
    f.setBold(true);
    painter->setFont(f);

    const QFontMetrics fm(f);
    const int padX = 4;
    const int padY = 2;
    const QSize ts = fm.size(Qt::TextSingleLine, text);
    QRect chip(0, 0, ts.width() + (2 * padX), ts.height() + (2 * padY));
    chip.moveTopRight(inner.topRight() + QPoint(-2, 2));

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 150));
    painter->drawRoundedRect(chip, 3, 3);
    painter->setPen(QColor(0xF0, 0xF0, 0xF0));
    painter->drawText(chip, Qt::AlignCenter, text);
}

// Paints aspect-correct thumbnails inside square filmstrip cells.
// Active item (current index) gets a full-brightness highlight border;
// other selected items get a dimmer version so the batch target is visible
// but the editing focus is unambiguous.
class ThumbnailDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(QListView* view, QObject* parent = nullptr)
        : QStyledItemDelegate(parent),
          view(view) {}

    void setThumbHeight(int h) { thumbHeight = h; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const QImage img = index.data(Qt::DecorationRole).value<QImage>();
        const int w = filmstrip::cellWidth(thumbHeight, img.size());
        return {w + 2 * kCellPad, thumbHeight + 2 * kCellPad};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index)
        const override {
        painter->save();
        const bool selected = option.state & QStyle::State_Selected;
        const bool active = selected && (index == view->currentIndex());

        const QRect inner = option.rect.adjusted(kCellPad, kCellPad, -kCellPad, -kCellPad);
        const QImage img = index.data(Qt::DecorationRole).value<QImage>();
        if (img.isNull()) {
            painter->fillRect(inner, option.palette.mid()); // placeholder
        } else {
            const QPixmap pm
                = QPixmap::fromImage(img)
                      .scaled(inner.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QRect target(QPoint(), pm.size());
            target.moveCenter(inner.center());
            painter->drawPixmap(target, pm);
        }

        paintMarks(
            painter,
            inner,
            thumbHeight,
            index.data(FilmStripModel::RatingRole).toInt(),
            ColourLabel(index.data(FilmStripModel::LabelRole).toInt()));

        if (thumbHeight >= kMarksMinHeight) // text chip needs a readable cell, like the stars
            paintFormatLabel(painter, inner, index.data(FilmStripModel::FormatLabelRole).toString());

        if (active) {
            // Active (editing) item: full-brightness border
            painter->setPen(QPen(option.palette.highlight().color(), kBorderWidth));
            painter->setBrush(Qt::NoBrush);
            const int o = kBorderWidth / 2;
            painter->drawRect(option.rect.adjusted(o, o, -o - 1, -o - 1));
        } else if (selected) {
            // Selected-but-not-active: same weight as active but at 60% opacity so
            // the batch target is clearly visible while editing focus stays unambiguous.
            QColor dim = option.palette.highlight().color();
            dim.setAlphaF(0.6);
            const int o = kBorderWidth / 2;
            painter->setPen(QPen(dim, kBorderWidth));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(option.rect.adjusted(o, o, -o - 1, -o - 1));
        }
        painter->restore();
    }

    QListView* view;
    int thumbHeight = 96;
};

} // namespace

// Swatch colours for the five labels (None is transparent / never drawn).
QColor labelColour(ColourLabel label) {
    switch (label) {
    case ColourLabel::Red:
        return QColor(0xD6, 0x45, 0x41);
    case ColourLabel::Yellow:
        return QColor(0xF5, 0xD8, 0x20);
    case ColourLabel::Green:
        return QColor(0x4C, 0xAF, 0x50);
    case ColourLabel::Blue:
        return QColor(0x3B, 0x82, 0xF6);
    case ColourLabel::Purple:
        return QColor(0x9B, 0x59, 0xB6);
    case ColourLabel::None:
        break;
    }
    return Qt::transparent;
}

FilmStrip::FilmStrip(QWidget* parent)
    : QWidget(parent) {
    model = new FilmStripModel(this);
    proxy = new FilmStripFilterModel(this);
    proxy->setSourceModel(model);
    thumbs = new ThumbnailCache(this);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    list = new QListView(this);
    list->setModel(proxy);
    list->setItemDelegate(new ThumbnailDelegate(list, list));
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(false);
    list->setMovement(QListView::Static);
    list->setUniformItemSizes(true);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list->viewport()->installEventFilter(this);
    list->installEventFilter(this); // intercept culling keys before type-ahead
    layout->addWidget(list, 1);

    // Overlay shown when an active filter hides every cell. Transparent to the
    // mouse so it never swallows clicks on the (empty) viewport.
    emptyHint = new QLabel(tr("No shots match the filter"), list->viewport());
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyHint->setStyleSheet(QStringLiteral("color: gray;"));
    emptyHint->hide();

    // The proxy adds/removes rows as the filter or marks change; keep the hint in
    // sync with whatever ends up visible.
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(proxy, signal, this, [this] { updateEmptyHint(); });
    connect(proxy, &QAbstractItemModel::modelReset, this, [this] { updateEmptyHint(); });
    connect(proxy, &QAbstractItemModel::layoutChanged, this, [this] { updateEmptyHint(); });

    connect(
        list->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current, const QModelIndex&) {
            if (current.isValid())
                emit fileSelected(current.data(FilmStripModel::PathRole).toString());
        });

    connect(
        list->selectionModel(),
        &QItemSelectionModel::selectionChanged,
        this,
        [this](const QItemSelection&, const QItemSelection&) {
            emit selectionChanged(selectedPaths());
        });

    // Centre the clicked thumbnail — but only after the click finishes
    // (clicked fires on release), so the scroll never moves content out from
    // under the cursor mid-click and re-selects a neighbour.
    connect(list, &QListView::clicked, this, [this](const QModelIndex& idx) {
        list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    });

    connect(list->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        requestVisibleThumbnails();
    });

    // A loaded thumbnail changes a cell's size, so re-check visibility after.
    connect(
        thumbs,
        &ThumbnailCache::thumbnailReady,
        this,
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

    // Collapse RAW+JPEG captures into one cell each: the RAW is the primary
    // shown and culled, its same-stem standard images ride along as companions.
    const QList<ImageGroup> groups = groupImageFiles(scanImageFiles(clean));
    QStringList primaries;
    QHash<QString, QStringList> companions;
    primaries.reserve(groups.size());
    for (const ImageGroup& group : groups) {
        primaries.append(group.primary);
        if (!group.companions.isEmpty())
            companions.insert(group.primary, group.companions);
    }
    model->setFiles(primaries, companions);
    loadMarks(primaries);

    // Deferred: cell geometry isn't laid out yet, so visibility tests would be
    // wrong if run synchronously here.
    QTimer::singleShot(0, this, [this] { requestVisibleThumbnails(); });
}

void FilmStrip::setCurrentFile(const QString& path) {
    const QModelIndex idx = proxy->mapFromSource(model->indexForPath(path));
    if (!idx.isValid())
        return; // not in this directory, or currently hidden by the filter
    // Called from MainWindow::loadImage on every selection. When it already
    // matches the view's current item — the common case, a click or keyboard
    // nav that originated here — there's nothing to do, and scrolling now would
    // yank the strip out from under the mouse mid-click. Only external opens
    // (File ▸ Open, command line) land here with a different current item.
    if (list->currentIndex() == idx)
        return;
    QSignalBlocker block(list->selectionModel()); // don't re-enter loadImage
    list->setCurrentIndex(idx);
    list->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    requestVisibleThumbnails();
}

FilmStripFilter FilmStrip::filter() const {
    return proxy->filter();
}

void FilmStrip::setFilter(const FilmStripFilter& newFilter) {
    const QString active = currentPath(); // capture before re-filtering drops the row
    proxy->setFilter(newFilter);
    updateEmptyHint(); // covers a filter change that leaves the row count at zero

    if (active.isEmpty())
        return; // nothing was open; don't force a selection

    const QModelIndex stillThere = proxy->mapFromSource(model->indexForPath(active));
    if (stillThere.isValid()) {
        // The active image still matches: keep it, just re-centre it.
        list->scrollTo(stillThere, QAbstractItemView::PositionAtCenter);
        requestVisibleThumbnails();
        return;
    }
    // The active image was hidden by the new filter: jump to the nearest match.
    selectNearestVisible(model->indexForPath(active).row());
}

// Selects the nearest still-visible row to a source row, biased forward. The
// proxy preserves source order, so the first visible row whose source position
// is at/after `sourceRow` is the nearest forward match; if none, the last
// visible row is the nearest backward fallback. Used by both the filter-change
// jump and the re-rate auto-advance (ADR 0042).
void FilmStrip::selectNearestVisible(int sourceRow) {
    const int n = proxy->rowCount();
    if (n == 0) {
        // Nothing matches: clear the strip's selection, leave the viewport be.
        list->selectionModel()->clear();
        return;
    }
    int chosen = n - 1; // fallback: last visible (all matches precede sourceRow)
    for (int row = 0; row < n; ++row) {
        if (proxy->mapToSource(proxy->index(row, 0)).row() >= sourceRow) {
            chosen = row;
            break;
        }
    }
    const QModelIndex idx = proxy->index(chosen, 0);
    list->setCurrentIndex(idx); // fires currentChanged → fileSelected → loadImage
    list->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    requestVisibleThumbnails();
}

QStringList FilmStrip::selectedPaths() const {
    QStringList paths;
    for (const QModelIndex& idx : list->selectionModel()->selectedIndexes())
        paths << idx.data(FilmStripModel::PathRole).toString();
    return paths;
}

void FilmStrip::selectFirst() {
    if (proxy->rowCount() > 0) {
        const QModelIndex idx = proxy->index(0, 0); // first visible row
        list->setCurrentIndex(idx);                 // fires currentChanged → fileSelected
        list->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    }
}

void FilmStrip::setThumbnail(const QString& path, const QImage& image) {
    model->setThumbnail(path, image); // no-op if path isn't in the current listing
}

void FilmStrip::setMarks(const QString& path, const UserMetadata& marks) {
    model->setMarks(path, marks);
}

bool FilmStrip::navigateBy(int delta) {
    const QModelIndex cur = list->currentIndex();
    const int next = (cur.isValid() ? cur.row() : -1) + delta;
    if (next < 0 || next >= proxy->rowCount()) // ±1 over visible rows only
        return false;
    const QModelIndex idx = proxy->index(next, 0);
    list->setCurrentIndex(idx); // fires currentChanged → fileSelected
    list->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    list->scrollTo(idx, QAbstractItemView::PositionAtCenter); // keyboard nav centres
    return true;
}

QString FilmStrip::currentPath() const {
    const QModelIndex cur = list->currentIndex();
    return cur.isValid() ? cur.data(FilmStripModel::PathRole).toString() : QString();
}

UserMetadata FilmStrip::currentMarks() const {
    return model->marksFor(currentPath()); // empty path → default marks
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
    m.label = (m.label == label) ? ColourLabel::None : label; // same key clears
    applyMarks(path, m);
}

// The single sink for a mark change: update the model (repaints the cell) and
// write it straight through to the sidecar (docs/adr/0007 — marks persist
// immediately, no Ctrl-S).
void FilmStrip::applyMarks(const QString& path, const UserMetadata& marks) {
    const bool wasActive = (path == currentPath());
    model->setMarks(path, marks); // dynamic proxy may now hide this row
    // If you re-rated the open image out of the active filter, auto-advance to
    // the next match so culling stays a single-key rhythm (ADR 0042).
    if (wasActive && !proxy->mapFromSource(model->indexForPath(path)).isValid())
        selectNearestVisible(model->indexForPath(path).row());
    const bool saved = XmpSidecar::saveMetadata(path, marks);
    emit marksChanged(path, marks, saved);
}

QStringList FilmStrip::contextTargetPaths(const QString& path) const {
    QStringList targets = selectedPaths();
    if (targets.contains(path))
        return targets;
    return {path};
}

void FilmStrip::loadMarks(const QStringList& paths) {
    using Marks = QList<QPair<QString, UserMetadata>>;
    auto* watcher = new QFutureWatcher<Marks>(this);
    connect(watcher, &QFutureWatcher<Marks>::finished, this, [this, watcher] {
        for (const auto& [path, m] : watcher->result())
            model->setMarks(path, m); // no-ops paths no longer in the model
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([paths] {
        Marks out;
        for (const QString& p : paths) {
            const UserMetadata m = XmpSidecar::loadMetadata(p);
            if (!(m == UserMetadata{})) // only carry files that actually have marks
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
    const QStringList targets = contextTargetPaths(path);
    const UserMetadata cur = model->marksFor(path);

    QMenu menu(this);
    emit populateContextMenu(path, targets, &menu);

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

    struct LabelEntry {
        const char* name;
        ColourLabel value;
    };

    for (auto [name, value] :
         {LabelEntry{"Red", ColourLabel::Red},
          {"Yellow", ColourLabel::Yellow},
          {"Green", ColourLabel::Green},
          {"Blue", ColourLabel::Blue},
          {"Purple", ColourLabel::Purple}}) {
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
        this,
        tr("Open Folder"),
        currentDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        setDirectory(dir);
        selectFirst();
    }
}

bool FilmStrip::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list->viewport() && event->type() == QEvent::Resize) {
        updateThumbHeight();
        if (emptyHint->isVisible())
            emptyHint->setGeometry(list->viewport()->rect());
    }

    if (watched == list->viewport() && event->type() == QEvent::ToolTip)
        return handleTooltip(static_cast<QHelpEvent*>(event));

    if (watched == list->viewport() && event->type() == QEvent::ContextMenu) {
        showContextMenu(static_cast<QContextMenuEvent*>(event)->pos());
        return true;
    }

    if (watched == list->viewport()
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton)
            return true;
    }

    // A Ctrl/Shift-click is a batch-selection gesture, not a navigation. Handle it
    // ourselves so the active image (current index) stays put — otherwise the view
    // would move current to the clicked item and MainWindow would reload it.
    if (watched == list->viewport() && event->type() == QEvent::MouseButtonPress) {
        if (handleModifierClick(static_cast<QMouseEvent*>(event)))
            return true;
    }

    // Culling keys: handle them on the list before QListView's type-ahead search
    // (which the view's shortcut-override otherwise steals — e.g. X jumping to a
    // filename) can run. Navigation keys fall through to the view as normal.
    if (watched == list && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->modifiers() == Qt::NoModifier && handleMarkKey(ke->key()))
            return true;
    }
    return QWidget::eventFilter(watched, event);
}

bool FilmStrip::handleTooltip(QHelpEvent* event) {
    const QModelIndex idx = list->indexAt(event->pos());
    if (!idx.isValid()) {
        QToolTip::hideText();
        return true;
    }

    const QString path = idx.data(FilmStripModel::PathRole).toString();
    if (!model->hasMetadata(path)) {
        if (const std::optional<ImageMetadata> cached = ThumbnailCache::loadMetadata(path))
            model->setMetadata(path, *cached);
        else
            requestTooltipMetadata(path, event->globalPos());
    }

    QToolTip::showText(event->globalPos(), idx.data(Qt::ToolTipRole).toString(), list->viewport());
    return true;
}

void FilmStrip::requestTooltipMetadata(const QString& path, const QPoint& globalPos) {
    if (metadataPending.contains(path))
        return;
    metadataPending.insert(path);

    QPointer<FilmStrip> self(this);
    (void) QtConcurrent::run([self, path, globalPos]() {
        std::optional<ImageMetadata> metadata;
        auto raw = std::make_unique<LibRaw>();
        if (raw->open_file(path.toLocal8Bit().constData()) == LIBRAW_SUCCESS) {
            metadata = extractMetadata(*raw);
            ThumbnailCache::storeMetadata(path, *metadata);
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(
            self,
            [self, path, globalPos, metadata = std::move(metadata)]() mutable {
                if (!self)
                    return;
                self->metadataPending.remove(path);
                if (!metadata.has_value())
                    return;
                self->model->setMetadata(path, *metadata);
                QToolTip::showText(
                    globalPos,
                    self->model->indexForPath(path).data(Qt::ToolTipRole).toString(),
                    self->list->viewport());
            },
            Qt::QueuedConnection);
    });
}

bool FilmStrip::handleMarkKey(int key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_5) {
        rateCurrent(key - Qt::Key_0);
        return true;
    }
    switch (key) {
    case Qt::Key_X:
        rateCurrent(-1);
        return true;
    case Qt::Key_R:
        labelCurrent(ColourLabel::Red);
        return true;
    case Qt::Key_Y:
        labelCurrent(ColourLabel::Yellow);
        return true;
    case Qt::Key_G:
        labelCurrent(ColourLabel::Green);
        return true;
    case Qt::Key_B:
        labelCurrent(ColourLabel::Blue);
        return true;
    case Qt::Key_P:
        labelCurrent(ColourLabel::Purple);
        return true;
    }
    return false;
}

bool FilmStrip::handleModifierClick(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton)
        return false;
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    if (!ctrl && !shift)
        return false; // plain click: let the view drive navigation as usual

    const QModelIndex idx = list->indexAt(e->position().toPoint());
    if (!idx.isValid())
        return false; // click on empty space: leave to the view

    // Anchor the gesture on the active image (current index) and never move it,
    // so the viewport keeps showing the file being edited.
    auto* sm = list->selectionModel();
    const QModelIndex anchor = list->currentIndex();

    if (shift) {
        // Contiguous range from the active image to the clicked one.
        const int a = anchor.isValid() ? anchor.row() : idx.row();
        const QModelIndex top = proxy->index(std::min(a, idx.row()), 0);
        const QModelIndex bottom = proxy->index(std::max(a, idx.row()), 0);
        sm->select(QItemSelection(top, bottom), QItemSelectionModel::ClearAndSelect);
    } else { // ctrl
        sm->select(idx, QItemSelectionModel::Toggle);
        // The active image must always remain part of the selection.
        if (anchor.isValid())
            sm->select(anchor, QItemSelectionModel::Select);
    }

    list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    return true; // consume: don't let the view move current / reload the image
}

void FilmStrip::updateThumbHeight() {
    // Square cells size to the viewport height, so re-laying the view can push the
    // total width across the horizontal scrollbar's appear/disappear threshold.
    // The scrollbar then steals (or returns) viewport height, delivering another
    // synchronous resize that re-enters here — an unbounded recursion that
    // overflows the stack (most visible when enlarging the window). Bail out of
    // the nested call so the height the outer call computed simply stands.
    if (updatingThumbHeight)
        return;
    auto* delegate = static_cast<ThumbnailDelegate*>(list->itemDelegate());
    const int h = std::max(32, list->viewport()->height() - 2 * kCellPad);
    if (h == delegate->thumbHeight)
        return;
    updatingThumbHeight = true;
    delegate->setThumbHeight(h);
    list->doItemsLayout();
    requestVisibleThumbnails();
    updatingThumbHeight = false;
}

void FilmStrip::requestVisibleThumbnails() {
    const QRect visible = list->viewport()->rect();
    for (int row = 0; row < proxy->rowCount(); ++row) { // visible rows only
        const QModelIndex idx = proxy->index(row, 0);
        if (!idx.data(Qt::DecorationRole).value<QImage>().isNull())
            continue;
        if (!visible.intersects(list->visualRect(idx)))
            continue;
        thumbs->request(idx.data(FilmStripModel::PathRole).toString());
    }
}

void FilmStrip::updateEmptyHint() {
    const bool show = proxy->filter().isActive() && proxy->rowCount() == 0;
    if (show) {
        emptyHint->setGeometry(list->viewport()->rect());
        emptyHint->raise();
    }
    emptyHint->setVisible(show);
}

QStringList FilmStrip::scanImageFiles(const QString& dir) {
    static const QStringList exts
        = {"cr2",
           "cr3",
           "nef",
           "arw",
           "dng",
           "raf",
           "orf",
           "rw2",
           "pef",
           "srw",
           "jpg",
           "jpeg",
           "png",
           "tiff",
           "tif",
           "webp",
           "bmp"};
    QDir d(dir);
    QStringList filters;
    for (const auto& ext : exts)
        filters << "*." + ext << "*." + ext.toUpper();

    QStringList result;
    const QStringList names = d.entryList(filters, QDir::Files);
    result.reserve(names.size());
    for (const auto& name : names)
        result << d.filePath(name);
    return result; // FilmStripModel natural-sorts on assignment
}
