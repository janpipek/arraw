#include "ExportDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSpinBox>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

ExportDialog::ExportDialog(int srcW, int srcH, QWidget* parent)
    : QDialog(parent), srcW(srcW), srcH(srcH)
{
    setWindowTitle("Export Image");
    setMinimumWidth(380);

    auto* root = new QVBoxLayout(this);

    // ── Format ──────────────────────────────────────────────────────────────
    auto* fmtForm = new QFormLayout;
    formatBox = new QComboBox;
    formatBox->addItems({"JPEG", "PNG", "TIFF"});
    fmtForm->addRow("Format:", formatBox);
    root->addLayout(fmtForm);

    root->addSpacing(6);

    // ── Output size ──────────────────────────────────────────────────────────
    auto* sizeGroup = new QGroupBox("Output Size");
    auto* sizeForm  = new QFormLayout(sizeGroup);

    widthSpin = new QSpinBox;
    widthSpin->setRange(1, 99999);
    widthSpin->setValue(srcW);
    widthSpin->setSuffix(" px");

    heightSpin = new QSpinBox;
    heightSpin->setRange(1, 99999);
    heightSpin->setValue(srcH);
    heightSpin->setSuffix(" px");

    constrainCheck = new QCheckBox("Constrain aspect ratio");
    constrainCheck->setChecked(true);

    sizeForm->addRow("Width:", widthSpin);
    sizeForm->addRow("Height:", heightSpin);
    sizeForm->addRow(constrainCheck);
    root->addWidget(sizeGroup);

    root->addSpacing(6);

    // ── JPEG quality ─────────────────────────────────────────────────────────
    qualityGroup = new QGroupBox("JPEG Quality");
    auto* qLayout = new QHBoxLayout(qualityGroup);

    qualitySlider = new QSlider(Qt::Horizontal);
    qualitySlider->setRange(1, 100);
    qualitySlider->setValue(90);

    qualitySpin = new QSpinBox;
    qualitySpin->setRange(1, 100);
    qualitySpin->setValue(90);
    qualitySpin->setFixedWidth(52);

    qLayout->addWidget(qualitySlider);
    qLayout->addWidget(qualitySpin);
    root->addWidget(qualityGroup);

    root->addSpacing(6);

    // ── Output sharpening ────────────────────────────────────────────────────
    auto* sharpGroup  = new QGroupBox("Output Sharpening");
    auto* sharpLayout = new QHBoxLayout(sharpGroup);

    sharpenSlider = new QSlider(Qt::Horizontal);
    sharpenSlider->setRange(0, 100);
    sharpenSlider->setValue(0);

    sharpenSpin = new QSpinBox;
    sharpenSpin->setRange(0, 100);
    sharpenSpin->setValue(0);
    sharpenSpin->setFixedWidth(52);

    sharpLayout->addWidget(sharpenSlider);
    sharpLayout->addWidget(sharpenSpin);
    root->addWidget(sharpGroup);

    root->addSpacing(8);

    // ── Buttons ──────────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Export");
    root->addWidget(buttons);

    // ── Connections ──────────────────────────────────────────────────────────
    connect(formatBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportDialog::onFormatChanged);

    connect(widthSpin,  QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ExportDialog::onWidthChanged);
    connect(heightSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ExportDialog::onHeightChanged);

    connect(qualitySlider, &QSlider::valueChanged, qualitySpin, &QSpinBox::setValue);
    connect(qualitySpin, QOverload<int>::of(&QSpinBox::valueChanged),
            qualitySlider, &QSlider::setValue);

    connect(sharpenSlider, &QSlider::valueChanged, sharpenSpin, &QSpinBox::setValue);
    connect(sharpenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            sharpenSlider, &QSlider::setValue);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    onFormatChanged(0);
}

ExportOptions ExportDialog::options() const {
    ExportOptions o;
    switch (formatBox->currentIndex()) {
    case 0: o.format = ExportOptions::Format::JPEG; break;
    case 1: o.format = ExportOptions::Format::PNG;  break;
    case 2: o.format = ExportOptions::Format::TIFF; break;
    }
    o.width     = widthSpin->value();
    o.height    = heightSpin->value();
    o.quality   = qualitySpin->value();
    o.sharpening = sharpenSpin->value();
    return o;
}

void ExportDialog::onFormatChanged(int idx) {
    qualityGroup->setVisible(idx == 0); // only for JPEG
    adjustSize();
}

void ExportDialog::onWidthChanged(int w) {
    if (syncing || !constrainCheck->isChecked() || srcW == 0) return;
    syncing = true;
    heightSpin->setValue(int(double(w) * srcH / srcW + 0.5));
    syncing = false;
}

void ExportDialog::onHeightChanged(int h) {
    if (syncing || !constrainCheck->isChecked() || srcH == 0) return;
    syncing = true;
    widthSpin->setValue(int(double(h) * srcW / srcH + 0.5));
    syncing = false;
}
