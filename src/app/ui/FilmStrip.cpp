#include "FilmStrip.h"

#include <Sidecar.h>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <exception>
#include <utility>

namespace arraw::app {

namespace {

namespace fs = std::filesystem;

/// Width of the outline of the active and the selected shots.
constexpr int borderWidth = 4;

/// Below this content height the stars and the format chip are illegible, and left out.
constexpr int marksMinHeight = 64;

/// Time a listing runs before the folder label says so, and between two updates of the note;
/// the anti-flicker rule of the render progress.
constexpr int busyNoteMs = 250;

/// Returns the font of the file name caption: the widget's, a little smaller.
QFont captionFont(QFont font) {
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(std::max(6.0, font.pointSizeF() * 0.85));
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(7, static_cast<int>(font.pixelSize() * 0.85)));
    }
    return font;
}

/// Returns the height of the caption strip below a thumbnail, for the widget's font.
int captionHeight(const QFont& widgetFont) {
    return QFontMetrics(captionFont(widgetFont)).height() + cellPadding;
}

QString toQString(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

fs::path toPath(const QString& text) {
    return fs::path(text.toStdU16String());
}

/// @brief Paints the colour swatch and the stars over a cell, as main's strip did.
///
/// A reject dims the frame and shows a cross where the stars would be.
void paintMarks(QPainter* painter, const QRect& inner, int rating, int label) {
    if (rating < 0) {
        painter->fillRect(inner, QColor(0, 0, 0, 140));
    }
    if (label >= 0) {
        const int size = std::max(8, inner.height() / 9);
        const QRect swatch(inner.left() + 3, inner.top() + 3, size, size);
        painter->setPen(QPen(QColor(0, 0, 0, 160), 1));
        painter->setBrush(labelColour(static_cast<ColorLabel>(label)));
        painter->drawRoundedRect(swatch, 2, 2);
    }
    if (inner.height() < marksMinHeight || rating == 0) {
        return;
    }
    const QString glyphs = rating < 0 ? QStringLiteral("✗") : QString(rating, QChar(0x2605));
    const int barHeight = std::max(14, inner.height() / 6);
    const QRect bar(inner.left(), inner.bottom() - barHeight + 1, inner.width(), barHeight);
    painter->fillRect(bar, QColor(0, 0, 0, 110));
    QFont font = painter->font();
    font.setPixelSize(static_cast<int>(barHeight * 0.8));
    painter->setFont(font);
    painter->setPen(rating < 0 ? QColor(0xFF, 0x5A, 0x5A) : QColor(0xFF, 0xD7, 0x00));
    painter->drawText(bar, Qt::AlignCenter, glyphs);
}

/// @brief Paints a chip with the format label in the top right corner of a cell.
void paintFormatChip(QPainter* painter, const QRect& inner, const QString& text) {
    if (text.isEmpty()) {
        return;
    }
    QFont font = painter->font();
    font.setPixelSize(std::max(9, inner.height() / 12));
    font.setBold(true);
    painter->setFont(font);
    const QSize size = QFontMetrics(font).size(Qt::TextSingleLine, text);
    QRect chip(0, 0, size.width() + 8, size.height() + 4);
    chip.moveTopRight(inner.topRight() + QPoint(-2, 2));
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 150));
    painter->drawRoundedRect(chip, 3, 3);
    painter->setPen(QColor(0xF0, 0xF0, 0xF0));
    painter->drawText(chip, Qt::AlignCenter, text);
}

/// @brief Paints the badge of a shot with a sidecar, bottom left of a cell: three sliders.
///
/// A dark chip with light strokes, so that it reads over any photograph and any palette.
void paintSidecarBadge(QPainter* painter, const QRect& inner) {
    const int size = std::max(12, inner.height() / 9);
    // Above the stars bar when that is shown.
    const int lift = inner.height() >= marksMinHeight ? std::max(14, inner.height() / 6) + 2 : 3;
    const QRect badge(inner.left() + 3, inner.bottom() - lift - size + 1, size, size);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 150));
    painter->drawRoundedRect(badge, 3, 3);
    const QColor ink(0xF0, 0xF0, 0xF0);
    const double margin = size * 0.2;
    const double left = badge.left() + margin;
    const double right = badge.right() + 1 - margin;
    const double knob = std::max(1.5, size * 0.11);
    const double knobs[] = {0.35, 0.7, 0.45};
    for (int i = 0; i < 3; ++i) {
        const double y = badge.top() + size * (0.27 + 0.23 * i);
        painter->setPen(QPen(ink, std::max(1.0, size / 14.0), Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(QPointF(left, y), QPointF(right, y));
        painter->setPen(Qt::NoPen);
        painter->setBrush(ink);
        painter->drawEllipse(QPointF(left + (right - left) * knobs[i], y), knob, knob);
    }
}

} // namespace

