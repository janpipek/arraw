#include "DevelopPanel.h"

#include "CropEditing.h"
#include "CurveEditor.h"
#include "HistoryModel.h"
#include "SettingPresentation.h"
#include "SettingSlider.h"
#include "WhiteBalanceChoice.h"

#include <Edits.h>
#include <SettingDescriptors.h>

#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace arraw::app {

namespace {

/// Gives the title of a group box: the group's display name, with `&` escaped for the mnemonics.
QString groupTitle(SettingGroup group) {
    return groupDisplayName(group).replace('&', QStringLiteral("&&"));
}

/// What the White Balance rows show when the camera's reading is not known.
constexpr ColourTemperature fallbackLight{5500.0F, 0.0F};

/// What an entry of the aspect menu stands for.
enum class AspectEntry {
    Original,
    Square,
    FourByFive,
    FiveBySeven,
    TwoByThree,
    SixteenByNine,
    Custom,
    Free
};

/// A menu entry that is a fixed ratio: its label and its landscape width over height.
struct PresetRatio {
    AspectEntry entry;
    const char* label;
    double widthOverHeight;
};

constexpr std::array<PresetRatio, 5> presetRatios{{
    {AspectEntry::Square, "1:1", 1.0},
    {AspectEntry::FourByFive, "4:5", 5.0 / 4.0},
    {AspectEntry::FiveBySeven, "5:7", 7.0 / 5.0},
    {AspectEntry::TwoByThree, "2:3", 3.0 / 2.0},
    {AspectEntry::SixteenByNine, "16:9", 16.0 / 9.0},
}};

/// How close two ratios must be to be the same preset.
constexpr double ratioTolerance = 1e-3;

/// Room the develop dock opens with beyond its minimum width, in lines of the panel's font.
constexpr double dockSlackInLines = 4.0;

/// @brief Makes a compact button for a row of several: as wide as its text, sharing the row.
///
/// A QToolButton rather than a QPushButton, whose style minimum (about 80
/// pixels) made rows of four or five buttons wider than the dock.
QToolButton* compactButton(const QString& text, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

/// Gives the menu's entry for an aspect, and for a ratio of its own the text to show.
AspectEntry entryFor(const CropAspect& aspect) {
    if (std::holds_alternative<FreeCropAspect>(aspect)) {
        return AspectEntry::Free;
    }
    if (std::holds_alternative<OriginalCropAspect>(aspect)) {
        return AspectEntry::Original;
    }
    const double ratio = std::get<CropRatio>(aspect).widthOverHeight;
    const double landscape = std::max(ratio, 1.0 / ratio);
    for (const PresetRatio& preset : presetRatios) {
        if (std::abs(landscape - preset.widthOverHeight) < ratioTolerance) {
            return preset.entry;
        }
    }
    return AspectEntry::Custom;
}

} // namespace

SettingSlider* DevelopPanel::addRow(std::string_view key, QWidget* group) {
    auto* row = new SettingSlider(key, group);
    group->layout()->addWidget(row);
    rows_.push_back(row);
    connect(row, &SettingSlider::editStarted, this, [this, row] {
        // One edit at a time: another row's pending edit ends before this one begins.
        finishOtherEdits(row);
        // The baseline of a run of straighten edits: SettingSlider starts every edit before its
        // first value.
        editStart_ = shown_;
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
    auto* group = new QGroupBox(groupTitle(SettingGroup::ToneCurve), this);
    auto* groupLayout = new QVBoxLayout(group);

    curveEditor_ = new CurveEditor(group);
    curveEditor_->setObjectName("curveEditor");

    auto* channelRow = new QHBoxLayout;
    channelRow->setSpacing(2);
    auto* channels = new QButtonGroup(group);
    const std::array<QString, curveChannelCount> titles{tr("Luma"), tr("Red"), tr("Green"),
                                                        tr("Blue")};
    for (std::size_t index = 0; index < curveChannelCount; ++index) {
        QToolButton* button = compactButton(titles[index], group);
        button->setCheckable(true);
        button->setChecked(index == 0);
        channels->addButton(button, static_cast<int>(index));
        channelRow->addWidget(button, 1);
    }
    QToolButton* reset = compactButton(tr("Reset"), group);
    reset->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
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
    connect(reset, &QToolButton::clicked, curveEditor_, &CurveEditor::resetChannel);
    connect(curveEditor_, &CurveEditor::editStarted, this, [this] {
        finishOtherEdits(curveEditor_);
        emit editStarted();
    });
    connect(curveEditor_, &CurveEditor::curveEdited, this, &DevelopPanel::applyCurveEdit);
    connect(curveEditor_, &CurveEditor::editFinished, this, &DevelopPanel::editFinished);
    connect(curveEditor_, &CurveEditor::resetFinished, this, &DevelopPanel::finishResetEdit);
    connect(curveEditor_, &CurveEditor::focusReleased, this, &DevelopPanel::focusReleased);
    return group;
}

QWidget* DevelopPanel::buildColorGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::Color), this);
    new QVBoxLayout(group);
    for (const std::string_view key : colorKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildPresenceGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::Presence), this);
    new QVBoxLayout(group);
    for (const std::string_view key : presenceKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildColorGradingGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::ColorGrading), this);
    new QVBoxLayout(group);
    for (const std::string_view key : colorGradingKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildNoiseReductionGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::Detail), this);
    new QVBoxLayout(group);
    for (const std::string_view key : noiseReductionKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildEffectsGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::Effects), this);
    new QVBoxLayout(group);
    for (const std::string_view key : effectsKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildHslGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::Hsl), this);
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
    auto* group = new QGroupBox(groupTitle(SettingGroup::BlackAndWhite), this);
    // On the group, so it shows over the title; the rows keep their own tips.
    group->setToolTip(tr("How each colour becomes grey: drag a band darker or lighter."));
    new QVBoxLayout(group);
    for (const std::string_view key : blackAndWhiteKeys()) {
        addRow(key, group);
    }
    return group;
}

