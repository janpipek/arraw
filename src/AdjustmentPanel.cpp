#include "AdjustmentPanel.h"
#include "AdjustmentSpinBox.h"
#include "Histogram.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

// Per-row number handling lives in exactly one place (see FieldSpec).
// {min, max, def, paramScale, displayScale, decimals, suffix, signed, step}
static const FieldSpec kExposureSpec{-500, 500, 0, 0.01f, 0.01f, 2, " EV", true, 0.05f};
static const FieldSpec kToneSpec{-100, 100, 0, 1.0f, 1.0f, 0, {}, true, 1.0f};
static const FieldSpec kTempSpec{2000, 12000, 5500, 1.0f, 1.0f, 0, " K", false, 50.0f};
static const FieldSpec kBipolarSpec{-100, 100, 0, 1.0f, 1.0f, 0, {}, true, 1.0f};
static const FieldSpec
    kHslHueSpec{-100, 100, 0, 1.0f, 0.3f, 1, QString::fromUtf8("\xc2\xb0"), true, 0.3f};
static const FieldSpec kSharpenSpec{0, 100, 0, 1.0f, 1.0f, 0, {}, false, 1.0f};
static const FieldSpec kEffectAmountSpec{-100, 100, 0, 1.0f, 1.0f, 0, {}, true, 1.0f};
static const FieldSpec kEffectShapeSpec{0, 100, 50, 1.0f, 1.0f, 0, {}, false, 1.0f};
static const FieldSpec kGrainAmountSpec{0, 100, 0, 1.0f, 1.0f, 0, {}, false, 1.0f};
static const FieldSpec
    kRotationSpec{-4500, 4500, 0, 0.01f, 0.01f, 2, QString::fromUtf8("\xc2\xb0"), true, 0.10f};

struct WBPreset {
    const char* name;
    int kelvin;
    int tint;
};

static const WBPreset kWBPresets[] = {
    {"As Shot", 5500, 0},
    {"Daylight", 5500, 0},
    {"Cloudy", 6500, 0},
    {"Shade", 7500, 0},
    {"Tungsten", 3200, 0},
    {"Fluorescent", 4000, 15},
    {"Flash", 5500, 0},
};

static const char* kHslRangeNames[]
    = {"Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues", "Purples", "Magentas"};