/// @brief Paints a shot's cell: thumbnail or placeholder, marks, and the outline of the active
/// and the selected shots.
class FilmStripDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    /// @brief Sets the side of the square thumbnail area of the cells and the height of the file
    /// name caption below it (0 for none).
    void setSide(int side, int caption) noexcept {
        side_ = side;
        caption_ = caption;
    }

    /// @brief Returns the height of the caption strip below the thumbnail area, 0 if none.
    [[nodiscard]] int caption() const noexcept {
        return caption_;
    }

    /// @brief Returns the side of the cells.
    [[nodiscard]] int side() const noexcept {
        return side_;
    }

    /// @brief Names the active shot, to outline it.
    void setActive(QString primary) {
        active_ = std::move(primary);
    }

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {side_, side_ + caption_};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool active =
            !active_.isEmpty() && index.data(ShotModel::PathRole).toString() == active_;
        const QRect thumbnailCell(option.rect.topLeft(), QSize(option.rect.width(), side_));
        const QRect inner =
            thumbnailCell.adjusted(cellPadding, cellPadding, -cellPadding, -cellPadding);
        const QString format = index.data(ShotModel::FormatLabelRole).toString();

        const auto thumbnail = index.data(ShotModel::ThumbnailRole).value<QImage>();
        if (thumbnail.isNull()) {
            // Until the thumbnail worker hands one over: a dark tile naming the format.
            painter->fillRect(inner, QColor(0x2B, 0x2B, 0x2B));
            QFont font = painter->font();
            font.setPixelSize(std::max(10, inner.height() / 7));
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(QColor(0x9A, 0x9A, 0x9A));
            painter->drawText(inner, Qt::AlignCenter, format);
        } else {
            const QImage scaled =
                thumbnail.scaled(inner.size() * painter->device()->devicePixelRatioF(),
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QRect target(QPoint(), scaled.size() / painter->device()->devicePixelRatioF());
            target.moveCenter(inner.center());
            // Letterboxed: the cell's tile shows around a photograph of another shape.
            painter->fillRect(inner, QColor(0x2B, 0x2B, 0x2B));
            painter->drawImage(target, scaled);
            if (inner.height() >= marksMinHeight) {
                paintFormatChip(painter, inner, format);
            }
        }

        paintMarks(painter, inner, index.data(ShotModel::RatingRole).toInt(),
                   index.data(ShotModel::LabelRole).toInt());
        if (index.data(ShotModel::HasSidecarRole).toBool()) {
            paintSidecarBadge(painter, inner);
        }

        QColor colour = option.palette.highlight().color();
        if (active || selected) {
            if (!active) {
                // Selected beside the active one: the same weight, dimmer.
                colour.setAlphaF(0.6);
            }
        }
        if (caption_ > 0 && inner.height() >= marksMinHeight) {
            const QRect strip(option.rect.left() + borderWidth, thumbnailCell.bottom() + 1,
                              option.rect.width() - 2 * borderWidth,
                              option.rect.bottom() - thumbnailCell.bottom() - borderWidth);
            if (active || selected) {
                painter->fillRect(strip, colour);
            }
            const QFont font = captionFont(option.font);
            painter->setFont(font);
            painter->setPen(active || selected ? option.palette.highlightedText().color()
                                               : option.palette.text().color());
            const QString name = QFontMetrics(font).elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, strip.width() - 2);
            painter->drawText(strip, Qt::AlignCenter, name);
        }
        if (active || selected) {
            painter->setPen(QPen(colour, borderWidth));
            painter->setBrush(Qt::NoBrush);
            const int offset = borderWidth / 2;
            painter->drawRect(option.rect.adjusted(offset, offset, -offset - 1, -offset - 1));
        }
        painter->restore();
    }

private:
    int side_ = 132;
    int caption_ = 0;
    QString active_;
};

