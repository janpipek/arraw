#include "MasksPanel.h"

#include "MaskPresentation.h"
#include "MasksModel.h"
#include "SettingPresentation.h"
#include "SettingSlider.h"

#include <LocalAdjustmentEdits.h>
#include <SettingDescriptors.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPalette>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

namespace arraw::app {

namespace {

/// Rows of the list shown before it scrolls.
constexpr int visibleListRows = 5;

/// Prefix of the ids of the rows, which keeps them apart from the global rows.
const QString idPrefix = QStringLiteral("local.");

/// @brief Makes a compact button for a row of several: as wide as its text, sharing the row.
QToolButton* compactButton(const QString& text, const QString& toolTip, const char* name,
                           QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setObjectName(name);
    button->setToolTip(toolTip);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Reached by Tab, but a click leaves the focus on the photograph, which claims the mode's
    // keys.
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

/// @brief Delegate that shows an inverted mask's mark and dims a disabled mask, as a disabled
/// control is.
class MaskDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.data(MasksModel::InvertedRole).toBool()) {
            option->text += QStringLiteral(" ") + MasksPanel::tr("(inverted)");
        }
        if (index.data(Qt::CheckStateRole).toInt() != Qt::Checked) {
            option->palette.setColor(QPalette::Text,
                                     option->palette.color(QPalette::Disabled, QPalette::Text));
            option->palette.setColor(
                QPalette::HighlightedText,
                option->palette.color(QPalette::Disabled, QPalette::HighlightedText));
        }
    }
};

} // namespace

MasksPanel::MasksPanel(QWidget* parent) : QGroupBox(tr("Masks"), parent) {
    setObjectName("masksPanel");
    auto* layout = new QVBoxLayout(this);

    // Linear and Radial arm a tool, Overlay shows the tint; the rest act on the selection.
    linear_ = compactButton(tr("Linear"), QString(), "maskLinear", this);
    radial_ = compactButton(tr("Radial"), QString(), "maskRadial", this);
    for (QToolButton* button : {linear_, radial_}) {
        button->setCheckable(true);
    }
    duplicate_ =
        compactButton(tr("Duplicate"), tr("Copies the selected mask."), "maskDuplicate", this);
    remove_ =
        compactButton(tr("Delete"), tr("Removes the selected mask (Delete)."), "maskDelete", this);
    up_ = compactButton(QString::fromUtf8("↑"),
                        tr("Moves the selected mask up the list; masks add up in that order."),
                        "maskMoveUp", this);
    down_ = compactButton(QString::fromUtf8("↓"), tr("Moves the selected mask down the list."),
                          "maskMoveDown", this);
    overlay_ = compactButton(tr("Overlay"), tr("Shows where the selected mask applies (O)."),
                             "maskOverlay", this);
    overlay_->setCheckable(true);
    for (QToolButton* button : {up_, down_}) {
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    up_->setAccessibleName(tr("Move Up"));
    down_->setAccessibleName(tr("Move Down"));

    auto* creation = new QHBoxLayout;
    creation->setSpacing(2);
    creation->addWidget(linear_, 1);
    creation->addWidget(radial_, 1);
    creation->addWidget(overlay_, 1);
    layout->addLayout(creation);

    model_ = new MasksModel(this);
    list_ = new QListView(this);
    list_->setObjectName("maskList");
    list_->setModel(model_);
    list_->setItemDelegate(new MaskDelegate(list_));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    list_->setFocusPolicy(Qt::StrongFocus);
    list_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    list_->setToolTip(tr("Double-click a mask to rename it; the check box turns it on or off."));
    layout->addWidget(list_);

    count_ = new QLabel(this);
    count_->setObjectName("maskCount");
    auto* actions = new QHBoxLayout;
    actions->setSpacing(2);
    actions->addWidget(duplicate_, 1);
    actions->addWidget(remove_, 1);
    actions->addWidget(up_);
    actions->addWidget(down_);
    actions->addWidget(count_);
    layout->addLayout(actions);

    hint_ = new QLabel(tr("Select a mask, or add one with Linear or Radial."), this);
    hint_->setObjectName("maskHint");
    hint_->setWordWrap(true);
    hint_->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint_);

    controls_ = new QWidget(this);
    controls_->setObjectName("maskControls");
    auto* controlsLayout = new QVBoxLayout(controls_);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    invert_ = new QCheckBox(tr("Invert"), controls_);
    invert_->setObjectName("maskInvert");
    invert_->setToolTip(tr("Applies the mask outside its shape instead of inside."));
    invert_->setFocusPolicy(Qt::TabFocus);
    controlsLayout->addWidget(invert_);
    opacity_ = addRow(idPrefix + "opacity");
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        rows_.push_back(
            addRow(idPrefix + QString::fromUtf8(descriptor.key.data(),
                                                static_cast<qsizetype>(descriptor.key.size()))));
    }
    layout->addWidget(controls_);

    connect(linear_, &QToolButton::clicked, this, [this](bool checked) {
        radial_->setChecked(false);
        emit maskToolChosen(checked ? MaskTool::Linear : MaskTool::None);
    });
    connect(radial_, &QToolButton::clicked, this, [this](bool checked) {
        linear_->setChecked(false);
        emit maskToolChosen(checked ? MaskTool::Radial : MaskTool::None);
    });
    connect(overlay_, &QToolButton::clicked, this, &MasksPanel::overlayToggled);
    connect(duplicate_, &QToolButton::clicked, this, [this] {
        if (!selected_ || !canAddLocalAdjustment(shown_)) {
            return;
        }
        // The copy takes the counter's value (withLocalAdjustmentDuplicated).
        const LocalAdjustmentId copy = shown_.nextLocalAdjustmentId;
        applyWhole(withLocalAdjustmentDuplicated(shown_, *selected_));
        emit maskSelected(copy);
    });
    connect(remove_, &QToolButton::clicked, this, [this] {
        if (selected_) {
            applyWhole(withLocalAdjustmentRemoved(shown_, *selected_));
        }
    });
    connect(up_, &QToolButton::clicked, this, [this] { moveSelected(-1); });
    connect(down_, &QToolButton::clicked, this, [this] { moveSelected(1); });
    connect(invert_, &QCheckBox::clicked, this, [this](bool checked) {
        if (selected_) {
            applyWhole(withLocalAdjustmentInverted(shown_, *selected_, checked));
        }
    });
    connect(model_, &MasksModel::renameRequested, this,
            [this](LocalAdjustmentId id, const QString& name) {
                if (findLocalAdjustment(shown_, id) != nullptr) {
                    applyWhole(withLocalAdjustmentRenamed(
                        shown_, id, storableMaskName(name.trimmed().toStdString())));
                }
                emit focusReleased();
            });
    connect(model_, &MasksModel::enabledRequested, this, [this](LocalAdjustmentId id, bool on) {
        if (findLocalAdjustment(shown_, id) != nullptr) {
            applyWhole(withLocalAdjustmentEnabled(shown_, id, on));
        }
    });
    const auto selectRow = [this](const QModelIndex& index) {
        if (showing_) {
            return;
        }
        const auto id = model_->idAt(index.isValid() ? index.row() : -1);
        if (!id) {
            return;
        }
        if (id != selected_) {
            selected_ = id;
            showSelected();
        }
        emit maskSelected(id);
    };
    connect(list_, &QListView::clicked, this, [this, selectRow](const QModelIndex& index) {
        selectRow(index);
        // A click asks to work on the photograph; the keys leave the focus where it is.
        emit maskClicked();
    });
    connect(list_, &QListView::activated, this, selectRow);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [selectRow](const QModelIndex& current) { selectRow(current); });

    showState(DevelopState{});
}

