#include "ExportDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <limits>

namespace arraw::app {

namespace {

/// Formats in the order the format box lists them.
constexpr ImageFileFormat formats[]{ImageFileFormat::Jpeg, ImageFileFormat::Png,
                                    ImageFileFormat::Tiff};

/// Encodings in the order the profile box lists them.
constexpr NamedEncoding encodings[]{NamedEncoding::Srgb, NamedEncoding::DisplayP3,
                                    NamedEncoding::AdobeRgb};

/// @brief Gives the position of a value in a list, or 0.
template <typename T, std::size_t N> int indexOf(const T (&list)[N], T value) {
    const auto* found = std::ranges::find(list, value);
    return found == std::end(list) ? 0 : static_cast<int>(found - std::begin(list));
}

/// @brief Clamps a pixel count to what a spin box holds.
int spinValue(std::uint32_t pixels) {
    return static_cast<int>(std::clamp<std::uint32_t>(pixels, 1, 99999));
}

/// @brief Makes a slider and a spin box that follow each other.
/// @return The row's layout, holding both.
QHBoxLayout* linkedRow(QSlider* slider, QSpinBox* spin, int low, int high, int value) {
    slider->setOrientation(Qt::Horizontal);
    slider->setRange(low, high);
    spin->setRange(low, high);
    spin->setFixedWidth(60);
    slider->setValue(value);
    spin->setValue(value);
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
    auto* row = new QHBoxLayout;
    row->addWidget(slider);
    row->addWidget(spin);
    return row;
}

/// @brief Gives the size box's item that stands for the size in settings.
/// @return 0 for the original size, 1.. for a preset long edge, the last item for custom.
int sizeChoiceOf(const ExportSettings& settings) {
    const auto edges = presetLongEdges();
    if (!settings.resize) {
        return 0;
    }
    if (settings.width == settings.height) {
        const auto found = std::ranges::find(edges, settings.width);
        if (found != edges.end()) {
            return 1 + static_cast<int>(found - edges.begin());
        }
    }
    return static_cast<int>(edges.size()) + 1;
}

/// Item data of the picker: the preset's name.
constexpr int nameRole = Qt::UserRole;

/// Item data of the picker: whether the item is the "No preset" one.
constexpr int noneRole = Qt::UserRole + 1;

/// Item data of the picker: whether the item is a preset hidden by a built-in of its name.
constexpr int shadowedRole = Qt::UserRole + 2;

/// Item data of the picker: the item's text without any "(modified)" mark.
constexpr int labelRole = Qt::UserRole + 3;

/// @brief Makes a path a string for display.
QString shown(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// @brief Makes a preset name a string.
QString shown(const std::string& name) {
    return QString::fromStdString(name);
}

} // namespace

ExportDialog::ExportDialog(const ExportSettings& initial, ImageSize frame, ExportPresetStore* store,
                           std::optional<std::string> rememberedPreset, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Export Image"));
    setMinimumWidth(380);

    auto* root = new QVBoxLayout(this);
    buildPresetRow(root);

    auto* form = new QFormLayout;
    formatBox_ = new QComboBox;
    formatBox_->addItems({tr("JPEG"), tr("PNG"), tr("TIFF")});
    formatBox_->setCurrentIndex(indexOf(formats, initial.format));
    form->addRow(tr("Format:"), formatBox_);

    profileBox_ = new QComboBox;
    profileBox_->addItems({tr("sRGB"), tr("Display P3"), tr("Adobe RGB")});
    profileBox_->setCurrentIndex(indexOf(encodings, initial.encoding));
    form->addRow(tr("Colour profile:"), profileBox_);

    sixteenBitCheck_ = new QCheckBox(tr("16-bit per channel"));
    sixteenBitCheck_->setChecked(initial.sixteenBit);
    form->addRow(QString(), sixteenBitCheck_);
    root->addLayout(form);

    auto* sizeGroup = new QGroupBox(tr("Output Size"));
    auto* sizeForm = new QFormLayout(sizeGroup);
    sizeForm_ = sizeForm;
    // Items: original, one per preset long edge, custom.
    sizeBox_ = new QComboBox;
    sizeBox_->addItem(tr("Original size"));
    for (const std::uint32_t edge : presetLongEdges()) {
        sizeBox_->addItem(tr("Long edge %1 px").arg(edge));
    }
    sizeBox_->addItem(tr("Custom\u2026"));
    sizeBox_->setCurrentIndex(sizeChoiceOf(initial));
    sizeForm->addRow(tr("Size:"), sizeBox_);
    widthSpin_ = new QSpinBox;
    widthSpin_->setRange(1, 99999);
    widthSpin_->setValue(spinValue(initial.resize ? initial.width : frame.width));
    widthSpin_->setSuffix(tr(" px"));
    sizeForm->addRow(tr("Width:"), widthSpin_);
    heightSpin_ = new QSpinBox;
    heightSpin_->setRange(1, 99999);
    heightSpin_->setValue(spinValue(initial.resize ? initial.height : frame.height));
    heightSpin_->setSuffix(tr(" px"));
    sizeForm->addRow(tr("Height:"), heightSpin_);
    enlargeCheck_ = new QCheckBox(tr("Allow enlarging"));
    enlargeCheck_->setChecked(initial.allowEnlarging);
    sizeForm->addRow(enlargeCheck_);
    root->addWidget(sizeGroup);

    qualityGroup_ = new QGroupBox(tr("JPEG Quality"));
    qualitySlider_ = new QSlider;
    qualitySpin_ = new QSpinBox;
    qualityGroup_->setLayout(linkedRow(qualitySlider_, qualitySpin_, 0, 100, initial.quality));
    root->addWidget(qualityGroup_);

    auto* sharpenGroup = new QGroupBox(tr("Output Sharpening"));
    sharpenSlider_ = new QSlider;
    sharpenSpin_ = new QSpinBox;
    sharpenGroup->setLayout(linkedRow(sharpenSlider_, sharpenSpin_, 0, 100, initial.sharpening));
    root->addWidget(sharpenGroup);

    auto* metadataGroup = new QGroupBox(tr("Metadata"));
    auto* metadataLayout = new QVBoxLayout(metadataGroup);
    captureCheck_ = new QCheckBox(tr("Camera && capture info"));
    captureCheck_->setChecked(initial.captureInfo);
    captureCheck_->setToolTip(tr("Camera, lens, exposure and capture time. Never serial numbers."));
    locationCheck_ = new QCheckBox(tr("Location"));
    locationCheck_->setChecked(initial.location);
    locationCheck_->setToolTip(tr("Where the photograph was taken (GPS). It can reveal a home."));
    descriptiveCheck_ = new QCheckBox(tr("Descriptive metadata"));
    descriptiveCheck_->setChecked(initial.descriptive);
    descriptiveCheck_->setToolTip(
        tr("Rating, label, title, caption, keywords, creator and rights."));
    metadataLayout->addWidget(captureCheck_);
    metadataLayout->addWidget(locationCheck_);
    metadataLayout->addWidget(descriptiveCheck_);
    root->addWidget(metadataGroup);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    root->addWidget(buttons);
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(formatBox_, &QComboBox::currentIndexChanged, this, [this] { updateEnabled(); });
    connect(sizeBox_, &QComboBox::currentIndexChanged, this, [this] { updateEnabled(); });
    updateEnabled();

    // Any change of a control may make the chosen preset modified, or not.
    const auto changed = [this] { refreshPresetState(); };
    connect(formatBox_, &QComboBox::currentIndexChanged, this, changed);
    connect(profileBox_, &QComboBox::currentIndexChanged, this, changed);
    connect(sizeBox_, &QComboBox::currentIndexChanged, this, changed);
    connect(sixteenBitCheck_, &QCheckBox::toggled, this, changed);
    connect(enlargeCheck_, &QCheckBox::toggled, this, changed);
    connect(captureCheck_, &QCheckBox::toggled, this, changed);
    connect(locationCheck_, &QCheckBox::toggled, this, changed);
    connect(descriptiveCheck_, &QCheckBox::toggled, this, changed);
    connect(widthSpin_, &QSpinBox::valueChanged, this, changed);
    connect(heightSpin_, &QSpinBox::valueChanged, this, changed);
    connect(qualitySpin_, &QSpinBox::valueChanged, this, changed);
    connect(sharpenSpin_, &QSpinBox::valueChanged, this, changed);

    if (store_ != nullptr) {
        reloadPresets(rememberedPreset);
    }
}

void ExportDialog::buildPresetRow(QVBoxLayout* root) {
    if (store_ == nullptr) {
        return;
    }
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Preset:")));
    presetBox_ = new QComboBox;
    presetBox_->setObjectName(QStringLiteral("presetBox"));
    presetBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    presetBox_->setMinimumContentsLength(18);
    row->addWidget(presetBox_, 1);

    saveAsButton_ = new QPushButton(tr("Save As\u2026"));
    saveAsButton_->setObjectName(QStringLiteral("presetSaveAs"));
    saveAsButton_->setAutoDefault(false);
    row->addWidget(saveAsButton_);

    presetMenuButton_ = new QToolButton;
    presetMenuButton_->setObjectName(QStringLiteral("presetMenuButton"));
    presetMenuButton_->setText(tr("More"));
    presetMenuButton_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(presetMenuButton_);
    menu->setToolTipsVisible(true);
    const auto addAction = [&](const char* name, const QString& text) {
        auto* action = menu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        return action;
    };
    updateAction_ = addAction("presetUpdate", tr("Update"));
    renameAction_ = addAction("presetRename", tr("Rename\u2026"));
    deleteAction_ = addAction("presetDelete", tr("Delete\u2026"));
    menu->addSeparator();
    showFileAction_ = addAction("presetShowFile", tr("Show File"));
    setAsideAction_ = addAction("presetSetAside", tr("Set Aside\u2026"));
    presetMenuButton_->setMenu(menu);
    row->addWidget(presetMenuButton_);
    root->addLayout(row);

    // Programmatic changes of the picker block this signal; only the user's reach it.
    connect(presetBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (presetBox_->itemData(index, noneRole).toBool()) {
            chosen_.reset();
        } else if (const ExportPreset* preset =
                       findPreset(presetBox_->itemData(index, nameRole).toString().toStdString(),
                                  presetBox_->itemData(index, shadowedRole).toBool())) {
            chosen_ = *preset;
            apply(preset->settings);
        }
        refreshPresetState();
    });
    connect(saveAsButton_, &QPushButton::clicked, this, &ExportDialog::saveAsPreset);
    connect(updateAction_, &QAction::triggered, this, &ExportDialog::updatePreset);
    connect(renameAction_, &QAction::triggered, this, &ExportDialog::renamePreset);
    connect(deleteAction_, &QAction::triggered, this, &ExportDialog::deletePreset);
    connect(showFileAction_, &QAction::triggered, this, &ExportDialog::showPresetsFile);
    connect(setAsideAction_, &QAction::triggered, this, &ExportDialog::setAsidePresets);
}

void ExportDialog::setDiagnostics(DiagnosticLog* log) {
    diagnostics_ = log;
    if (store_ != nullptr) {
        reloadPresets(chosenPreset());
    }
}

const ExportPreset* ExportDialog::findPreset(const std::string& name, bool shadowed) const {
    for (const ExportPreset& preset : catalog_.presets) {
        if (preset.shadowed == shadowed && presetNamesEqual(preset.name, name)) {
            return &preset;
        }
    }
    return nullptr;
}

void ExportDialog::reloadPresets(const std::optional<std::string>& name) {
    try {
        catalog_ = store_->read(diagnostics_ != nullptr ? *diagnostics_ : discardedDiagnostics());
    } catch (const std::exception& error) {
        catalog_ = {};
        catalog_.state = ExportPresetFileState::Unreadable;
        catalog_.problem = error.what();
    }
    const QSignalBlocker blocker(presetBox_);
    presetBox_->clear();
    auto* model = qobject_cast<QStandardItemModel*>(presetBox_->model());
    const auto addItem = [&](const QString& text, const std::string& preset, bool usable,
                             const QString& reason) {
        presetBox_->addItem(text);
        const int index = presetBox_->count() - 1;
        presetBox_->setItemData(index, shown(preset), nameRole);
        presetBox_->setItemData(index, text, labelRole);
        if (!usable) {
            model->item(index)->setEnabled(false);
            presetBox_->setItemData(index, reason, Qt::ToolTipRole);
        }
    };
    addItem(tr("No preset"), {}, true, {});
    presetBox_->setItemData(0, true, noneRole);

    // Built-ins first, in a section of their own; then the file's, then the unusable ones.
    bool separated = false;
    const auto separate = [&] {
        if (!separated) {
            presetBox_->insertSeparator(presetBox_->count());
            separated = true;
        }
    };
    for (const ExportPreset& preset : catalog_.presets) {
        if (preset.builtIn) {
            separate();
            addItem(shown(preset.name), preset.name, true, {});
        }
    }
    separated = false;
    for (const ExportPreset& preset : catalog_.presets) {
        if (!preset.builtIn) {
            separate();
            // A hidden preset stays selectable, to be renamed or deleted (ADR 054 section 8).
            addItem(preset.shadowed ? tr("%1 (hidden by a built-in)").arg(shown(preset.name))
                                    : shown(preset.name),
                    preset.name, true, hiddenReason());
            if (preset.shadowed) {
                presetBox_->setItemData(presetBox_->count() - 1, true, shadowedRole);
                presetBox_->setItemData(presetBox_->count() - 1, hiddenReason(), Qt::ToolTipRole);
            }
        }
    }
    for (const UnusableExportPreset& preset : catalog_.unusable) {
        separate();
        addItem(preset.name.empty() ? tr("(unnamed)") : shown(preset.name), {}, false,
                shown(preset.reason));
    }

    chosen_.reset();
    int index = 0;
    if (name) {
        if (const ExportPreset* preset = findPreset(*name, false)) {
            chosen_ = *preset;
            for (int i = 1; i < presetBox_->count(); ++i) {
                if (model->item(i)->isEnabled() &&
                    !presetBox_->itemData(i, shadowedRole).toBool() &&
                    presetBox_->itemData(i, nameRole).toString() == shown(preset->name)) {
                    index = i;
                    break;
                }
            }
        }
    }
    presetBox_->setCurrentIndex(index);
    refreshPresetState();
}

bool ExportDialog::modified() const {
    return chosen_ && canonical(chosen_->settings) != canonical(settings());
}

QString ExportDialog::writeBlock() const {
    switch (catalog_.state) {
    case ExportPresetFileState::Newer:
        return tr("A newer arraw wrote the presets file, so this one does not change it.");
    case ExportPresetFileState::Unreadable:
        return tr("The presets file cannot be read (%1). Set it aside to start a new one.")
            .arg(shown(catalog_.problem));
    case ExportPresetFileState::Missing:
    case ExportPresetFileState::Read:
        break;
    }
    return {};
}

void ExportDialog::refreshPresetState() {
    if (store_ == nullptr || presetBox_ == nullptr) {
        return;
    }
    const bool isModified = modified();
    // Only the chosen item can say it is modified.
    const int index = presetBox_->currentIndex();
    for (int i = 1; i < presetBox_->count(); ++i) {
        const QVariant label = presetBox_->itemData(i, labelRole);
        if (label.isValid()) {
            presetBox_->setItemText(i, i == index && isModified
                                           ? label.toString() + tr(" (modified)")
                                           : label.toString());
        }
    }

    const QString block = writeBlock();
    const bool writable = block.isEmpty();
    const bool custom = chosen_ && !chosen_->builtIn;
    const bool hidden = chosen_ && chosen_->shadowed;
    saveAsButton_->setEnabled(writable);
    saveAsButton_->setToolTip(block);
    updateAction_->setText(chosen_ ? tr("Update \"%1\"").arg(shown(chosen_->name)) : tr("Update"));
    updateAction_->setEnabled(writable && custom && !hidden && isModified);
    renameAction_->setEnabled(writable && custom);
    deleteAction_->setEnabled(writable && custom);
    for (QAction* action : {updateAction_, renameAction_, deleteAction_}) {
        action->setToolTip(block);
    }
    if (hidden && writable) {
        updateAction_->setToolTip(hiddenReason());
    }
    // A hidden preset is there to be renamed or deleted, not to be exported with.
    if (okButton_ != nullptr) {
        okButton_->setEnabled(!hidden);
        okButton_->setToolTip(hidden ? hiddenReason() : QString());
    }
    std::error_code ignored;
    showFileAction_->setEnabled(std::filesystem::exists(store_->file(), ignored));
    setAsideAction_->setVisible(catalog_.state == ExportPresetFileState::Unreadable);
}

QString ExportDialog::hiddenReason() const {
    return tr("Hidden by the built-in preset of the same name: rename or delete it, then it can "
              "be used again.");
}

std::optional<std::string> ExportDialog::chosenPreset() const {
    if (!chosen_ || chosen_->shadowed) {
        return std::nullopt;
    }
    return chosen_->name;
}

void ExportDialog::apply(const ExportSettings& settings) {
    formatBox_->setCurrentIndex(indexOf(formats, settings.format));
    profileBox_->setCurrentIndex(indexOf(encodings, settings.encoding));
    updateEnabled();
    sixteenBitCheck_->setChecked(settings.sixteenBit && settings.format != ImageFileFormat::Jpeg);
    if (settings.resize) {
        widthSpin_->setValue(spinValue(settings.width));
        heightSpin_->setValue(spinValue(settings.height));
    }
    sizeBox_->setCurrentIndex(sizeChoiceOf(settings));
    enlargeCheck_->setChecked(settings.resize && settings.allowEnlarging);
    qualitySpin_->setValue(settings.quality);
    sharpenSpin_->setValue(settings.sharpening);
    captureCheck_->setChecked(settings.captureInfo);
    locationCheck_->setChecked(settings.location);
    descriptiveCheck_->setChecked(settings.descriptive);
    updateEnabled();
}

std::optional<QString> ExportDialog::askName(const QString& title, const QString& label,
                                             const QString& initial) {
    if (prompts_.name) {
        return prompts_.name(title, label, initial);
    }
    bool accepted = false;
    const QString text =
        QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &accepted);
    if (!accepted) {
        return std::nullopt;
    }
    return text;
}