AdjustmentPanel::AdjustmentPanel(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(4);

    histogram = new Histogram(this);
    root->addWidget(histogram);

    auto makeGroup = [&](const QString& title) -> QVBoxLayout* {
        auto* box = new QGroupBox(title, this);
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
    temperature = addSlider(wbLayout, "Temp", kTempSpec);
    tint = addSlider(wbLayout, "Tint", kBipolarSpec);

    // ── Tone ──────────────────────────────────────────────────────────────────
    auto* tone = makeGroup("Tone");
    exposure = addSlider(tone, "Exposure", kExposureSpec);
    contrast = addSlider(tone, "Contrast", kToneSpec);
    highlights = addSlider(tone, "Highlights", kToneSpec);
    shadows = addSlider(tone, "Shadows", kToneSpec);
    whites = addSlider(tone, "Whites", kToneSpec);
    blacks = addSlider(tone, "Blacks", kToneSpec);

    // ── Tone Curve ────────────────────────────────────────────────────────────
    {
        auto* box = new QGroupBox("Tone Curve", this);
        auto* layout = new QVBoxLayout(box);
        layout->setSpacing(4);
        root->addWidget(box);

        // Channel selector buttons
        auto* chRow = new QWidget(box);
        auto* chLayout = new QHBoxLayout(chRow);
        chLayout->setContentsMargins(0, 0, 0, 0);
        chLayout->setSpacing(2);

        auto* chGroup = new QButtonGroup(box);
        auto makeChBtn =
            [&](const QString& label, ToneCurveWidget::Channel ch, const QString& color) {
                auto* btn = new QPushButton(label, chRow);
                btn->setCheckable(true);
                btn->setFixedHeight(20);
                btn->setStyleSheet(
                    QString("QPushButton:checked { background: %1; color: white; }").arg(color));
                chGroup->addButton(btn, int(ch));
                chLayout->addWidget(btn);
                curveChannelBtns[int(ch)] = btn;
                return btn;
            };
        auto* btnL = makeChBtn("L", ToneCurveWidget::Channel::Luma, "#555");
        makeChBtn("R", ToneCurveWidget::Channel::Red, "#c03");
        makeChBtn("G", ToneCurveWidget::Channel::Green, "#080");
        makeChBtn("B", ToneCurveWidget::Channel::Blue, "#06c");
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
            toneCurve->resetChannel(toneCurve->channel()); // emits curveChanged
            commit();
        });
    }

    // ── Color ─────────────────────────────────────────────────────────────────
    auto* color = makeGroup("Color");
    saturation = addSlider(color, "Saturation", kBipolarSpec);
    vibrance = addSlider(color, "Vibrance", kBipolarSpec);

    // ── HSL / Color Mix ───────────────────────────────────────────────────────
    {
        auto* box = new QGroupBox("HSL / Color Mix", this);
        auto* layout = new QVBoxLayout(box);
        layout->setSpacing(4);
        root->addWidget(box);

        // Tab buttons: Hue | Saturation | Luminance
        auto* tabRow = new QWidget(box);
        auto* tabLayout = new QHBoxLayout(tabRow);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(2);
        auto* tabGroup = new QButtonGroup(box);

        hslStack = new QStackedWidget(box);

        auto makeHslPage = [&](std::array<SliderRow, 8>& rows, const FieldSpec& spec) {
            auto* page = new QWidget(hslStack);
            auto* pvlay = new QVBoxLayout(page);
            pvlay->setContentsMargins(0, 0, 0, 0);
            pvlay->setSpacing(1);
            for (int i = 0; i < 8; ++i)
                rows[i] = addSlider(pvlay, kHslRangeNames[i], spec);
            hslStack->addWidget(page);
        };

        makeHslPage(hslHue, kHslHueSpec); // shown in degrees (±30°)
        makeHslPage(hslSat, kBipolarSpec);
        makeHslPage(hslLum, kBipolarSpec);

        auto addTabBtn = [&](const QString& label, int page) {
            auto* btn = new QPushButton(label, tabRow);
            btn->setCheckable(true);
            btn->setFixedHeight(20);
            tabGroup->addButton(btn, page);
            tabLayout->addWidget(btn);
            if (page == 0)
                btn->setChecked(true);
        };
        addTabBtn("Hue", 0);
        addTabBtn("Saturation", 1);
        addTabBtn("Luminance", 2);

        layout->addWidget(tabRow);
        layout->addWidget(hslStack);

        connect(tabGroup, &QButtonGroup::idClicked, hslStack, &QStackedWidget::setCurrentIndex);
    }

    // ── Detail ────────────────────────────────────────────────────────────────
    auto* detail = makeGroup("Detail");
    sharpening = addSlider(detail, "Sharpen", kSharpenSpec);

    // ── Geometry ──────────────────────────────────────────────────────────────
    auto* geo = makeGroup("Geometry");
    rotation = addSlider(geo, "Rotation", kRotationSpec);

    // ── Effects ───────────────────────────────────────────────────────────────
    auto* effects = makeGroup("Effects");
    effects->addWidget(new QLabel("Vignette", this));
    vignetteAmount = addSlider(effects, "Amount", kEffectAmountSpec);
    vignetteMidpoint = addSlider(effects, "Midpoint", kEffectShapeSpec);
    vignetteFeather = addSlider(effects, "Feather", kEffectShapeSpec);
    effects->addWidget(new QLabel("Grain", this));
    grainAmount = addSlider(effects, "Amount", kGrainAmountSpec);
    grainSize = addSlider(effects, "Size", kEffectShapeSpec);
    grainRoughness = addSlider(effects, "Roughness", kEffectShapeSpec);

    auto* resetBtn = new QPushButton("Reset All", this);
    root->addWidget(resetBtn);
    root->addStretch();

    connect(resetBtn, &QPushButton::clicked, this, &AdjustmentPanel::resetAll);
    connect(wbPresets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0)
            return;
        temperature.slider->setValue(kWBPresets[i].kelvin);
        tint.slider->setValue(kWBPresets[i].tint);
        commit(); // one undo entry for the whole preset
    });
    connect(rotation.slider, &QSlider::sliderPressed, this, [this] { emit straightenActive(true); });
    connect(rotation.slider, &QSlider::sliderReleased, this, [this] {
        emit straightenActive(false);
    });

    // Wire every row and register double-click-to-reset on its slider and label.
    for (SliderRow* r : allRows()) {
        connectRow(*r);
        r->slider->installEventFilter(this);
        r->nameLabel->installEventFilter(this);
        resetTargets.insert(r->slider, r);
        resetTargets.insert(r->nameLabel, r);
    }
    connectCurve();
}

// ── Slider factory ────────────────────────────────────────────────────────────

