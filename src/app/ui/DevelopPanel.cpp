#include "DevelopPanel.h"

#include "SettingPresentation.h"
#include "SettingSlider.h"
#include "WhiteBalanceChoice.h"

#include <SettingDescriptors.h>

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <type_traits>

namespace arraw::app {

namespace {

/// What the White Balance rows show when the camera's reading is not known.
constexpr ColourTemperature fallbackLight{5500.0F, 0.0F};

} // namespace

SettingSlider* DevelopPanel::addRow(std::string_view key, QWidget* group) {
    auto* row = new SettingSlider(key, group);
    group->layout()->addWidget(row);
    rows_.push_back(row);
    connect(row, &SettingSlider::editStarted, this, [this, row] {
        // One edit at a time: another row's pending edit ends before this one begins.
        for (SettingSlider* other : rows_) {
            if (other != row) {
                other->finishPendingEdit();
            }
        }
        emit editStarted();
    });
    connect(row, &SettingSlider::editFinished, this, &DevelopPanel::editFinished);
    connect(row, &SettingSlider::valueEdited, this,
            [this, row](double value) { applyEdit(*row, value); });
    connect(row, &SettingSlider::valueCleared, this, [this, row] { applyClear(*row); });
    return row;
}

QWidget* DevelopPanel::buildWhiteBalanceGroup() {
    auto* group = new QGroupBox(tr("White Balance"), this);
    auto* groupLayout = new QVBoxLayout(group);

    presetCombo_ = new QComboBox(group);
    for (const WhiteBalanceChoice choice : whiteBalanceChoices()) {
        presetCombo_->addItem(nameOf(choice), static_cast<int>(choice));
    }
    pickButton_ = new QPushButton(tr("Pick"), group);
    pickButton_->setCheckable(true);
    pickButton_->setToolTip(
        tr("Pick a neutral grey or white area in the image to set white balance"));

    auto* pickRow = new QHBoxLayout;
    pickRow->addWidget(presetCombo_, 1);
    pickRow->addWidget(pickButton_);
    groupLayout->addLayout(pickRow);

    connect(presetCombo_, &QComboBox::activated, this, &DevelopPanel::applyChoice);
    connect(pickButton_, &QPushButton::toggled, this, &DevelopPanel::pickToggled);

    for (const std::string_view key : whiteBalanceKeys()) {
        addRow(key, group);
    }
    return group;
}

DevelopPanel::DevelopPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(buildWhiteBalanceGroup());

    auto* tone = new QGroupBox(tr("Tone"), this);
    new QVBoxLayout(tone);
    for (const std::string_view key : toneKeys()) {
        addRow(key, tone);
    }
    layout->addWidget(tone);
    layout->addStretch(1);
    showState(shown_, PanelContext{});
}

void DevelopPanel::showState(const DevelopState& state, const PanelContext& context) {
    shown_ = state;
    const ColorSettings& color = shown_.settings.color;

    {
        const QSignalBlocker blocker(presetCombo_);
        presetCombo_->setCurrentIndex(presetCombo_->findData(static_cast<int>(choiceOf(color))));
    }
    const QString needsRaw = tr("White balance needs a RAW file for now");
    presetCombo_->setEnabled(context.raw);
    presetCombo_->setToolTip(context.raw ? QString() : needsRaw);
    pickButton_->setEnabled(context.raw);
    pickButton_->setToolTip(context.raw ? tr("Pick a neutral grey or white area in the image to "
                                             "set white balance")
                                        : needsRaw);

    const ColourTemperature light = shownLight(color, context.asShot.value_or(fallbackLight));
    for (SettingSlider* row : rows_) {
        const FieldDescriptor& descriptor = *findDescriptor(row->key());
        row->setVisible(context.raw || descriptor.applies != Applicability::RawOnly);
        if (row->key() == "temperature") {
            row->setValue(light.kelvin);
        } else if (row->key() == "tint") {
            row->setValue(light.tint);
        } else {
            row->setValue(visitField(descriptor, shown_.settings, [](const auto& field) -> double {
                if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(field)>>) {
                    return static_cast<double>(field);
                } else {
                    return 0.0;
                }
            }));
        }
    }
}

void DevelopPanel::setPicking(bool picking) {
    const QSignalBlocker blocker(pickButton_);
    pickButton_->setChecked(picking);
}

void DevelopPanel::finishPendingEdit() {
    for (SettingSlider* row : rows_) {
        row->finishPendingEdit();
    }
}

void DevelopPanel::applyEdit(const SettingSlider& row, double value) {
    DevelopState next = shown_;
    if (row.key() == "temperature") {
        next.settings.color = withTemperature(next.settings.color, static_cast<float>(value));
    } else if (row.key() == "tint") {
        next.settings.color = withTint(next.settings.color, static_cast<float>(value));
    } else {
        visitField(*findDescriptor(row.key()), next.settings, [value](auto& field) {
            using Field = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::is_same_v<Field, float> || std::is_same_v<Field, double>) {
                field = static_cast<Field>(value);
            }
        });
    }
    emit stateEdited(next);
}

void DevelopPanel::applyClear(const SettingSlider& row) {
    DevelopState next = shown_;
    if (row.key() == "temperature") {
        next.settings.color = withTemperature(next.settings.color, std::nullopt);
    } else if (row.key() == "tint") {
        next.settings.color = withTint(next.settings.color, std::nullopt);
    }
    emit stateEdited(next);
}

void DevelopPanel::applyChoice(int index) {
    const auto choice = static_cast<WhiteBalanceChoice>(presetCombo_->itemData(index).toInt());
    DevelopState next = shown_;
    next.settings.color = withChoice(next.settings.color, choice);
    if (next.settings.color == shown_.settings.color) {
        // The Custom entry, or the light already set: nothing to edit, and the
        // combo goes back to describing the settings.
        const QSignalBlocker blocker(presetCombo_);
        presetCombo_->setCurrentIndex(
            presetCombo_->findData(static_cast<int>(choiceOf(shown_.settings.color))));
        return;
    }
    finishPendingEdit();
    emit editStarted();
    emit stateEdited(next);
    emit editFinished();
}

} // namespace arraw::app