bool ExportDialog::confirm(const QString& title, const QString& text) {
    if (prompts_.confirm) {
        return prompts_.confirm(title, text);
    }
    return QMessageBox::question(this, title, text) == QMessageBox::Yes;
}

void ExportDialog::warn(const QString& title, const QString& text) {
    if (prompts_.warn) {
        prompts_.warn(title, text);
        return;
    }
    QMessageBox::warning(this, title, text);
}

bool ExportDialog::change(const QString& title, const std::function<void()>& action) {
    try {
        action();
        return true;
    } catch (const std::exception& error) {
        warn(title, QString::fromUtf8(error.what()));
    }
    return false;
}

void ExportDialog::saveAsPreset() {
    const QString title = tr("Save Export Preset");
    QString initial = chosen_ ? shown(chosen_->name) : QString();
    if (chosen_ && (chosen_->builtIn || chosen_->shadowed)) {
        initial = tr("%1 copy").arg(initial);
    }
    const auto typed = askName(title, tr("Name of the preset:"), initial);
    if (!typed) {
        return;
    }
    const auto name = normalizedPresetName(typed->toStdString());
    if (!name) {
        warn(title, tr("A preset name has 1 to 64 characters and no control characters."));
        return;
    }
    const auto same = std::ranges::find_if(catalog_.presets, [&](const ExportPreset& preset) {
        return !preset.builtIn && presetNamesEqual(preset.name, *name);
    });
    if (same != catalog_.presets.end() &&
        !confirm(title, tr("A preset named \"%1\" exists. Replace it?").arg(shown(same->name)))) {
        return;
    }
    if (change(title, [&] { store_->save({.name = *name, .settings = settings()}); })) {
        reloadPresets(*name);
    }
}

