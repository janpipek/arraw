#include "AdjustmentPanel.h"
#include "Histogram.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QGroupBox>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QScrollArea>

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

static const char* kHslRangeNames[] = {
    "Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues", "Purples", "Magentas"
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

    // ── White Balance ─────────────────────────────────────────────────────────
    auto* wbLayout = makeGroup("White Balance");
    wbPresets = new QComboBox(this);
    for (auto& p : kWBPresets)
        wbPresets->addItem(p.name);
    wbLayout->addWidget(wbPresets);
    temperature = addSlider(wbLayout, "Temp",   2000, 12000, 5500, "K");
    tint        = addSlider(wbLayout, "Tint",   -100,   100,    0);

    // ── Tone ──────────────────────────────────────────────────────────────────
    auto* tone = makeGroup("Tone");
    exposure   = addSlider(tone, "Exposure",   -500,  500,   0);
    contrast   = addSlider(tone, "Contrast",   -100,  100,   0);
    highlights = addSlider(tone, "Highlights", -100,  100,   0);
    shadows    = addSlider(tone, "Shadows",    -100,  100,   0);
    whites     = addSlider(tone, "Whites",     -100,  100,   0);
    blacks     = addSlider(tone, "Blacks",     -100,  100,   0);

    // ── Tone Curve ────────────────────────────────────────────────────────────
    {
        auto* box    = new QGroupBox("Tone Curve", this);
        auto* layout = new QVBoxLayout(box);
        layout->setSpacing(4);
        root->addWidget(box);

        // Channel selector buttons
        auto* chRow    = new QWidget(box);
        auto* chLayout = new QHBoxLayout(chRow);
        chLayout->setContentsMargins(0, 0, 0, 0);
        chLayout->setSpacing(2);

        auto* chGroup = new QButtonGroup(box);
        auto makeChBtn = [&](const QString& label, ToneCurveWidget::Channel ch,
                             const QString& color) {
            auto* btn = new QPushButton(label, chRow);
            btn->setCheckable(true);
            btn->setFixedHeight(20);
            btn->setStyleSheet(QString("QPushButton:checked { background: %1; color: white; }").arg(color));
            chGroup->addButton(btn, int(ch));
            chLayout->addWidget(btn);
            return btn;
        };
        auto* btnL = makeChBtn("L", ToneCurveWidget::Channel::Luma,  "#555");
        makeChBtn("R", ToneCurveWidget::Channel::Red,   "#c03");
        makeChBtn("G", ToneCurveWidget::Channel::Green, "#080");
        makeChBtn("B", ToneCurveWidget::Channel::Blue,  "#06c");
        btnL->setChecked(true);
        layout->addWidget(chRow);

        toneCurve = new ToneCurveWidget(box);
        layout->addWidget(toneCurve);

        auto* resetCurveBtn = new QPushButton("Reset Curve", box);
        resetCurveBtn->setFixedHeight(20);
        layout->addWidget(resetCurveBtn);

        connect(chGroup, &QButtonGroup::idClicked, this, [this](int id) {
            toneCurve->setChannel(ToneCurveWidget::Channel(id));
        });
        connect(resetCurveBtn, &QPushButton::clicked, this, [this] {
            beforeDrag = adjustments;
            toneCurve->resetChannel(toneCurve->channel());
        });
    }

    // ── Color ─────────────────────────────────────────────────────────────────
    auto* color = makeGroup("Color");
    saturation  = addSlider(color, "Saturation", -100, 100, 0);
    vibrance    = addSlider(color, "Vibrance",   -100, 100, 0);

    // ── HSL / Color Mix ───────────────────────────────────────────────────────
    {
        auto* box    = new QGroupBox("HSL / Color Mix", this);
        auto* layout = new QVBoxLayout(box);
        layout->setSpacing(4);
        root->addWidget(box);

        // Tab buttons: Hue | Saturation | Luminance
        auto* tabRow    = new QWidget(box);
        auto* tabLayout = new QHBoxLayout(tabRow);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(2);
        auto* tabGroup = new QButtonGroup(box);

        hslStack = new QStackedWidget(box);

        auto makeHslPage = [&](std::array<SliderRow, 8>& rows) {
            auto* page   = new QWidget(hslStack);
            auto* pvlay  = new QVBoxLayout(page);
            pvlay->setContentsMargins(0, 0, 0, 0);
            pvlay->setSpacing(1);
            for (int i = 0; i < 8; ++i)
                rows[i] = addSlider(pvlay, kHslRangeNames[i], -100, 100, 0);
            hslStack->addWidget(page);
        };

        makeHslPage(hslHue);
        makeHslPage(hslSat);
        makeHslPage(hslLum);

        auto addTabBtn = [&](const QString& label, int page) {
            auto* btn = new QPushButton(label, tabRow);
            btn->setCheckable(true);
            btn->setFixedHeight(20);
            tabGroup->addButton(btn, page);
            tabLayout->addWidget(btn);
            if (page == 0) btn->setChecked(true);
        };
        addTabBtn("Hue",        0);
        addTabBtn("Saturation", 1);
        addTabBtn("Luminance",  2);

        layout->addWidget(tabRow);
        layout->addWidget(hslStack);

        connect(tabGroup, &QButtonGroup::idClicked, hslStack, &QStackedWidget::setCurrentIndex);
    }

    // ── Detail ────────────────────────────────────────────────────────────────
    auto* detail = makeGroup("Detail");
    sharpening   = addSlider(detail, "Sharpen", 0, 100, 0);

    // ── Geometry ──────────────────────────────────────────────────────────────
    auto* geo = makeGroup("Geometry");
    rotation  = addSlider(geo, "Rotation", -4500, 4500, 0, "°");

    auto* resetBtn = new QPushButton("Reset All", this);
    root->addWidget(resetBtn);
    root->addStretch();

    connect(resetBtn, &QPushButton::clicked, this, &AdjustmentPanel::resetAll);
    connect(this, &AdjustmentPanel::paramsChanged, histogram, &Histogram::setAdjustments);
    connect(wbPresets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0) return;
        temperature.slider->setValue(kWBPresets[i].kelvin);
        tint.slider->setValue(kWBPresets[i].tint);
    });
    connect(rotation.slider, &QSlider::sliderPressed,
            this, [this] { emit straightenActive(true); });
    connect(rotation.slider, &QSlider::sliderReleased,
            this, [this] { emit straightenActive(false); });

    connectSliders();
    connectCurve();
}