QWidget* DevelopPanel::buildCropGroup() {
    auto* group = new QGroupBox(groupTitle(SettingGroup::Geometry), this);
    auto* groupLayout = new QVBoxLayout(group);

    // None of these takes the focus: in the crop mode it stays on the overlay, which claims
    // Enter, Esc, O and X there (ADR 040). Only the Angle row's spin box takes it, to type.
    cropButton_ = new QPushButton(tr("Crop"), group);
    cropButton_->setObjectName("cropMode");
    cropButton_->setCheckable(true);
    cropButton_->setFocusPolicy(Qt::NoFocus);
    cropButton_->setToolTip(tr("Frame the photograph on screen: drag the handles to crop, drag "
                               "inside to move the photograph, outside to turn it (C)."));
    levelButton_ = new QPushButton(tr("Level"), group);
    levelButton_->setObjectName("cropLevel");
    levelButton_->setCheckable(true);
    levelButton_->setFocusPolicy(Qt::NoFocus);
    levelButton_->setToolTip(
        tr("Draw a line along a horizon or an upright, and the photograph is turned to make it "
           "level. Ctrl-drag does the same."));
    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(2);
    modeRow->addWidget(cropButton_, 1);
    modeRow->addWidget(levelButton_, 1);
    groupLayout->addLayout(modeRow);

    aspectCombo_ = new QComboBox(group);
    aspectCombo_->setObjectName("cropAspect");
    aspectCombo_->setFocusPolicy(Qt::NoFocus);
    aspectCombo_->setToolTip(tr("The shape of the crop."));
    // Sized to the entries, not to "Custom (1.78:1)", which may replace one of them.
    aspectCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    aspectCombo_->setMinimumContentsLength(8);
    aspectCombo_->addItem(tr("Original"), static_cast<int>(AspectEntry::Original));
    for (const PresetRatio& preset : presetRatios) {
        aspectCombo_->addItem(QString::fromLatin1(preset.label), static_cast<int>(preset.entry));
    }
    aspectCombo_->addItem(tr("Custom\u2026"), static_cast<int>(AspectEntry::Custom));
    aspectCombo_->addItem(tr("Free"), static_cast<int>(AspectEntry::Free));
    QToolButton* lock = compactButton(tr("Lock"), group);
    lock->setObjectName("cropLock");
    lock->setCheckable(true);
    lock->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    lock->setToolTip(tr("Keeps the crop's shape while it is resized."));
    lockButton_ = lock;
    QToolButton* swapButton = compactButton(tr("Swap"), group);
    swapButton->setObjectName("cropSwap");
    swapButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    swapButton->setToolTip(tr("Swaps portrait and landscape (X in the crop mode)."));
    auto* aspectRow = new QHBoxLayout;
    aspectRow->setSpacing(2);
    aspectRow->addWidget(aspectCombo_, 1);
    aspectRow->addWidget(lock);
    aspectRow->addWidget(swapButton);
    groupLayout->addLayout(aspectRow);

    for (const std::string_view key : geometryKeys()) {
        addRow(key, group);
    }

    const auto addTool = [&](const QString& text, const QString& tip, const char* name) {
        QToolButton* button = compactButton(text, group);
        button->setObjectName(name);
        button->setToolTip(tip);
        return button;
    };
    QToolButton* turnLeft =
        addTool(QString::fromUtf8("\u21B6"), tr("Rotates a quarter-turn anticlockwise (Ctrl+[)."),
                "cropTurnLeft");
    QToolButton* turnRight =
        addTool(QString::fromUtf8("\u21B7"), tr("Rotates a quarter-turn clockwise (Ctrl+])."),
                "cropTurnRight");
    QToolButton* flipHorizontal =
        addTool(QString::fromUtf8("\u2194"), tr("Flips left and right."), "cropFlipHorizontal");
    QToolButton* flipVertical =
        addTool(QString::fromUtf8("\u2195"), tr("Flips top and bottom."), "cropFlipVertical");
    QToolButton* reset = addTool(tr("Reset"),
                                 tr("Puts the turns, flips, angle and crop back as the photograph "
                                    "came."),
                                 "cropReset");
    reset->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    cropResetButton_ = reset;
    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(2);
    for (QToolButton* button : {turnLeft, turnRight, flipHorizontal, flipVertical}) {
        toolRow->addWidget(button);
    }
    toolRow->addSpacing(8);
    toolRow->addWidget(reset);
    groupLayout->addLayout(toolRow);

    for (QToolButton* button :
         {lock, swapButton, turnLeft, turnRight, flipHorizontal, flipVertical, reset}) {
        button->setFocusPolicy(Qt::NoFocus);
    }

    // Each click is one complete change, made by whoever owns the photograph.
    connect(cropButton_, &QPushButton::clicked, this, &DevelopPanel::cropModeToggled);
    connect(levelButton_, &QPushButton::clicked, this, &DevelopPanel::straighteningToggled);
    connect(aspectCombo_, &QComboBox::activated, this, [this](int index) {
        const auto entry = static_cast<AspectEntry>(aspectCombo_->itemData(index).toInt());
        switch (entry) {
        case AspectEntry::Original:
            emit aspectChosen(OriginalCropAspect{}, false);
            return;
        case AspectEntry::Free:
            emit aspectChosen(FreeCropAspect{}, false);
            return;
        case AspectEntry::Custom:
            chooseCustomAspect();
            return;
        default:
            break;
        }
        for (const PresetRatio& preset : presetRatios) {
            if (preset.entry == entry) {
                emit aspectChosen(CropRatio{preset.widthOverHeight}, true);
            }
        }
    });
    connect(lock, &QToolButton::clicked, this, &DevelopPanel::lockToggled);
    connect(swapButton, &QToolButton::clicked, this, &DevelopPanel::orientationSwapped);
    connect(turnLeft, &QToolButton::clicked, this, [this] { emit turned(false); });
    connect(turnRight, &QToolButton::clicked, this, [this] { emit turned(true); });
    connect(flipHorizontal, &QToolButton::clicked, this, [this] { emit flipped(true); });
    connect(flipVertical, &QToolButton::clicked, this, [this] { emit flipped(false); });
    connect(reset, &QToolButton::clicked, this, &DevelopPanel::applyGeometryReset);
    return group;
}

