#include "ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
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

} // namespace

ExportDialog::ExportDialog(const ExportSettings& initial, ImageSize frame, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Export Image"));
    setMinimumWidth(380);

    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout;
    formatBox_ = new QComboBox;
    formatBox_->addItems({tr("JPEG"), tr("PNG"), tr("TIFF")});
    formatBox_->setCurrentIndex(indexOf(formats, initial.format));
    form->addRow(tr("Format:"), formatBox_);

    profileBox_ = new QComboBox;
    profileBox_->addItems({tr("sRGB"), tr("Display P3"), tr("Adobe RGB")});
    profileBox_->setCurrentIndex(indexOf(encodings, initial.encoding));
    form->addRow(tr("Color profile:"), profileBox_);

    sixteenBitCheck_ = new QCheckBox(tr("16-bit per channel"));
    sixteenBitCheck_->setChecked(initial.sixteenBit);
    form->addRow(QString(), sixteenBitCheck_);
    root->addLayout(form);

    auto* sizeGroup = new QGroupBox(tr("Output Size"));
    auto* sizeForm = new QFormLayout(sizeGroup);
    resizeCheck_ = new QCheckBox(tr("Resize to fit"));
    resizeCheck_->setChecked(initial.resize);
    sizeForm->addRow(resizeCheck_);
    widthSpin_ = new QSpinBox;
    widthSpin_->setRange(1, 99999);
    widthSpin_->setValue(spinValue(frame.width));
    widthSpin_->setSuffix(tr(" px"));
    sizeForm->addRow(tr("Width:"), widthSpin_);
    heightSpin_ = new QSpinBox;
    heightSpin_->setRange(1, 99999);
    heightSpin_->setValue(spinValue(frame.height));
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
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(formatBox_, &QComboBox::currentIndexChanged, this, [this] { updateEnabled(); });
    connect(resizeCheck_, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    updateEnabled();
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
    const bool resize = resizeCheck_->isChecked();
    widthSpin_->setEnabled(resize);
    heightSpin_->setEnabled(resize);
    enlargeCheck_->setEnabled(resize);
}

ExportSettings ExportDialog::settings() const {
    return {.format = formats[formatBox_->currentIndex()],
            .encoding = encodings[profileBox_->currentIndex()],
            .sixteenBit = sixteenBitCheck_->isChecked(),
            .resize = resizeCheck_->isChecked(),
            .width = static_cast<std::uint32_t>(widthSpin_->value()),
            .height = static_cast<std::uint32_t>(heightSpin_->value()),
            .allowEnlarging = enlargeCheck_->isChecked(),
            .quality = qualitySpin_->value(),
            .sharpening = sharpenSpin_->value(),
            .captureInfo = captureCheck_->isChecked(),
            .location = locationCheck_->isChecked(),
            .descriptive = descriptiveCheck_->isChecked()};
}

} // namespace arraw::app