// ── Slider factory ────────────────────────────────────────────────────────────

AdjustmentPanel::SliderRow AdjustmentPanel::addSlider(
    QVBoxLayout* layout, const QString& name,
    int min, int max, int def, const QString& suffix)
{
    auto* row   = new QWidget(this);
    auto* hbox  = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);

    auto* lbl   = new QLabel(name, row);
    lbl->setFixedWidth(72);
    auto* sl    = new QSlider(Qt::Horizontal, row);
    sl->setRange(min, max);
    sl->setValue(def);
    auto* val   = new QLabel(QString::number(def) + suffix, row);
    val->setFixedWidth(44);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    hbox->addWidget(lbl);
    hbox->addWidget(sl, 1);
    hbox->addWidget(val);
    layout->addWidget(row);

    connect(sl, &QSlider::valueChanged, val,
            [val, suffix](int v) { val->setText(QString::number(v) + suffix); });

    return {sl, val};
}

// ── Param sync ────────────────────────────────────────────────────────────────

// Pull all slider values into `adjustments`. Sliders store ×100 fixed-point
// where the param is fractional: exposure ±500 → ±5 EV, rotation ±4500 → ±45°.
// cropRect and the curves have no sliders (viewport / curve widget own them),
// so they are carried over untouched.
void AdjustmentPanel::syncParams() {
    const QRectF crop      = adjustments.cropRect;
    adjustments.exposure   = exposure.slider->value()   / 100.0f;
    adjustments.contrast   = float(contrast.slider->value());
    adjustments.highlights = float(highlights.slider->value());
    adjustments.shadows    = float(shadows.slider->value());
    adjustments.whites     = float(whites.slider->value());
    adjustments.blacks     = float(blacks.slider->value());
    adjustments.temperature = float(temperature.slider->value());
    adjustments.tint       = float(tint.slider->value());
    adjustments.saturation = float(saturation.slider->value());
    adjustments.vibrance   = float(vibrance.slider->value());
    adjustments.sharpening = float(sharpening.slider->value());
    adjustments.rotation   = rotation.slider->value() / 100.0f;
    adjustments.cropRect   = crop;
    for (int i = 0; i < 8; ++i) {
        adjustments.hslHue[i] = float(hslHue[i].slider->value());
        adjustments.hslSat[i] = float(hslSat[i].slider->value());
        adjustments.hslLum[i] = float(hslLum[i].slider->value());
    }
}