SettingSlider* MasksPanel::addRow(const QString& id) {
    SettingSlider* row = nullptr;
    if (id == idPrefix + "opacity") {
        row = new SettingSlider(id.toStdString(), maskOpacityRange, maskOpacityRange.maximum,
                                maskOpacityPresentation(), controls_);
    } else {
        const std::string key = id.mid(idPrefix.size()).toStdString();
        const LocalDescriptor& descriptor = *findLocalDescriptor(key);
        row = new SettingSlider(id.toStdString(), descriptor.range, 0.0,
                                localPresentationOf(descriptor.key), controls_);
    }
    controls_->layout()->addWidget(row);
    connect(row, &SettingSlider::editStarted, this, [this, row] {
        // One edit at a time: another row's pending edit ends before this one begins.
        finishOtherEdits(row);
        emit editStarted();
    });
    connect(row, &SettingSlider::editFinished, this, &MasksPanel::editFinished);
    connect(row, &SettingSlider::focusReleased, this, &MasksPanel::focusReleased);
    connect(row, &SettingSlider::valueEdited, this, [this, row](double value) {
        if (!selected_) {
            return;
        }
        const std::string key(row->key());
        if (row == opacity_) {
            emit stateEdited(
                withLocalOpacity(shown_, *selected_, static_cast<float>(value / 100.0)));
        } else {
            emit stateEdited(
                withLocalDelta(shown_, *selected_, key.substr(idPrefix.size()), value));
        }
    });
    return row;
}

void MasksPanel::finishOtherEdits(const SettingSlider* keep) {
    for (SettingSlider* row : findChildren<SettingSlider*>()) {
        if (row != keep) {
            row->finishPendingEdit();
        }
    }
}

void MasksPanel::finishPendingEdit() {
    finishOtherEdits(nullptr);
}

