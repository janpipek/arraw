#include "DevelopPanel.h"

#include "SettingPresentation.h"
#include "SettingSlider.h"
#include "WhiteBalanceChoice.h"

#include <SettingDescriptors.h>

#include <QButtonGroup>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <array>
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

QWidget* DevelopPanel::buildTreatmentRow() {
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(2);
    rowLayout->addWidget(new QLabel(tr("Treatment"), row));

    treatment_ = new QButtonGroup(this);
    const auto addButton = [&](const QString& text, int id, const char* name) {
        auto* button = new QPushButton(text, row);
        button->setObjectName(name);
        button->setCheckable(true);
        treatment_->addButton(button, id);
        rowLayout->addWidget(button, 1);
    };
    addButton(tr("Colour"), 0, "treatmentColour");
    addButton(tr("B&&W"), 1, "treatmentBw");
    treatment_->button(0)->setChecked(true);
    connect(treatment_, &QButtonGroup::idClicked, this,
            [this](int id) { applyTreatment(id == 1); });
    return row;
}

QWidget* DevelopPanel::buildColorGroup() {
    auto* group = new QGroupBox(tr("Color"), this);
    new QVBoxLayout(group);
    for (const std::string_view key : colorKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildHslGroup() {
    auto* group = new QGroupBox(tr("HSL / Color Mix"), this);
    auto* groupLayout = new QVBoxLayout(group);

    auto* tabRow = new QHBoxLayout;
    tabRow->setSpacing(2);
    auto* tabs = new QButtonGroup(group);
    auto* stack = new QStackedWidget(group);
    const std::array<QString, hslPageCount> titles{tr("Hue"), tr("Saturation"), tr("Luminance")};
    for (int page = 0; page < hslPageCount; ++page) {
        auto* pageWidget = new QWidget(stack);
        auto* pageLayout = new QVBoxLayout(pageWidget);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        for (const std::string_view key : hslKeys(page)) {
            addRow(key, pageWidget);
        }
        stack->addWidget(pageWidget);

        auto* button = new QPushButton(titles[static_cast<std::size_t>(page)], group);
        button->setCheckable(true);
        button->setChecked(page == 0);
        tabs->addButton(button, page);
        tabRow->addWidget(button);
    }
    // The page is view state: choosing one is not an edit.
    connect(tabs, &QButtonGroup::idClicked, stack, &QStackedWidget::setCurrentIndex);
    groupLayout->addLayout(tabRow);
    groupLayout->addWidget(stack);
    return group;
}

QWidget* DevelopPanel::buildBlackAndWhiteGroup() {
    auto* group = new QGroupBox(tr("Black && White"), this);
    auto* groupLayout = new QVBoxLayout(group);
    auto* hint =
        new QLabel(tr("How each colour becomes grey: drag a band darker or lighter."), group);
    hint->setWordWrap(true);
    groupLayout->addWidget(hint);
    for (const std::string_view key : blackAndWhiteKeys()) {
        addRow(key, group);
    }
    return group;
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
    layout->addWidget(buildTreatmentRow());
    layout->addWidget(buildWhiteBalanceGroup());

    auto* tone = new QGroupBox(tr("Tone"), this);
    new QVBoxLayout(tone);
    for (const std::string_view key : toneKeys()) {
        addRow(key, tone);
    }
    layout->addWidget(tone);

    colorGroup_ = buildColorGroup();
    hslGroup_ = buildHslGroup();
    blackAndWhiteGroup_ = buildBlackAndWhiteGroup();
    layout->addWidget(colorGroup_);
    layout->addWidget(hslGroup_);
    layout->addWidget(blackAndWhiteGroup_);
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

    const bool grayscale = shown_.settings.blackAndWhite.convertToGrayscale;
    {
        const QSignalBlocker blocker(treatment_);
        treatment_->button(grayscale ? 1 : 0)->setChecked(true);
    }
    const TreatmentVisibility visible = visibleGroups(grayscale);
    colorGroup_->setVisible(visible.color);
    hslGroup_->setVisible(visible.hsl);
    blackAndWhiteGroup_->setVisible(visible.blackAndWhiteMix);

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

void DevelopPanel::applyTreatment(bool grayscale) {
    if (grayscale == shown_.settings.blackAndWhite.convertToGrayscale) {
        return;
    }
    DevelopState next = shown_;
    next.settings.blackAndWhite.convertToGrayscale = grayscale;
    finishPendingEdit();
    emit editStarted();
    emit stateEdited(next);
    emit editFinished();
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