FilmStrip::FilmStrip(QWidget* parent, ThumbnailCache cache, LensDatabaseSource lensDatabase)
    : QWidget(parent), watcher_(model_),
      thumbnails_(
          std::move(cache),
          [this](ThumbnailResult result) {
              // On the worker thread. Dropped if the strip is gone by the time the GUI thread
              // would run it.
              QMetaObject::invokeMethod(
                  this, [this, result = std::move(result)] { receiveThumbnail(result); },
                  Qt::QueuedConnection);
          },
          true, std::move(lensDatabase)) {
    proxy_.setSourceModel(&model_);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    list_ = new QListView(this);
    list_->setModel(&proxy_);
    delegate_ = new FilmStripDelegate(list_);
    list_->setItemDelegate(delegate_);
    list_->setFlow(QListView::LeftToRight);
    list_->setWrapping(false);
    list_->setMovement(QListView::Static);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Always there, so that the room for the cells does not depend on whether they fit.
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Keys are the window's (arrows step, letters mark), never the view's own type-ahead.
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->viewport()->installEventFilter(this);
    list_->installEventFilter(this);
    layout->addWidget(list_, 1);

    emptyHint_ = new QLabel(tr("No shots match the filter"), list_->viewport());
    emptyHint_->setObjectName(QStringLiteral("filmStripEmptyHint"));
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyHint_->setStyleSheet(QStringLiteral("color: gray;"));
    emptyHint_->hide();

    buildTitleBar();

    activeCheck_.setSingleShot(true);
    activeCheck_.setInterval(0);
    connect(&activeCheck_, &QTimer::timeout, this, &FilmStrip::ensureActiveShown);
    for (const auto signal :
         {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(&proxy_, signal, this, [this] {
            updateEmptyHint();
            scheduleActiveCheck();
        });
    }
    connect(&proxy_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyHint(); });
    connect(&proxy_, &QAbstractItemModel::layoutChanged, this, [this] {
        updateEmptyHint();
        scheduleActiveCheck();
    });
    connect(&watcher_, &FolderWatcher::sidecarChangedExternally, this,
            [this](const QString& primary) {
                thumbnails_.invalidate(toPath(primary));
                emit sidecarChangedExternally(primary);
            });

    // Shots appear as the walk brings them, and by refresh (the watcher). Connected after the
    // proxy, which has taken the rows in by the time this runs.
    connect(&model_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) { takeInsertedRows(first, last); });
    connect(&model_, &ShotModel::listingChanged, this, &FilmStrip::onListingChanged);
    busyTimer_.setInterval(busyNoteMs);
    connect(&busyTimer_, &QTimer::timeout, this, &FilmStrip::updateFolderLabel);
    // A change too scattered to merge by runs resets the model (see ShotModel::applyFolders);
    // the selection is carried over it. A new folder is not carried, and gets its thumbnails in
    // setFolder.
    connect(&proxy_, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        if (!changingFolder_) {
            stashSelection();
        }
    });
    connect(&proxy_, &QAbstractItemModel::modelReset, this, [this] {
        if (!changingFolder_) {
            restoreSelection();
        }
    });
    connect(&model_, &QAbstractItemModel::modelReset, this, [this] {
        if (changingFolder_) {
            return;
        }
        std::vector<fs::path> shots;
        shots.reserve(static_cast<std::size_t>(model_.rowCount()));
        for (int row = 0; row < model_.rowCount(); ++row) {
            shots.push_back(model_.shot(row).primary);
        }
        thumbnails_.addShots(shots);
    });
    // What is on screen changes with scrolling, size, filter and the shots themselves; one report
    // after the burst.
    visibleTimer_.setSingleShot(true);
    visibleTimer_.setInterval(40);
    connect(&visibleTimer_, &QTimer::timeout, this, &FilmStrip::reportVisible);
    const auto scheduleVisible = [this] { visibleTimer_.start(); };
    connect(list_->horizontalScrollBar(), &QScrollBar::valueChanged, this, scheduleVisible);
    connect(&proxy_, &QAbstractItemModel::rowsInserted, this, scheduleVisible);
    connect(&proxy_, &QAbstractItemModel::rowsRemoved, this, scheduleVisible);
    connect(&proxy_, &QAbstractItemModel::modelReset, this, scheduleVisible);
    connect(&proxy_, &QAbstractItemModel::layoutChanged, this, scheduleVisible);
}

FilmStrip::~FilmStrip() = default;

QSize FilmStrip::sizeHint() const {
    return {600, 132 + captionHeight(font())};
}

void FilmStrip::buildTitleBar() {
    titleBar_ = new QWidget(this);
    auto* layout = new QHBoxLayout(titleBar_);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(6);

    auto* folderButton = new QToolButton(titleBar_);
    folderButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    folderButton->setAutoRaise(true);
    folderButton->setToolTip(tr("Open Folder…"));
    folderButton->setFocusPolicy(Qt::NoFocus);
    connect(folderButton, &QToolButton::clicked, this, &FilmStrip::folderRequested);
    layout->addWidget(folderButton);

    subfoldersButton_ = new QToolButton(titleBar_);
    subfoldersButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    subfoldersButton_->setCheckable(true);
    subfoldersButton_->setAutoRaise(true);
    subfoldersButton_->setToolTip(tr("Include subfolders"));
    subfoldersButton_->setFocusPolicy(Qt::NoFocus);
    connect(subfoldersButton_, &QToolButton::toggled, this, [this](bool include) {
        // A shot still waiting for its listing counts as the active one.
        const fs::path active = active_.empty() ? pendingActive_.value_or(fs::path()) : active_;
        const bool leaves =
            !include && !active.empty() && !model_.folder().empty() &&
            active.parent_path().lexically_normal() != model_.folder().lexically_normal();
        if (leaves && subfoldersLeaveGuard_ && !subfoldersLeaveGuard_()) {
            subfoldersButton_->blockSignals(true);
            subfoldersButton_->setChecked(true);
            subfoldersButton_->blockSignals(false);
            return;
        }
        setIncludeSubfolders(include);
    });
    layout->addWidget(subfoldersButton_);

    folderLabel_ = new QLabel(tr("No folder"), titleBar_);
    folderLabel_->setObjectName(QStringLiteral("filmStripFolderLabel"));
    folderLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Yields room to the filter controls when the window is narrow.
    folderLabel_->setMinimumWidth(0);
    layout->addWidget(folderLabel_, 1);

    buildFilterControls(titleBar_);
}