void ExportDialog::updatePreset() {
    if (!chosen_ || chosen_->builtIn) {
        return;
    }
    const std::string name = chosen_->name;
    if (change(tr("Update Export Preset"),
               [&] { store_->save({.name = name, .settings = settings()}); })) {
        reloadPresets(name);
    }
}

void ExportDialog::renamePreset() {
    if (!chosen_ || chosen_->builtIn) {
        return;
    }
    const QString title = tr("Rename Export Preset");
    const auto typed = askName(title, tr("New name:"), shown(chosen_->name));
    if (!typed) {
        return;
    }
    const auto name = normalizedPresetName(typed->toStdString());
    if (!name) {
        warn(title, tr("A preset name has 1 to 64 characters and no control characters."));
        return;
    }
    const std::string from = chosen_->name;
    if (change(title, [&] { store_->rename(from, *name); })) {
        reloadPresets(*name);
    }
}

void ExportDialog::deletePreset() {
    if (!chosen_ || chosen_->builtIn) {
        return;
    }
    const QString title = tr("Delete Export Preset");
    const std::string name = chosen_->name;
    if (!confirm(title, tr("Delete the preset \"%1\"?").arg(shown(name)))) {
        return;
    }
    if (change(title, [&] { store_->remove(name); })) {
        reloadPresets(std::nullopt);
    }
}