AdjustmentPanel::SliderRow AdjustmentPanel::addSlider(
    QVBoxLayout* layout, const QString& name, const FieldSpec& spec) {
    auto* row = new QWidget(this);
    auto* hbox = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);

    auto* lbl = new QLabel(name, row);
    lbl->setFixedWidth(72);
    auto* sl = new QSlider(Qt::Horizontal, row);
    sl->setRange(spec.min, spec.max);
    sl->setValue(spec.def);
    sl->setMinimumWidth(28); // override the ~84px default so the panel can narrow
    auto* spin = new AdjustmentSpinBox(spec, row);
    spin->setValue(spec.rawToDisplay(spec.def));
    spin->setFixedWidth(64);

    hbox->addWidget(lbl);
    hbox->addWidget(sl, 1);
    hbox->addWidget(spin);
    layout->addWidget(row);

    return {sl, spin, lbl, spec};
}

// ── Param sync ────────────────────────────────────────────────────────────────

// Pull all slider values into `adjustments`, each through its FieldSpec's
// raw→param scale (exposure ±500→±5 EV, rotation ±4500→±45°, others 1:1).
// cropRect and the curves have no sliders (viewport / curve widget own them),
// so they are carried over untouched.
void AdjustmentPanel::syncParams() {
    auto v = [](const SliderRow& r) { return r.spec.toParam(r.slider->value()); };
    adjustments.exposure = v(exposure);
    adjustments.contrast = v(contrast);
    adjustments.highlights = v(highlights);
    adjustments.shadows = v(shadows);
    adjustments.whites = v(whites);
    adjustments.blacks = v(blacks);
    adjustments.temperature = v(temperature);
    adjustments.tint = v(tint);
    adjustments.saturation = v(saturation);
    adjustments.vibrance = v(vibrance);
    adjustments.sharpening = v(sharpening);
    adjustments.rotation = v(rotation);
    adjustments.vignetteAmount = v(vignetteAmount);
    adjustments.vignetteMidpoint = v(vignetteMidpoint);
    adjustments.vignetteFeather = v(vignetteFeather);
    adjustments.grainAmount = v(grainAmount);
    adjustments.grainSize = v(grainSize);
    adjustments.grainRoughness = v(grainRoughness);
    if (adjustments.grainAmount > 0.0f && adjustments.grainSeed == 0)
        adjustments.grainSeed = QRandomGenerator::global()->generate() | 1U;
    for (int i = 0; i < 8; ++i) {
        adjustments.hslHue[i] = v(hslHue[i]);
        adjustments.hslSat[i] = v(hslSat[i]);
        adjustments.hslLum[i] = v(hslLum[i]);
    }
}

// ── Connect helpers ───────────────────────────────────────────────────────────

std::vector<AdjustmentPanel::SliderRow*> AdjustmentPanel::allRows() {
    std::vector<SliderRow*> rows
        = {&exposure,
           &contrast,
           &highlights,
           &shadows,
           &whites,
           &blacks,
           &temperature,
           &tint,
           &saturation,
           &vibrance,
           &sharpening,
           &rotation,
           &vignetteAmount,
           &vignetteMidpoint,
           &vignetteFeather,
           &grainAmount,
           &grainSize,
           &grainRoughness};
    for (int i = 0; i < 8; ++i) {
        rows.push_back(&hslHue[i]);
        rows.push_back(&hslSat[i]);
        rows.push_back(&hslLum[i]);
    }
    return rows;
}

// The slider is the source of truth for a row's value; the spinbox is a second
// face on it. Slider moves drive the live preview and mirror into the spinbox;
// spinbox edits (committed on Enter/focus-out, or via scroll/arrows) drive the
// slider, which then re-runs the preview path. Each settled change becomes one
// undo entry via commit() (baseline = last committed state).
void AdjustmentPanel::connectRow(SliderRow& row) {
    auto* slider = row.slider;
    auto* spin = row.spin;
    const FieldSpec spec = row.spec;

    connect(slider, &QSlider::valueChanged, this, [this, spin, spec](int v) {
        QSignalBlocker block(spin);
        spin->setValue(spec.rawToDisplay(v));
        syncParams();
        emit paramsChanged(adjustments);
    });
    connect(slider, &QSlider::sliderReleased, this, [this] { commit(); });

    connect(
        spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, slider, spec](double d) {
            slider->setValue(spec.displayToRaw(d)); // runs the preview path above
            commit();
        });
    // After typing, snap the field text to the slider's actual tick.
    connect(spin, &QAbstractSpinBox::editingFinished, this, [slider, spin, spec] {
        QSignalBlocker block(spin);
        spin->setValue(spec.rawToDisplay(slider->value()));
    });
}