void DevelopPanel::chooseCustomAspect() {
    QDialog dialog(this);
    dialog.setObjectName("customAspectDialog");
    dialog.setWindowTitle(tr("Custom Aspect"));
    auto* form = new QFormLayout(&dialog);
    const auto addSpin = [&](const QString& label, const char* name) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setObjectName(name);
        spin->setRange(0.1, 1000.0);
        spin->setDecimals(2);
        spin->setValue(3.0);
        form->addRow(label, spin);
        return spin;
    };
    auto* width = addSpin(tr("Width"), "customAspectWidth");
    auto* height = addSpin(tr("Height"), "customAspectHeight");
    height->setValue(2.0);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        emit aspectChosen(CropRatio{width->value() / height->value()}, false);
    } else {
        // The menu goes back to describing the settings.
        showGeometry(shown_.settings.geometry);
    }
}

void DevelopPanel::applyGeometryReset() {
    DevelopState next = shown_;
    next.settings.geometry = GeometrySettings{};
    if (next.settings.geometry == shown_.settings.geometry) {
        return;
    }
    finishPendingEdit();
    emit editStarted();
    emit stateEdited(next);
    finishResetEdit();
}

void DevelopPanel::finishResetEdit() {
    editOrigin_ = EditOrigin::Reset;
    emit editFinished();
    editOrigin_ = EditOrigin::Edit;
}