void FilmStrip::buildFilterControls(QWidget* into) {
    auto* layout = static_cast<QHBoxLayout*>(into->layout());
    layout->addWidget(new QLabel(tr("Filter:"), into));

    starChoice_ = new QComboBox(into);
    starChoice_->setFocusPolicy(Qt::NoFocus);
    starChoice_->addItem(tr("Any rating"), 0);
    for (int stars = 1; stars <= highestRating; ++stars) {
        starChoice_->addItem(QString::fromUtf8("≥ ") + QString::number(stars) + QChar(0x2605),
                             stars);
    }
    starChoice_->addItem(tr("Rejects only"), rejectsOnlyChoice);
    starChoice_->setToolTip(tr("Show shots with at least this many stars"));
    layout->addWidget(starChoice_);

    for (const auto& [label, name] : colorLabelNames) {
        auto* button = new QToolButton(into);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tr("Show %1 shots").arg(tr(labelName(label))));
        button->setFixedSize(18, 18);
        button->setStyleSheet(
            QStringLiteral("QToolButton{border:1px solid #555;border-radius:3px;background:%1;}"
                           "QToolButton:checked{border:2px solid white;}")
                .arg(labelColour(label).name()));
        layout->addWidget(button);
        swatches_.emplace_back(button, label);
        connect(button, &QToolButton::toggled, this, &FilmStrip::applyFilter);
    }
    connect(starChoice_, &QComboBox::currentIndexChanged, this, &FilmStrip::applyFilter);

    clearButton_ = new QToolButton(into);
    clearButton_->setText(tr("Clear"));
    clearButton_->setAutoRaise(true);
    clearButton_->setFocusPolicy(Qt::NoFocus);
    clearButton_->setToolTip(tr("Clear the filter"));
    clearButton_->setEnabled(false);
    layout->addWidget(clearButton_);
    connect(clearButton_, &QToolButton::clicked, this, &FilmStrip::clearFilter);
}

void FilmStrip::applyFilter() {
    std::set<ColorLabel> labels;
    for (const auto& [button, label] : swatches_) {
        if (button->isChecked()) {
            labels.insert(label);
        }
    }
    const MarksFilter next = filterOf(starChoice_->currentData().toInt(), labels);
    clearButton_->setEnabled(next.isActive());
    proxy_.setFilter(next);
    updateEmptyHint();
    scheduleActiveCheck();
}

void FilmStrip::clearFilter() {
    {
        const QSignalBlocker blockCombo(starChoice_);
        starChoice_->setCurrentIndex(0);
        for (const auto& [button, label] : swatches_) {
            const QSignalBlocker blocker(button);
            button->setChecked(false);
        }
    }
    applyFilter();
}

void FilmStrip::stashSelection() {
    ResetStash stash;
    for (const QModelIndex& index : list_->selectionModel()->selectedIndexes()) {
        stash.selected.push_back(primaryAt(index));
    }
    if (const QModelIndex current = list_->selectionModel()->currentIndex(); current.isValid()) {
        stash.current = primaryAt(current);
    }
    if (const QModelIndex first = list_->indexAt(QPoint(2, 2)); first.isValid()) {
        stash.anchor = primaryAt(first);
    }
    stash_ = std::move(stash);
}

void FilmStrip::restoreSelection() {
    if (!stash_) {
        return;
    }
    const ResetStash stash = *std::exchange(stash_, std::nullopt);
    QItemSelection selection;
    for (const fs::path& primary : stash.selected) {
        if (const QModelIndex index = proxyIndexOf(primary); index.isValid()) {
            selection.select(index, index);
        }
    }
    QItemSelectionModel* model = list_->selectionModel();
    model->select(selection, QItemSelectionModel::ClearAndSelect);
    // The shot that is open keeps the focus if nothing else had it.
    const QModelIndex current = proxyIndexOf(stash.current.empty() ? active_ : stash.current);
    if (current.isValid()) {
        model->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    }
    if (const QModelIndex anchor = proxyIndexOf(stash.anchor); anchor.isValid()) {
        list_->scrollTo(anchor, QAbstractItemView::PositionAtTop);
    }
}