// Emit one undo entry if the settled state differs from the last commit.
void AdjustmentPanel::commit() {
    if (!(adjustments == committed)) {
        emit adjustmentCommitted(committed, adjustments);
        committed = adjustments;
    }
}

void AdjustmentPanel::resetRow(SliderRow& row) {
    row.slider->setValue(row.spec.def); // drives preview + spinbox
    commit();
}

bool AdjustmentPanel::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::MouseButtonDblClick) {
        if (auto* row = resetTargets.value(obj, nullptr)) {
            resetRow(*row);
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void AdjustmentPanel::connectCurve() {
    connect(
        toneCurve,
        &ToneCurveWidget::curveChanged,
        this,
        [this](ToneCurveWidget::Channel ch, const std::vector<QPointF>& pts) {
            switch (ch) {
            case ToneCurveWidget::Channel::Luma:
                adjustments.curveLuma.points = pts;
                break;
            case ToneCurveWidget::Channel::Red:
                adjustments.curveR.points = pts;
                break;
            case ToneCurveWidget::Channel::Green:
                adjustments.curveG.points = pts;
                break;
            case ToneCurveWidget::Channel::Blue:
                adjustments.curveB.points = pts;
                break;
            }
            updateCurveChannelIndicators();
            emit paramsChanged(adjustments);
        });
    connect(toneCurve, &ToneCurveWidget::editingFinished, this, [this] { commit(); });
}

// ── Reset / set params ────────────────────────────────────────────────────────

void AdjustmentPanel::resetAll() {
    setParams(GlobalAdjustment{});
}

void AdjustmentPanel::setParams(const GlobalAdjustment& p) {
    const auto rows = allRows();
    for (auto* r : rows) {
        r->slider->blockSignals(true);
        r->spin->blockSignals(true);
    }

    auto set = [](SliderRow& r, float param) { r.slider->setValue(r.spec.fromParam(param)); };
    set(exposure, p.exposure);
    set(contrast, p.contrast);
    set(highlights, p.highlights);
    set(shadows, p.shadows);
    set(whites, p.whites);
    set(blacks, p.blacks);
    set(temperature, p.temperature);
    set(tint, p.tint);
    set(saturation, p.saturation);
    set(vibrance, p.vibrance);
    set(sharpening, p.sharpening);
    set(rotation, p.rotation);
    set(vignetteAmount, p.vignetteAmount);
    set(vignetteMidpoint, p.vignetteMidpoint);
    set(vignetteFeather, p.vignetteFeather);
    set(grainAmount, p.grainAmount);
    set(grainSize, p.grainSize);
    set(grainRoughness, p.grainRoughness);
    for (int i = 0; i < 8; ++i) {
        set(hslHue[i], p.hslHue[i]);
        set(hslSat[i], p.hslSat[i]);
        set(hslLum[i], p.hslLum[i]);
    }
    // Mirror each slider tick into its spinbox.
    for (auto* r : rows)
        r->spin->setValue(r->spec.rawToDisplay(r->slider->value()));

    for (auto* r : rows) {
        r->slider->blockSignals(false);
        r->spin->blockSignals(false);
    }

    // Curve widget update (no signals needed — setPoints doesn't emit curveChanged)
    toneCurve->setPoints(ToneCurveWidget::Channel::Luma, p.curveLuma.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Red, p.curveR.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Green, p.curveG.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Blue, p.curveB.points);

    adjustments = p;
    if (adjustments.grainAmount > 0.0f && adjustments.grainSeed == 0)
        adjustments.grainSeed = QRandomGenerator::global()->generate() | 1U;
    committed = adjustments;
    updateCurveChannelIndicators();
    emit paramsChanged(adjustments);
}

// A "•" suffix marks channels whose curve is bent — otherwise a non-identity
// curve on an unselected channel is invisible.
void AdjustmentPanel::updateCurveChannelIndicators() {
    static const char* labels[4] = {"L", "R", "G", "B"};
    const CurvePoints* curves[4] = {
        &adjustments.curveLuma,
        &adjustments.curveR,
        &adjustments.curveG,
        &adjustments.curveB,
    };
    for (int i = 0; i < 4; ++i) {
        const QString text = curves[i]->isIdentity() ? QString(labels[i])
                                                     : QString(labels[i]) + "•";
        if (curveChannelBtns[i]->text() != text)
            curveChannelBtns[i]->setText(text);
    }
}

void AdjustmentPanel::setHistogramSamples(const QImage& finalSample, const QImage& curveInputSample) {
    histogram->setSample(finalSample);
    toneCurve->setHistogramSample(curveInputSample);
}