void ExportDialog::showPresetsFile() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(shown(store_->file().parent_path())));
}

void ExportDialog::setAsidePresets() {
    const QString title = tr("Set Aside Export Presets");
    if (!confirm(title,
                 tr("The presets file cannot be read:\n%1\n\nMove it to\n%2\n\nand "
                    "start a new one?")
                     .arg(shown(store_->file()), shown(store_->file().parent_path() /
                                                       "export-presets.unreadable-<date>.json")))) {
        return;
    }
    const std::optional<std::string> keep = chosenPreset();
    if (change(title, [&] { (void)store_->setAside(); })) {
        reloadPresets(keep);
    }
}

void ExportDialog::updateEnabled() {
    const ImageFileFormat format = formats[formatBox_->currentIndex()];
    const bool jpeg = format == ImageFileFormat::Jpeg;
    // JPEG has 8 bits only: shown unchecked, and disabled.
    if (jpeg) {
        sixteenBitCheck_->setChecked(false);
    }
    sixteenBitCheck_->setEnabled(!jpeg);
    qualityGroup_->setEnabled(jpeg);
    const int choice = sizeBox_->currentIndex();
    const bool custom = choice == sizeBox_->count() - 1;
    sizeForm_->setRowVisible(widthSpin_, custom);
    sizeForm_->setRowVisible(heightSpin_, custom);
    widthSpin_->setEnabled(custom);
    heightSpin_->setEnabled(custom);
    enlargeCheck_->setEnabled(choice != 0);
}

ExportSettings ExportDialog::settings() const {
    const auto edges = presetLongEdges();
    const int choice = sizeBox_->currentIndex();
    const bool preset = choice >= 1 && choice <= static_cast<int>(edges.size());
    const bool custom = choice == static_cast<int>(edges.size()) + 1;
    const std::uint32_t edge = preset ? edges[static_cast<std::size_t>(choice - 1)] : 0;
    const std::uint32_t width = custom ? static_cast<std::uint32_t>(widthSpin_->value()) : edge;
    const std::uint32_t height = custom ? static_cast<std::uint32_t>(heightSpin_->value()) : edge;
    return {.format = formats[formatBox_->currentIndex()],
            .encoding = encodings[profileBox_->currentIndex()],
            .sixteenBit = sixteenBitCheck_->isChecked(),
            .resize = choice != 0,
            .width = width,
            .height = height,
            .allowEnlarging = enlargeCheck_->isChecked(),
            .quality = qualitySpin_->value(),
            .sharpening = sharpenSpin_->value(),
            .captureInfo = captureCheck_->isChecked(),
            .location = locationCheck_->isChecked(),
            .descriptive = descriptiveCheck_->isChecked()};
}

} // namespace arraw::app