void MasksPanel::applyWhole(const DevelopState& next) {
    if (next == shown_) {
        return;
    }
    finishPendingEdit();
    emit editStarted();
    emit stateEdited(next);
    emit editFinished();
}

void MasksPanel::moveSelected(int places) {
    if (!selected_) {
        return;
    }
    const int row = model_->rowOf(*selected_);
    const int target = row + places;
    if (row < 0 || target < 0 || target >= model_->rowCount()) {
        return;
    }
    applyWhole(withLocalAdjustmentReordered(shown_, *selected_, static_cast<std::size_t>(target)));
}

void MasksPanel::showState(const DevelopState& state) {
    shown_ = state;
    showing_ = true;
    model_->setState(state);
    if (selected_ && findLocalAdjustment(state, *selected_) == nullptr) {
        selected_.reset();
    }
    const int rows = std::clamp(model_->rowCount(), 2, visibleListRows);
    const int rowHeight = std::max(list_->sizeHintForRow(0), fontMetrics().height() + 4);
    list_->setFixedHeight(rows * rowHeight + 2 * list_->frameWidth());
    showSelected();
    showing_ = false;
}

void MasksPanel::setSelectedMask(std::optional<LocalAdjustmentId> id) {
    selected_ = id && findLocalAdjustment(shown_, *id) != nullptr ? id : std::nullopt;
    showing_ = true;
    showSelected();
    showing_ = false;
}

void MasksPanel::showSelected() {
    const bool was = showing_;
    showing_ = true;
    const LocalAdjustment* mask = selected_ ? findLocalAdjustment(shown_, *selected_) : nullptr;
    const int row = mask != nullptr ? model_->rowOf(mask->id) : -1;
    if (row >= 0) {
        list_->setCurrentIndex(model_->index(row));
    } else {
        list_->selectionModel()->clear();
        list_->setCurrentIndex({});
    }
    hint_->setVisible(mask == nullptr);
    controls_->setVisible(mask != nullptr);
    if (mask != nullptr) {
        {
            const QSignalBlocker blocker(invert_);
            invert_->setChecked(mask->invert);
        }
        opacity_->setValue(static_cast<double>(mask->opacity) * 100.0);
        const bool grayscale = shown_.settings.blackAndWhite.convertToGrayscale;
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            const LocalDescriptor& descriptor = localAdjustmentDescriptors[index];
            rows_[index]->setValue(static_cast<double>(mask->deltas.*(descriptor.member)));
            // As the global Colour group, which the Black & White treatment hides.
            const bool colour = descriptor.key == "saturation" || descriptor.key == "vibrance";
            rows_[index]->setVisible(!(colour && grayscale));
        }
    }
    updateButtons();
    showing_ = was;
}

void MasksPanel::updateButtons() {
    const bool room = canAddLocalAdjustment(shown_);
    const QString full = tr("A photograph holds at most %1 masks.").arg(maximumLocalAdjustments);
    // An armed tool stays unchecked-able at the limit, so that Esc and a click can disarm it.
    linear_->setEnabled(room || linear_->isChecked());
    radial_->setEnabled(room || radial_->isChecked());
    duplicate_->setEnabled(room && selected_.has_value());
    linear_->setToolTip(room ? tr("Draws a graduated mask: drag from where it applies fully to "
                                  "where it fades out, or click for a default one.")
                             : full);
    radial_->setToolTip(room ? tr("Draws an oval mask: drag from its centre outwards, or click "
                                  "for a default one.")
                             : full);
    duplicate_->setToolTip(room ? tr("Copies the selected mask.") : full);
    remove_->setEnabled(selected_.has_value());
    const int row = selected_ ? model_->rowOf(*selected_) : -1;
    up_->setEnabled(row > 0);
    down_->setEnabled(row >= 0 && row + 1 < model_->rowCount());
    count_->setText(tr("%1 of %2").arg(model_->rowCount()).arg(maximumLocalAdjustments));
}

void MasksPanel::setTool(MaskTool tool) {
    const QSignalBlocker linear(linear_);
    const QSignalBlocker radial(radial_);
    linear_->setChecked(tool == MaskTool::Linear);
    radial_->setChecked(tool == MaskTool::Radial);
    updateButtons();
}

void MasksPanel::setOverlayShown(bool shown) {
    const QSignalBlocker blocker(overlay_);
    overlay_->setChecked(shown);
}

int MasksPanel::labelWidthHint() const {
    int width = 0;
    for (const SettingSlider* row : findChildren<SettingSlider*>()) {
        width = std::max(width, row->labelWidthHint());
    }
    return width;
}

void MasksPanel::setLabelWidth(int width) {
    for (SettingSlider* row : findChildren<SettingSlider*>()) {
        row->setLabelWidth(width);
    }
}

} // namespace arraw::app
