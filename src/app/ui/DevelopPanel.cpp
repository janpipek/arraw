#include "DevelopPanel.h"

#include "CurveEditor.h"
#include "SettingPresentation.h"
#include "SettingSlider.h"
#include "WhiteBalanceChoice.h"

#include <EffectsSettings.h>
#include <SettingDescriptors.h>

#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
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
        finishOtherEdits(row);
        emit editStarted();
    });
    connect(row, &SettingSlider::editFinished, this, &DevelopPanel::editFinished);
    connect(row, &SettingSlider::focusReleased, this, &DevelopPanel::focusReleased);
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

void DevelopPanel::finishOtherEdits(const QObject* keep) {
    for (SettingSlider* other : rows_) {
        if (other != keep) {
            other->finishPendingEdit();
        }
    }
    if (curveEditor_ != nullptr && curveEditor_ != keep) {
        curveEditor_->finishPendingEdit();
    }
}

QWidget* DevelopPanel::buildToneCurveGroup() {
    auto* group = new QGroupBox(tr("Tone Curve"), this);
    auto* groupLayout = new QVBoxLayout(group);

    curveEditor_ = new CurveEditor(group);
    curveEditor_->setObjectName("curveEditor");

    auto* channelRow = new QHBoxLayout;
    channelRow->setSpacing(2);
    auto* channels = new QButtonGroup(group);
    const std::array<QString, curveChannelCount> titles{tr("Luma"), tr("Red"), tr("Green"),
                                                        tr("Blue")};
    for (std::size_t index = 0; index < curveChannelCount; ++index) {
        auto* button = new QPushButton(titles[index], group);
        button->setCheckable(true);
        button->setChecked(index == 0);
        channels->addButton(button, static_cast<int>(index));
        channelRow->addWidget(button, 1);
    }
    auto* reset = new QPushButton(tr("Reset"), group);
    reset->setObjectName("curveReset");
    reset->setToolTip(tr("Straightens the curve of the channel shown."));
    channelRow->addWidget(reset);
    groupLayout->addLayout(channelRow);
    groupLayout->addWidget(curveEditor_);

    auto* readout = new QLabel(group);
    readout->setObjectName("curveReadout");
    readout->setAlignment(Qt::AlignCenter);
    // Room for a line whether or not a point is selected, so the panel does not jump.
    readout->setMinimumHeight(readout->fontMetrics().height());
    groupLayout->addWidget(readout);
    connect(curveEditor_, &CurveEditor::readoutChanged, readout, &QLabel::setText);
    // Whether the histogram is worth counting follows whether the editor is on screen.
    curveEditor_->installEventFilter(this);

    // The channel is view state: choosing one is not an edit.
    connect(channels, &QButtonGroup::idClicked, curveEditor_, [this](int id) {
        curveEditor_->setChannel(curveChannels[static_cast<std::size_t>(id)]);
    });
    connect(reset, &QPushButton::clicked, curveEditor_, &CurveEditor::resetChannel);
    connect(curveEditor_, &CurveEditor::editStarted, this, [this] {
        finishOtherEdits(curveEditor_);
        emit editStarted();
    });
    connect(curveEditor_, &CurveEditor::curveEdited, this, &DevelopPanel::applyCurveEdit);
    connect(curveEditor_, &CurveEditor::editFinished, this, &DevelopPanel::editFinished);
    connect(curveEditor_, &CurveEditor::focusReleased, this, &DevelopPanel::focusReleased);
    return group;
}

