#include "AdjustmentPanel.h"
#include "Histogram.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QGroupBox>

struct WBPreset { const char* name; int kelvin; int tint; };
static const WBPreset kWBPresets[] = {
    { "As Shot",     5500,  0 },
    { "Daylight",    5500,  0 },
    { "Cloudy",      6500,  0 },
    { "Shade",       7500,  0 },
    { "Tungsten",    3200,  0 },
    { "Fluorescent", 4000, 15 },
    { "Flash",       5500,  0 },
};

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

    // White Balance
    auto* wbLayout = makeGroup("White Balance");
    wbPresets = new QComboBox(this);
    for (auto& p : kWBPresets)
        wbPresets->addItem(p.name);
    wbLayout->addWidget(wbPresets);
    temperature = addSlider(wbLayout, "Temp",   2000, 12000, 5500, "K");
    tint        = addSlider(wbLayout, "Tint",   -100,   100,    0);

    // Tone
    auto* tone = makeGroup("Tone");
    exposure   = addSlider(tone, "Exposure",   -500,  500,   0);
    contrast   = addSlider(tone, "Contrast",   -100,  100,   0);
    highlights = addSlider(tone, "Highlights", -100,  100,   0);
    shadows    = addSlider(tone, "Shadows",    -100,  100,   0);
    whites     = addSlider(tone, "Whites",     -100,  100,   0);
    blacks     = addSlider(tone, "Blacks",     -100,  100,   0);

    // Color
    auto* color = makeGroup("Color");
    saturation  = addSlider(color, "Saturation", -100, 100, 0);
    vibrance    = addSlider(color, "Vibrance",   -100, 100, 0);

    // Detail
    auto* detail = makeGroup("Detail");
    sharpening   = addSlider(detail, "Sharpen", 0, 100, 0);

    // Geometry
    auto* geo = makeGroup("Geometry");
    rotation  = addSlider(geo, "Rotation", -4500, 4500, 0, "°");

    auto* resetBtn = new QPushButton("Reset All", this);
    root->addWidget(resetBtn);
    root->addStretch();

    connect(resetBtn, &QPushButton::clicked, this, &AdjustmentPanel::resetAll);

    connect(wbPresets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0) return;
        temperature.slider->setValue(kWBPresets[i].kelvin);
        tint.slider->setValue(kWBPresets[i].tint);
    });

    connectAll();
}

AdjustmentPanel::SliderRow AdjustmentPanel::addSlider(
    QVBoxLayout* layout, const QString& name,
    int min, int max, int def, const QString& suffix)
{
    auto* row    = new QWidget(this);
    auto* hbox   = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);

    auto* lbl    = new QLabel(name, row);
    lbl->setFixedWidth(72);
    auto* slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(min, max);
    slider->setValue(def);
    auto* val    = new QLabel(QString::number(def) + suffix, row);
    val->setFixedWidth(44);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    hbox->addWidget(lbl);
    hbox->addWidget(slider, 1);
    hbox->addWidget(val);
    layout->addWidget(row);

    connect(slider, &QSlider::valueChanged, val,
            [val, suffix](int v) { val->setText(QString::number(v) + suffix); });

    return {slider, val};
}

void AdjustmentPanel::connectAll() {
    auto syncParams = [this]() {
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
        adjustments.rotation    = rotation.slider->value() / 100.0f;
    };

    auto allSliders = {
        exposure.slider, contrast.slider, highlights.slider,
        shadows.slider,  whites.slider,   blacks.slider,
        temperature.slider, tint.slider,
        saturation.slider, vibrance.slider, sharpening.slider,
        rotation.slider
    };

    for (auto* s : allSliders) {
        connect(s, &QSlider::valueChanged, this, [this, syncParams](int) {
            syncParams();
            emit paramsChanged(adjustments);
        });
        connect(s, &QSlider::sliderPressed, this, [this]() {
            beforeDrag = adjustments;
        });
        connect(s, &QSlider::sliderReleased, this, [this]() {
            if (!(adjustments == beforeDrag))
                emit adjustmentCommitted(beforeDrag, adjustments);
        });
    }
}

void AdjustmentPanel::resetAll() {
    setParams(AdjustmentParams{});
}

void AdjustmentPanel::setParams(const AdjustmentParams& p) {
    auto allSliders = {
        exposure.slider, contrast.slider, highlights.slider,
        shadows.slider,  whites.slider,   blacks.slider,
        temperature.slider, tint.slider,
        saturation.slider, vibrance.slider, sharpening.slider,
        rotation.slider
    };
    for (auto* s : allSliders) s->blockSignals(true);

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
    rotation.slider->setValue(int(p.rotation * 100.0f));

    for (auto* s : allSliders) s->blockSignals(false);

    adjustments = p;
    emit paramsChanged(adjustments);
}

void AdjustmentPanel::setHistogramImage(const ImageBuffer& buf) {
    histogram->setImage(buf);
}