void FilmStrip::listFolder(const fs::path& folder, std::optional<fs::path> activate) {
    changingFolder_ = true;
    try {
        model_.setFolder(folder, includeSubfolders_);
    } catch (...) {
        changingFolder_ = false;
        throw;
    }
    changingFolder_ = false;
    live_.clear();
    // The rows come as the walk brings them; each insert hands its shots to the worker.
    thumbnails_.setShots({});
    visibleTimer_.start();
    active_.clear();
    pendingActive_ = std::move(activate);
    firstShotSent_ = false;
    ++listingNumber_;
    delegate_->setActive({});
    updateFolderLabel();
    updateEmptyHint();
}

void FilmStrip::setFolder(const fs::path& folder, std::optional<fs::path> activate) {
    listFolder(folder, std::move(activate));
    emit activeChanged();
}

void FilmStrip::takeInsertedRows(int first, int last) {
    std::vector<fs::path> added;
    added.reserve(static_cast<std::size_t>(last - first + 1));
    std::optional<fs::path> found;
    for (int row = first; row <= last; ++row) {
        const Shot& shot = model_.shot(row);
        added.push_back(shot.primary);
        if (pendingActive_ && !found &&
            (shot.primary == *pendingActive_ ||
             std::ranges::find(shot.companions, *pendingActive_) != shot.companions.end())) {
            found = shot.primary;
        }
    }
    thumbnails_.addShots(added);
    if (found) {
        pendingActive_.reset();
        setActive(*found);
    }
    if (!firstShotSent_ && model_.isListing() && proxy_.rowCount() > 0) {
        firstShotSent_ = true;
        // Sent from the event loop, not from within the model's insert: the window may open the
        // shot and ask about unsaved edits in a dialog, whose loop must not deliver the next
        // batch into the middle of this one.
        QMetaObject::invokeMethod(
            this,
            [this, listing = listingNumber_, primary = toQString(primaryAt(proxy_.index(0, 0)))] {
                if (listing == listingNumber_) {
                    emit firstShotListed(primary);
                }
            },
            Qt::QueuedConnection);
    }
}

void FilmStrip::onListingChanged(bool listing) {
    if (listing) {
        listingClock_.start();
        busyTimer_.start();
    } else {
        busyTimer_.stop();
        // The listing is complete without it: the photograph stays open, and no shot is active.
        pendingActive_.reset();
        // Queued as firstShotListed is, so that it comes after it.
        QMetaObject::invokeMethod(
            this,
            [this, number = listingNumber_] {
                if (number == listingNumber_) {
                    emit listingEnded();
                }
            },
            Qt::QueuedConnection);
    }
    updateFolderLabel();
    updateEmptyHint();
}

void FilmStrip::updateFolderLabel() {
    QString text = toQString(model_.folder().lexically_normal());
    folderLabel_->setToolTip(text);
    if (includeSubfolders_ && !model_.folder().empty()) {
        text += tr(" (+ subfolders)");
    }
    if (model_.isListing() && listingClock_.isValid() && listingClock_.elapsed() >= busyNoteMs) {
        text += tr(" — listing… %1 shots").arg(QLocale().toString(model_.rowCount()));
    }
    folderLabel_->setText(text);
}

void FilmStrip::setIncludeSubfolders(bool include) {
    if (include == includeSubfolders_) {
        return;
    }
    const fs::path folder = model_.folder();
    std::optional<fs::path> active = pendingActive_;
    if (!active_.empty()) {
        active = active_;
    }
    if (!folder.empty()) {
        // Checked first, so that a folder that cannot be opened leaves everything as it was.
        const bool before = includeSubfolders_;
        includeSubfolders_ = include;
        try {
            // The active shot is active again if the new listing brings it.
            listFolder(folder, std::move(active));
        } catch (const std::exception&) {
            includeSubfolders_ = before;
            subfoldersButton_->blockSignals(true);
            subfoldersButton_->setChecked(before);
            subfoldersButton_->blockSignals(false);
            return;
        }
        emit activeChanged();
    } else {
        includeSubfolders_ = include;
    }
    subfoldersButton_->blockSignals(true);
    subfoldersButton_->setChecked(include);
    subfoldersButton_->blockSignals(false);
    emit includeSubfoldersToggled(include);
}

QModelIndex FilmStrip::proxyIndexOf(const fs::path& primary) const {
    const int row = model_.rowOf(primary);
    return row < 0 ? QModelIndex() : proxy_.mapFromSource(model_.index(row, 0));
}

fs::path FilmStrip::primaryAt(const QModelIndex& index) {
    return toPath(index.data(ShotModel::PathRole).toString());
}

void FilmStrip::setActive(const fs::path& primary) {
    if (model_.rowOf(primary) < 0) {
        if (model_.isListing()) {
            // The shot active before is no longer the open one: marks must not go to it while
            // this one is on its way.
            clearActive();
            pendingActive_ = primary;
        }
        return;
    }
    pendingActive_.reset();
    QModelIndex index = proxyIndexOf(primary);
    if (!index.isValid()) {
        // The photograph is open, so the strip has to show it.
        clearFilter();
        index = proxyIndexOf(primary);
    }
    active_ = primary;
    delegate_->setActive(toQString(primary));
    list_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    list_->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
    list_->scrollTo(index, QAbstractItemView::PositionAtCenter);
    list_->viewport()->update();
    emit activeChanged();
}