QWidget* DevelopPanel::buildColorGroup() {
    auto* group = new QGroupBox(tr("Colour"), this);
    new QVBoxLayout(group);
    for (const std::string_view key : colorKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildColorGradingGroup() {
    auto* group = new QGroupBox(tr("Colour Grading"), this);
    new QVBoxLayout(group);
    for (const std::string_view key : colorGradingKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildNoiseReductionGroup() {
    auto* group = new QGroupBox(tr("Noise Reduction"), this);
    new QVBoxLayout(group);
    for (const std::string_view key : noiseReductionKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildEffectsGroup() {
    auto* group = new QGroupBox(tr("Effects"), this);
    new QVBoxLayout(group);
    for (const std::string_view key : effectsKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildHslGroup() {
    auto* group = new QGroupBox(tr("HSL / Colour Mix"), this);
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
    // On the group, so it shows over the title; the rows keep their own tips.
    group->setToolTip(tr("How each colour becomes grey: drag a band darker or lighter."));
    new QVBoxLayout(group);
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
    layout->addWidget(buildToneCurveGroup());

    colorGroup_ = buildColorGroup();
    hslGroup_ = buildHslGroup();
    blackAndWhiteGroup_ = buildBlackAndWhiteGroup();
    layout->addWidget(colorGroup_);
    layout->addWidget(hslGroup_);
    layout->addWidget(blackAndWhiteGroup_);
    layout->addWidget(buildColorGradingGroup());
    layout->addWidget(buildNoiseReductionGroup());
    layout->addWidget(buildEffectsGroup());
    layout->addStretch(1);
    // One label column for every row, so that the grooves line up across groups.
    int labelWidth = 0;
    for (const SettingSlider* row : rows_) {
        labelWidth = std::max(labelWidth, row->labelWidthHint());
    }
    for (SettingSlider* row : rows_) {
        row->setLabelWidth(labelWidth);
    }
    showState(shown_, PanelContext{});
    watchViewport();
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

    curveEditor_->setCurves(shown_.settings.toneCurve);

    const ColourTemperature light = shownLight(color, context.asShot.value_or(fallbackLight));
    for (SettingSlider* row : rows_) {
        const FieldDescriptor& descriptor = *findDescriptor(row->key());
        row->setVisible(context.raw || descriptor.applies != Applicability::RawOnly);
        // What a reset restores is the photograph's default, which can depend
        // on its kind; an optional setting is cleared instead.
        row->setDefaultValue(visitField(descriptor, context.defaults, [](const auto& field) {
            if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(field)>>) {
                return static_cast<double>(field);
            } else {
                return 0.0;
            }
        }));
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
    finishOtherEdits(nullptr);
}

void DevelopPanel::showCurveHistogram(const CurveHistogram& histogram) {
    curveEditor_->setHistogram(histogram);
}

void DevelopPanel::clearCurveHistogram() {
    curveEditor_->setHistogram(std::nullopt);
}

void DevelopPanel::applyCurveEdit(CurveChannel channel, const ToneCurve& curve) {
    DevelopState next = shown_;
    curveOf(next.settings.toneCurve, channel) = curve;
    emit stateEdited(next);
}

bool DevelopPanel::curveHistogramWanted() const {
    return curveHistogramWanted_;
}

void DevelopPanel::updateCurveHistogramWanted() {
    if (curveEditor_ == nullptr) {
        return; // Still being built.
    }
    // Hidden with the dock or a hidden ancestor, or scrolled out of the
    // scroll area: either way no part of it is on screen.
    const bool wanted = curveEditor_->isVisible() && !curveEditor_->visibleRegion().isEmpty();
    if (wanted != curveHistogramWanted_) {
        curveHistogramWanted_ = wanted;
        emit curveHistogramWantedChanged(wanted);
    }
}

bool DevelopPanel::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Move:   // Scrolling moves the panel inside the scroll area's viewport.
    case QEvent::Resize: // So does a resize, and it changes what fits.
    case QEvent::Show:
    case QEvent::Hide:
        updateCurveHistogramWanted();
        break;
    case QEvent::ParentChange:
        watchViewport();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

bool DevelopPanel::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if ((watched == curveEditor_ &&
         (type == QEvent::Show || type == QEvent::Hide || type == QEvent::Paint)) ||
        (watched == viewport_ && type == QEvent::Resize)) {
        // A paint is how an editor scrolled back into view first shows it.
        updateCurveHistogramWanted();
    }
    return QWidget::eventFilter(watched, event);
}

void DevelopPanel::watchViewport() {
    if (viewport_ != nullptr) {
        viewport_->removeEventFilter(this);
    }
    viewport_ = parentWidget();
    if (viewport_ != nullptr) {
        viewport_->installEventFilter(this);
    }
    updateCurveHistogramWanted();
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
    // Grain this edit turns on gets its photograph's own seed (ADR 038); every
    // other edit, and turning it off, keeps the one it has, zero included.
    next.settings.effects.grain.seed =
        chooseGrainSeed(shown_.settings.effects.grain, next.settings.effects.grain);
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