// ── Connect helpers ───────────────────────────────────────────────────────────

std::vector<QSlider*> AdjustmentPanel::allSliders() const {
    std::vector<QSlider*> sliders = {
        exposure.slider, contrast.slider, highlights.slider,
        shadows.slider,  whites.slider,   blacks.slider,
        temperature.slider, tint.slider,
        saturation.slider, vibrance.slider, sharpening.slider,
        rotation.slider
    };
    for (int i = 0; i < 8; ++i) {
        sliders.push_back(hslHue[i].slider);
        sliders.push_back(hslSat[i].slider);
        sliders.push_back(hslLum[i].slider);
    }
    return sliders;
}

// Each drag gesture becomes one undo entry: sliderPressed snapshots the params
// into beforeDrag, sliderReleased emits the (before, after) pair that
// MainWindow turns into a single AdjustmentCommand.
void AdjustmentPanel::connectSliders() {
    for (auto* s : allSliders()) {
        connect(s, &QSlider::valueChanged, this, [this](int) {
            syncParams();
            emit paramsChanged(adjustments);
        });
        connect(s, &QSlider::sliderPressed,  this, [this] { beforeDrag = adjustments; });
        connect(s, &QSlider::sliderReleased, this, [this] {
            if (!(adjustments == beforeDrag))
                emit adjustmentCommitted(beforeDrag, adjustments);
        });
    }
}

void AdjustmentPanel::connectCurve() {
    connect(toneCurve, &ToneCurveWidget::editingStarted, this, [this] {
        beforeDrag = adjustments;
    });
    connect(toneCurve, &ToneCurveWidget::curveChanged,
            this, [this](ToneCurveWidget::Channel ch, const std::vector<QPointF>& pts) {
        switch (ch) {
        case ToneCurveWidget::Channel::Luma:  adjustments.curveLuma.points = pts; break;
        case ToneCurveWidget::Channel::Red:   adjustments.curveR.points    = pts; break;
        case ToneCurveWidget::Channel::Green: adjustments.curveG.points    = pts; break;
        case ToneCurveWidget::Channel::Blue:  adjustments.curveB.points    = pts; break;
        }
        emit paramsChanged(adjustments);
    });
    connect(toneCurve, &ToneCurveWidget::editingFinished, this, [this] {
        if (!(adjustments == beforeDrag))
            emit adjustmentCommitted(beforeDrag, adjustments);
    });
}

// ── Reset / set params ────────────────────────────────────────────────────────

void AdjustmentPanel::resetAll() {
    setParams(AdjustmentParams{});
}

void AdjustmentPanel::setParams(const AdjustmentParams& p) {
    // Block all slider signals
    const auto sliders = allSliders();
    for (auto* s : sliders) s->blockSignals(true);

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
    for (int i = 0; i < 8; ++i) {
        hslHue[i].slider->setValue(int(p.hslHue[i]));
        hslSat[i].slider->setValue(int(p.hslSat[i]));
        hslLum[i].slider->setValue(int(p.hslLum[i]));
    }

    for (auto* s : sliders) s->blockSignals(false);

    // Curve widget update (no signals needed — setPoints doesn't emit curveChanged)
    toneCurve->setPoints(ToneCurveWidget::Channel::Luma,  p.curveLuma.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Red,   p.curveR.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Green, p.curveG.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Blue,  p.curveB.points);

    adjustments = p;
    emit paramsChanged(adjustments);
}

void AdjustmentPanel::setHistogramImage(const ImageBuffer& buf) {
    histogram->setImage(buf);
}