void FilmStrip::clearActive() {
    active_.clear();
    pendingActive_.reset();
    delegate_->setActive({});
    list_->selectionModel()->clear();
    list_->viewport()->update();
    emit activeChanged();
}

std::optional<fs::path> FilmStrip::activePrimary() const {
    if (active_.empty()) {
        return std::nullopt;
    }
    return active_;
}

std::optional<fs::path> FilmStrip::shotContaining(const fs::path& file) const {
    for (int row = 0; row < model_.rowCount(); ++row) {
        const Shot& shot = model_.shot(row);
        if (shot.primary == file ||
            std::ranges::find(shot.companions, file) != shot.companions.end()) {
            return shot.primary;
        }
    }
    return std::nullopt;
}

std::optional<fs::path> FilmStrip::firstVisible() const {
    if (proxy_.rowCount() == 0) {
        return std::nullopt;
    }
    return primaryAt(proxy_.index(0, 0));
}

PhotoMarks FilmStrip::activeMarks() const {
    const int row = active_.empty() ? -1 : model_.rowOf(active_);
    return row < 0 ? PhotoMarks{} : model_.marks(row);
}

std::vector<fs::path> FilmStrip::targets() const {
    QList<QModelIndex> selected = list_->selectionModel()->selectedIndexes();
    std::ranges::sort(selected, {}, &QModelIndex::row);
    std::vector<fs::path> shots;
    for (const QModelIndex& index : selected) {
        shots.push_back(primaryAt(index));
    }
    if (shots.empty() && !active_.empty() && proxyIndexOf(active_).isValid()) {
        shots.push_back(active_);
    }
    return shots;
}

std::vector<fs::path> FilmStrip::contextTargets(const QModelIndex& index) const {
    auto shots = targets();
    const fs::path clicked = primaryAt(index);
    // A shot outside the selection is the only target; the selection is left alone.
    if (std::ranges::find(shots, clicked) != shots.end() &&
        list_->selectionModel()->isSelected(index)) {
        return shots;
    }
    return {clicked};
}

void FilmStrip::setMarksWriter(MarksWriter writer) {
    writer_ = std::move(writer);
}

void FilmStrip::noteOwnWrite(const fs::path& photo) {
    try {
        watcher_.noteOwnWrite(sidecarPath(photo));
        model_.noteSidecarWritten(photo);
    } catch (const std::exception&) {
        // A photograph with no sidecar name has none that could change.
    }
}

void FilmStrip::setLiveThumbnail(const fs::path& primary, QImage thumbnail) {
    if (model_.rowOf(primary) < 0) {
        return;
    }
    live_ = primary;
    model_.setThumbnail(primary, std::move(thumbnail));
}

void FilmStrip::releaseLiveThumbnail() {
    if (live_.empty()) {
        return;
    }
    const fs::path primary = std::move(live_);
    live_.clear();
    thumbnails_.invalidate(primary);
}

QImage FilmStrip::thumbnail(const fs::path& primary) const {
    const int row = model_.rowOf(primary);
    if (row < 0) {
        return {};
    }
    return model_.data(model_.index(row, 0), ShotModel::ThumbnailRole).value<QImage>();
}

void FilmStrip::noteSettingsSaved(const fs::path& primary) {
    thumbnails_.invalidate(primary);
}

void FilmStrip::receiveThumbnail(const ThumbnailResult& result) {
    if (result.generation != thumbnails_.generation() || result.primary == live_) {
        return;
    }
    model_.setThumbnail(result.primary, result.image);
}

void FilmStrip::reportVisible() {
    const int rows = proxy_.rowCount();
    std::vector<fs::path> shown;
    if (rows > 0) {
        const QRect view = list_->viewport()->rect();
        const int y = view.height() / 2;
        const QModelIndex left = list_->indexAt(QPoint(0, y));
        const QModelIndex right = list_->indexAt(QPoint(view.right(), y));
        // One cell either side, so that a small scroll does not show placeholders.
        const int first = std::max(0, (left.isValid() ? left.row() : 0) - 1);
        const int last = std::min(rows - 1, (right.isValid() ? right.row() : rows - 1) + 1);
        for (int row = first; row <= last; ++row) {
            shown.push_back(primaryAt(proxy_.index(row, 0)));
        }
    }
    thumbnails_.setVisible(std::move(shown));
}

