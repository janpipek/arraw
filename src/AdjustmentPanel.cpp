#include "AdjustmentPanel.h"
#include "Histogram.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>

AdjustmentPanel::AdjustmentPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(4);

    histogram = new Histogram(this);
    root->addWidget(histogram);

    auto makeGroup = [&](const QString& title) -> QVBoxLayout* {
        auto* box    = new QGroupBox(title, this);
        auto* layout = new QVBoxLayout(box);
        layout->setSpacing(2);
        root->addWidget(box);
        return layout;
    };

    auto* tone  = makeGroup("Tone");
    exposure    = addSlider(tone, "Exposure",    -500,  500,   0);
    contrast    = addSlider(tone, "Contrast",    -100,  100,   0);
    highlights  = addSlider(tone, "Highlights",  -100,  100,   0);
    shadows     = addSlider(tone, "Shadows",     -100,  100,   0);
    whites      = addSlider(tone, "Whites",      -100,  100,   0);
    blacks      = addSlider(tone, "Blacks",      -100,  100,   0);

    auto* color = makeGroup("Color");
    temperature = addSlider(color, "Temp",       2000, 12000, 5500);
    tint        = addSlider(color, "Tint",       -100,  100,   0);
    saturation  = addSlider(color, "Saturation", -100,  100,   0);
    vibrance    = addSlider(color, "Vibrance",   -100,  100,   0);

    auto* detail = makeGroup("Detail");
    sharpening  = addSlider(detail, "Sharpen",      0,  100,   0);

    auto* resetBtn = new QPushButton("Reset All", this);
    root->addWidget(resetBtn);
    root->addStretch();

    connect(resetBtn, &QPushButton::clicked, this, &AdjustmentPanel::resetAll);
    connectAll();
}

AdjustmentPanel::SliderRow AdjustmentPanel::addSlider(
    QVBoxLayout* layout, const QString& name, int min, int max, int def)
{
    auto* row    = new QWidget(this);
    auto* hbox   = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);

    auto* lbl    = new QLabel(name, row);
    lbl->setFixedWidth(72);
    auto* slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(min, max);
    slider->setValue(def);
    auto* val    = new QLabel(QString::number(def), row);
    val->setFixedWidth(36);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    hbox->addWidget(lbl);
    hbox->addWidget(slider, 1);
    hbox->addWidget(val);
    layout->addWidget(row);

    connect(slider, &QSlider::valueChanged, val,
            [val](int v) { val->setText(QString::number(v)); });

    return {slider, val};
}

void AdjustmentPanel::connectAll() {
    auto notify = [this](int) {
        adjustments.exposure    = exposure.slider->value()    / 100.0f;
        adjustments.contrast    = float(contrast.slider->value());
        adjustments.highlights  = float(highlights.slider->value());
        adjustments.shadows     = float(shadows.slider->value());
        adjustments.whites      = float(whites.slider->value());
        adjustments.blacks      = float(blacks.slider->value());
        adjustments.temperature = float(temperature.slider->value());
        adjustments.tint        = float(tint.slider->value());
        adjustments.saturation  = float(saturation.slider->value());
        adjustments.vibrance    = float(vibrance.slider->value());
        adjustments.sharpening  = float(sharpening.slider->value());
        emit paramsChanged(adjustments);
    };

    for (auto* s : {exposure.slider, contrast.slider, highlights.slider,
                    shadows.slider,  whites.slider,   blacks.slider,
                    temperature.slider, tint.slider,
                    saturation.slider, vibrance.slider, sharpening.slider})
        connect(s, &QSlider::valueChanged, this, notify);
}

void AdjustmentPanel::resetAll() {
    setParams(AdjustmentParams{});
}

void AdjustmentPanel::setParams(const AdjustmentParams& p) {
    for (auto* s : {exposure.slider, contrast.slider, highlights.slider,
                    shadows.slider,  whites.slider,   blacks.slider,
                    temperature.slider, tint.slider,
                    saturation.slider, vibrance.slider, sharpening.slider})
        s->blockSignals(true);

    exposure.slider->setValue(int(p.exposure * 100.0f));
    contrast.slider->setValue(int(p.contrast));
    highlights.slider->setValue(int(p.highlights));
    shadows.slider->setValue(int(p.shadows));
    whites.slider->setValue(int(p.whites));
    blacks.slider->setValue(int(p.blacks));
    temperature.slider->setValue(int(p.temperature));
    tint.slider->setValue(int(p.tint));
    saturation.slider->setValue(int(p.saturation));
    vibrance.slider->setValue(int(p.vibrance));
    sharpening.slider->setValue(int(p.sharpening));

    for (auto* s : {exposure.slider, contrast.slider, highlights.slider,
                    shadows.slider,  whites.slider,   blacks.slider,
                    temperature.slider, tint.slider,
                    saturation.slider, vibrance.slider, sharpening.slider})
        s->blockSignals(false);

    adjustments = p;
    emit paramsChanged(adjustments);
}

void AdjustmentPanel::setHistogramImage(const ImageBuffer& buf) {
    histogram->setImage(buf);
}