void DevelopPanel::showGeometry(const GeometrySettings& geometry) {
    const AspectEntry entry = entryFor(geometry.crop.aspect);
    const QSignalBlocker blocker(aspectCombo_);
    const int custom = aspectCombo_->findData(static_cast<int>(AspectEntry::Custom));
    // A ratio of the user's own is named in the menu's Custom entry.
    QString customText = tr("Custom\u2026");
    if (entry == AspectEntry::Custom) {
        const double ratio = std::get<CropRatio>(geometry.crop.aspect).widthOverHeight;
        const double landscape = std::max(ratio, 1.0 / ratio);
        customText = tr("Custom (%1:1)").arg(landscape, 0, 'g', 3);
    }
    aspectCombo_->setItemText(custom, customText);
    aspectCombo_->setCurrentIndex(aspectCombo_->findData(static_cast<int>(entry)));
    lockButton_->setChecked(entry != AspectEntry::Free);
    cropResetButton_->setEnabled(geometry != GeometrySettings{});
}

void DevelopPanel::setCropMode(bool cropping) {
    const QSignalBlocker blocker(cropButton_);
    cropButton_->setChecked(cropping);
    for (QWidget* group : nonGeometryGroups_) {
        group->setEnabled(!cropping);
    }
}

int DevelopPanel::minimumDockWidth() const {
    return minimumSizeHint().width() +
           style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
}

int DevelopPanel::defaultDockWidth() const {
    return minimumDockWidth() +
           static_cast<int>(std::lround(dockSlackInLines * fontMetrics().height()));
}

void DevelopPanel::setStraightening(bool straightening) {
    const QSignalBlocker blocker(levelButton_);
    levelButton_->setChecked(straightening);
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
    const auto addGroup = [&](QWidget* group) {
        layout->addWidget(group);
        nonGeometryGroups_.push_back(group);
    };
    addGroup(buildTreatmentRow());
    // Geometry is the first thing a photograph is given, as in Lightroom, below
    // the Treatment row, which describes the whole photograph rather than a step.
    layout->addWidget(buildCropGroup());
    addGroup(buildWhiteBalanceGroup());

    auto* tone = new QGroupBox(groupTitle(SettingGroup::Tone), this);
    new QVBoxLayout(tone);
    for (const std::string_view key : toneKeys()) {
        addRow(key, tone);
    }
    addGroup(tone);
    // Below Tone, as Lightroom's Basic panel has it, and shown in both treatments.
    addGroup(buildPresenceGroup());
    addGroup(buildToneCurveGroup());

    colorGroup_ = buildColorGroup();
    hslGroup_ = buildHslGroup();
    blackAndWhiteGroup_ = buildBlackAndWhiteGroup();
    addGroup(colorGroup_);
    addGroup(hslGroup_);
    addGroup(blackAndWhiteGroup_);
    addGroup(buildColorGradingGroup());
    addGroup(buildNoiseReductionGroup());
    addGroup(buildEffectsGroup());
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
    photo_ = context.photo;
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
    showGeometry(shown_.settings.geometry);

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
            // Shown as it appears on screen; an unset optional shows nothing.
            row->setValue(displayedValue(descriptor, shown_).value_or(0.0));
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
    if (row.key() == "straighten") {
        // The row shows the angle as on screen. Each tick starts from the state before the
        // edit, so the crop's shrinking is undone when the angle goes back.
        try {
            emit stateEdited(withDisplayedStraighten(photo_, editStart_, value));
        } catch (const std::invalid_argument&) {
            // A geometry that is not valid for the frame (a crop that disagrees with its
            // locked aspect, say) cannot be fitted; assign the angle alone and let the render
            // report the geometry, as it does without an edit.
            DevelopState next = editStart_;
            next.settings.geometry.straighten = storedStraighten(next.settings.geometry, value);
            emit stateEdited(next);
        }
        return;
    }
    // The rules of the key (white balance mode, a new grain seed) are core's. withValue throws
    // on a value out of range, which cannot reach a slot: SettingSlider clamps both the slider
    // and the spin box to the descriptor's range. The other keys do not read the frame.
    emit stateEdited(withValue(photo_, shown_, row.key(), value));
}

void DevelopPanel::applyClear(const SettingSlider& row) {
    if (row.key() == "temperature" || row.key() == "tint") {
        emit stateEdited(withValue(photo_, shown_, row.key(), std::nullopt));
        return;
    }
    emit stateEdited(shown_);
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
    if (choice != WhiteBalanceChoice::Custom) {
        // Only the three white balance values change; the rest of the colour settings stay.
        DevelopSettings source;
        if (const auto light = lightOf(choice)) {
            source.color = {WhiteBalanceMode::Custom, light->kelvin, light->tint};
        }
        constexpr std::array<std::string_view, 3> keys{"whiteBalance", "temperature", "tint"};
        next = withValues(photo_, next, keys, source);
    }
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