std::vector<PhotoMarks> FilmStrip::currentMarks(const std::vector<fs::path>& shots) const {
    std::vector<PhotoMarks> marks;
    for (const fs::path& shot : shots) {
        const int row = model_.rowOf(shot);
        if (row < 0) {
            marks.emplace_back();
        } else if (model_.index(row, 0).data(ShotModel::MarksLoadedRole).toBool()) {
            marks.push_back(model_.marks(row));
        } else {
            // Not read yet: writing the default would erase what the sidecar has.
            try {
                const auto contents = readSidecar(shot);
                marks.push_back(contents ? contents->marks : PhotoMarks{});
            } catch (const std::exception&) {
                marks.emplace_back();
            }
        }
    }
    return marks;
}

void FilmStrip::applyMarks(const std::vector<fs::path>& shots,
                           const std::function<PhotoMarks(const PhotoMarks&)>& edit) {
    if (!writer_) {
        return;
    }
    const std::vector<PhotoMarks> before = currentMarks(shots);
    QStringList failures;
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const PhotoMarks after = edit(before[i]);
        if (after == before[i]) {
            continue;
        }
        try {
            writer_(shots[i], after);
            noteOwnWrite(shots[i]);
            model_.setMarks(shots[i], after);
        } catch (const std::exception& error) {
            failures << tr("%1: %2").arg(toQString(shots[i].filename()),
                                         QString::fromUtf8(error.what()));
        }
    }
    if (!failures.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot Set Marks"), failures.join('\n'));
    }
}

void FilmStrip::rateShots(const std::vector<fs::path>& shots, int rating) {
    applyMarks(shots, [rating](PhotoMarks marks) {
        marks.rating = rating;
        return marks;
    });
}

void FilmStrip::setLabelOn(const std::vector<fs::path>& shots, std::optional<ColorLabel> label) {
    applyMarks(shots, [label](PhotoMarks marks) {
        marks.label = label;
        return marks;
    });
}

void FilmStrip::toggleLabelOn(const std::vector<fs::path>& shots, ColorLabel label) {
    setLabelOn(shots, toggledLabel(currentMarks(shots), label));
}

void FilmStrip::rate(int rating) {
    rateShots(targets(), rating);
}

void FilmStrip::toggleLabel(ColorLabel label) {
    toggleLabelOn(targets(), label);
}

void FilmStrip::setLabel(std::optional<ColorLabel> label) {
    setLabelOn(targets(), label);
}

void FilmStrip::navigate(int delta) {
    const QModelIndex current = active_.empty() ? QModelIndex() : proxyIndexOf(active_);
    const auto next = adjacentRow(proxy_.rowCount(), current.isValid() ? current.row() : -1, delta);
    if (next) {
        emit activationRequested(proxy_.index(*next, 0).data(ShotModel::PathRole).toString());
    }
}

void FilmStrip::scheduleActiveCheck() {
    if (!active_.empty()) {
        activeCheck_.start();
    }
}

void FilmStrip::ensureActiveShown() {
    const int row = active_.empty() ? -1 : model_.rowOf(active_);
    if (row < 0 || proxyIndexOf(active_).isValid()) {
        return;
    }
    // Nothing shown leaves the photograph where it is: there is nowhere to go.
    if (const auto nearest = proxy_.nearestMatchingSourceRow(row)) {
        emit activationRequested(toQString(model_.shot(*nearest).primary));
    }
}

void FilmStrip::updateEmptyHint() {
    QString text;
    if (proxy_.rowCount() == 0 && !model_.folder().empty()) {
        if (model_.isListing()) {
            text = tr("Listing folder…");
        } else if (model_.rowCount() == 0) {
            text = tr("No photographs in this folder");
        } else if (proxy_.filter().isActive()) {
            text = tr("No shots match the filter");
        }
    }
    const bool show = !text.isEmpty();
    emptyHint_->setText(text);
    if (show) {
        emptyHint_->setGeometry(list_->viewport()->rect());
        emptyHint_->raise();
    }
    emptyHint_->setVisible(show);
}

void FilmStrip::updateCellSide() {
    // The side follows the height; a scroll bar that comes and goes would change it, so it stays.
    // This also keeps a resize from re-entering through the bar.
    if (updatingCells_) {
        return;
    }
    const int available = list_->height() - 2 * list_->frameWidth() -
                          list_->horizontalScrollBar()->sizeHint().height();
    // The caption takes its height from the thumbnail area, unless that gets too small to take it.
    int caption = captionHeight(list_->font());
    int side = cellSide(available - caption);
    if (side - 2 * cellPadding < marksMinHeight) {
        caption = 0;
        side = cellSide(available);
    }
    if (side == delegate_->side() && caption == delegate_->caption()) {
        return;
    }
    updatingCells_ = true;
    delegate_->setSide(side, caption);
    list_->doItemsLayout();
    updatingCells_ = false;
}

void FilmStrip::clickCell(const QModelIndex& index, Qt::KeyboardModifiers modifiers) {
    QItemSelectionModel* selection = list_->selectionModel();
    const QModelIndex activeIndex = active_.empty() ? QModelIndex() : proxyIndexOf(active_);
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        // The range from the active shot to the clicked one; the active shot never moves.
        const int from = activeIndex.isValid() ? activeIndex.row() : index.row();
        selection->select(QItemSelection(proxy_.index(std::min(from, index.row()), 0),
                                         proxy_.index(std::max(from, index.row()), 0)),
                          QItemSelectionModel::ClearAndSelect);
    } else if (modifiers.testFlag(Qt::ControlModifier)) {
        selection->select(index, QItemSelectionModel::Toggle);
        if (activeIndex.isValid()) {
            selection->select(activeIndex, QItemSelectionModel::Select);
        }
    } else if (index == activeIndex) {
        // Back to the active shot alone.
        selection->select(index, QItemSelectionModel::ClearAndSelect);
    } else {
        // The window decides: it may be cancelled, and then nothing here changes.
        emit activationRequested(toQString(primaryAt(index)));
    }
}

void FilmStrip::showContextMenu(const QPoint& position) {
    const QModelIndex index = list_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    const fs::path clicked = primaryAt(index);
    const std::vector<fs::path> shots = contextTargets(index);
    const std::vector<PhotoMarks> marks = currentMarks(shots);

    QMenu menu(this);
    QMenu* rate = menu.addMenu(tr("Rate"));
    const bool sameRating = std::ranges::all_of(
        marks, [&](const PhotoMarks& each) { return each.rating == marks[0].rating; });
    for (int stars = highestRating; stars >= 1; --stars) {
        QAction* action = rate->addAction(QString(stars, QChar(0x2605)));
        action->setCheckable(true);
        action->setChecked(sameRating && marks[0].rating == stars);
        connect(action, &QAction::triggered, this,
                [this, shots, stars] { rateShots(shots, stars); });
    }
    rate->addSeparator();
    connect(rate->addAction(tr("Unrated")), &QAction::triggered, this,
            [this, shots] { rateShots(shots, 0); });
    connect(rate->addAction(tr("Reject")), &QAction::triggered, this,
            [this, shots] { rateShots(shots, rejectedRating); });

    // One label or none, as radio items that set it; the one checked is the
    // label every target shares, and none is checked when they differ.
    QMenu* label = menu.addMenu(tr("Label"));
    auto* labels = new QActionGroup(label);
    labels->setExclusive(true);
    const auto addLabel = [&](const QString& text, std::optional<ColorLabel> value) {
        QAction* action = label->addAction(text);
        action->setCheckable(true);
        action->setActionGroup(labels);
        action->setChecked(std::ranges::all_of(
            marks, [value](const PhotoMarks& each) { return each.label == value; }));
        connect(action, &QAction::triggered, this,
                [this, shots, value] { setLabelOn(shots, value); });
    };
    for (const auto& [value, name] : colorLabelNames) {
        addLabel(tr(labelName(value)), value);
    }
    label->addSeparator();
    addLabel(tr("None"), std::nullopt);

    menu.addSeparator();
    // Opens the folder; selecting the file in it is not portable (Linux has no standard way).
    connect(menu.addAction(tr("Show in File Manager")), &QAction::triggered, this, [clicked] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(clicked.parent_path())));
    });
    // Export belongs to the photograph in the develop view, so it is offered for that one only.
    if (clicked == active_) {
        connect(menu.addAction(tr("Export…")), &QAction::triggered, this,
                &FilmStrip::exportRequested);
    }
    menu.exec(list_->viewport()->mapToGlobal(position));
}

bool FilmStrip::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::Resize) {
        updateCellSide();
        visibleTimer_.start();
        if (emptyHint_->isVisible()) {
            emptyHint_->setGeometry(list_->viewport()->rect());
        }
        return false;
    }
    if (watched != list_->viewport()) {
        return QWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::ContextMenu:
        showContextMenu(static_cast<QContextMenuEvent*>(event)->pos());
        return true;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            return true; // The selection stays; the menu acts on the clicked shot.
        }
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        // The strip does its own selecting: the view would move its current shot, and with it the
        // photograph, before the window has had its say.
        pressHandled_ = true;
        const QModelIndex index = list_->indexAt(mouse->position().toPoint());
        if (index.isValid() && event->type() == QEvent::MouseButtonPress) {
            clickCell(index, mouse->modifiers());
        }
        return true;
    }
    case QEvent::MouseButtonRelease: {
        const auto button = static_cast<QMouseEvent*>(event)->button();
        if (button == Qt::RightButton || (button == Qt::LeftButton && pressHandled_)) {
            pressHandled_ = false;
            return true;
        }
        return false;
    }
    case QEvent::MouseMove:
        return pressHandled_;
    default:
        return QWidget::eventFilter(watched, event);
    }
}

} // namespace arraw::app
