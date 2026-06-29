#include "ui/AdjustmentPanel.h"
#include "develop/DemosaicAlgorithm.h"
#include "develop/DevelopParameter.h"
#include "develop/GlobalAdjustment.h"
#include "ui/AdjustmentSpinBox.h"
#include "ui/Histogram.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFont>
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

// Per-row number handling lives in exactly one place: developParameterSpec()
// (DevelopParameter.h). These name the spec for each kind of slider by a
// representative parameter, so the panel and the History labels read identical
// numbers. (Colour-NR Smoothness, like the post-crop/grain shape sliders, resets
// to 50 — Lightroom parity — unlike Strength's 0; Filmic Highlights resets to 25,
// a gentle shoulder on by default — docs/adr/0040.)
static const FieldSpec kExposureSpec = developParameterSpec(DevelopParameter::Exposure).value();
static const FieldSpec kToneSpec = developParameterSpec(DevelopParameter::Contrast).value();
static const FieldSpec kTempSpec = developParameterSpec(DevelopParameter::Temperature).value();
static const FieldSpec kBipolarSpec = developParameterSpec(DevelopParameter::Tint).value();
static const FieldSpec kHslHueSpec = developParameterSpec(DevelopParameter::HslRedHue).value();
static const FieldSpec kSharpenSpec = developParameterSpec(DevelopParameter::Sharpening).value();
static const FieldSpec kFilmicHighlightsSpec
    = developParameterSpec(DevelopParameter::FilmicHighlights).value();
static const FieldSpec kColorSmoothnessSpec
    = developParameterSpec(DevelopParameter::ColorNoiseReductionSmoothness).value();
static const FieldSpec kEffectAmountSpec
    = developParameterSpec(DevelopParameter::PostCropVignetteAmount).value();
static const FieldSpec kEffectShapeSpec
    = developParameterSpec(DevelopParameter::PostCropVignetteMidpoint).value();
static const FieldSpec kGrainAmountSpec
    = developParameterSpec(DevelopParameter::GrainAmount).value();
static const FieldSpec kRotationSpec = developParameterSpec(DevelopParameter::Straighten).value();

// Demosaic algorithms in the combo, ordered soft → sharp (Linear is a diagnostic
// baseline, kept last). Label + tooltip + enum; AHD is the default (docs/adr/0036).
struct DemosaicChoice {
    DemosaicAlgorithm algo;
    const char* label;
    const char* tooltip;
};

static const DemosaicChoice kDemosaicChoices[] = {
    {DemosaicAlgorithm::VNG, "VNG", "Smooth and noise-tolerant; softer detail."},
    {DemosaicAlgorithm::AHD, "AHD (default)", "Balanced default — arraw's original decode."},
    {DemosaicAlgorithm::PPG, "PPG", "Fast and a touch sharper than VNG; mild maze artifacts."},
    {DemosaicAlgorithm::DCB, "DCB", "Detail-oriented; occasional artifacts."},
    {DemosaicAlgorithm::AAHD, "AAHD", "Aliasing-aware AHD variant."},
    {DemosaicAlgorithm::DHT, "DHT", "High detail; slower and noisier on high-ISO frames."},
    {DemosaicAlgorithm::Linear, "Linear", "Bilinear baseline; mainly diagnostic."},
};

// Grain's hidden per-image seed is minted the first time Grain is enabled, and
// must never be zero (0 marks "uninitialised"). Centralise the invariant so the
// two places that surface a GlobalAdjustment can't diverge.
static void ensureGrainSeed(GlobalAdjustment& a) {
    if (a.grainAmount > 0.0f && a.grainSeed == 0)
        a.grainSeed = QRandomGenerator::global()->generate() | 1U;
}

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
    // A bold sub-label that splits one group box into named sub-sections (e.g.
    // Detail's Color Noise, Effects' Post-Crop Vignette / Grain).
    auto subHeader = [this](QVBoxLayout* group, const QString& title) {
        auto* lbl = new QLabel(title, this);
        QFont f = lbl->font();
        f.setBold(true);
        lbl->setFont(f);
        group->addWidget(lbl);
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
    exposure = addSlider(
        tone,
        "Exposure",
        kExposureSpec,
        "Overall brightness, in stops (EV). Shifts the whole image up or down.");
    contrast = addSlider(
        tone,
        "Contrast",
        kToneSpec,
        "Strengthens or softens the difference between light and dark tones, "
        "pivoting around the midtones.");
    highlights = addSlider(
        tone,
        "Highlights",
        kToneSpec,
        "Recovers or brightens the brighter tones, leaving shadows mostly untouched. "
        "Pull it down to bring back detail in skies and bright areas.");
    shadows = addSlider(
        tone,
        "Shadows",
        kToneSpec,
        "Lifts or deepens the darker tones, leaving highlights mostly untouched. "
        "Raise it to open up detail in shadow areas.");
    whites = addSlider(
        tone,
        "Whites",
        kToneSpec,
        "Sets the white point — how bright a tone has to be before it clips to pure "
        "white. Raise for punchier highlights, lower to protect them.");
    blacks = addSlider(
        tone,
        "Blacks",
        kToneSpec,
        "Sets the black point — how dark a tone has to be before it crushes to pure "
        "black. Lower for deeper blacks, raise for a lifted, matte look.");
    // Filmic Highlights (docs/adr/0040): default 25 (gentle shoulder on); 0 = off.
    filmicHighlights = addSlider(
        tone,
        "Filmic Highlights",
        kFilmicHighlightsSpec,
        "Eases the brightest tones gracefully toward white instead of hard-clipping, "
        "fading their colour the way film does. On by default; set to 0 to turn it "
        "off (a hard digital clip).");

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
    subHeader(detail, "Demosaic");
    demosaicCombo = new QComboBox(this);
    demosaicCombo->setObjectName("demosaicCombo");
    for (const auto& choice : kDemosaicChoices)
        demosaicCombo
            ->addItem(choice.label, static_cast<int>(choice.algo)); // tooltip set per item below
    for (int i = 0; i < demosaicCombo->count(); ++i)
        demosaicCombo->setItemData(i, kDemosaicChoices[i].tooltip, Qt::ToolTipRole);
    // Default selection matches GlobalAdjustment's default (AHD); set before the
    // change handler is connected, so it raises no spurious commit/re-decode.
    demosaicCombo->setCurrentIndex(demosaicCombo->findData(static_cast<int>(kDefaultDemosaic)));
    detail->addWidget(demosaicCombo);
    texture = addSlider(
        detail,
        "Texture",
        kBipolarSpec,
        "Emphasises or smooths fine luminance detail without deliberately changing broad "
        "local contrast.");
    clarity = addSlider(
        detail,
        "Clarity",
        kBipolarSpec,
        "Changes midtone local contrast: positive values add punch, negative values soften "
        "regional transitions.");
    dehaze = addSlider(
        detail,
        "Dehaze",
        kBipolarSpec,
        "Reduces or adds veiling light using spatial luminance context.");
    sharpening = addSlider(detail, "Sharpen", kSharpenSpec);
    subHeader(detail, "Color Noise");
    colorNoiseReduction = addSlider(detail, "Strength", kSharpenSpec);
    colorNoiseReductionSmoothness = addSlider(detail, "Smoothness", kColorSmoothnessSpec);

    // ── Geometry ──────────────────────────────────────────────────────────────
    auto* geo = makeGroup("Geometry");
    rotation = addSlider(geo, "Rotation", kRotationSpec);

    // ── Lens Corrections ────────────────────────────────────────────────────────
    auto* lens = makeGroup("Lens Corrections");
    lensProfileLabel = new QLabel("No lens profile", this);
    lensProfileLabel->setObjectName("lensProfileLabel");
    lensProfileLabel->setWordWrap(true);
    lens->addWidget(lensProfileLabel);
    auto addLensToggle = [this, lens](const QString& text, const char* objectName) {
        auto* box = new QCheckBox(text, this);
        box->setObjectName(objectName);
        box->setEnabled(false); // enabled once a profile is set
        lens->addWidget(box);
        connect(box, &QCheckBox::toggled, this, [this] {
            syncParams();
            emit paramsChanged(adjustments);
            commit();
        });
        return box;
    };
    lensCorrectDistortionBox = addLensToggle("Distortion", "lensCorrectDistortionBox");
    lensCorrectVignettingBox = addLensToggle("Vignetting", "lensCorrectVignettingBox");
    lensCorrectCABox = addLensToggle("Chromatic Aberration", "lensCorrectCABox");

    // ── Effects ───────────────────────────────────────────────────────────────
    auto* effects = makeGroup("Effects");
    subHeader(effects, "Post-Crop Vignette");
    postCropVignetteAmount = addSlider(effects, "Amount", kEffectAmountSpec);
    postCropVignetteMidpoint = addSlider(effects, "Midpoint", kEffectShapeSpec);
    postCropVignetteFeather = addSlider(effects, "Feather", kEffectShapeSpec);
    subHeader(effects, "Grain");
    grainAmount = addSlider(effects, "Amount", kGrainAmountSpec);
    grainAmount.slider->setObjectName("grainAmountSlider"); // drives the Grain-seed lifecycle test
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
    // A demosaic change is a discrete decode-time choice (not a drag): it commits
    // immediately as one undo entry. MainWindow turns the commit into a re-decode.
    connect(demosaicCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0)
            return;
        syncParams();
        emit paramsChanged(adjustments);
        commit();
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
    QVBoxLayout* layout, const QString& name, const FieldSpec& spec, const QString& tooltip) {
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

    // One tooltip across the whole row (label, track, and spin) so it shows
    // wherever the pointer rests, not just over the name.
    if (!tooltip.isEmpty()) {
        lbl->setToolTip(tooltip);
        sl->setToolTip(tooltip);
        spin->setToolTip(tooltip);
    }

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
    adjustments.filmicHighlights = v(filmicHighlights);
    adjustments.temperature = v(temperature);
    adjustments.tint = v(tint);
    adjustments.saturation = v(saturation);
    adjustments.vibrance = v(vibrance);
    if (const int i = demosaicCombo->currentIndex(); i >= 0)
        adjustments.demosaicAlgorithm = static_cast<DemosaicAlgorithm>(
            demosaicCombo->itemData(i).toInt());
    adjustments.texture = v(texture);
    adjustments.clarity = v(clarity);
    adjustments.dehaze = v(dehaze);
    adjustments.sharpening = v(sharpening);
    adjustments.colorNoiseReduction = v(colorNoiseReduction); // Strength (issue #59)
    adjustments.colorNoiseReductionSmoothness = v(colorNoiseReductionSmoothness);
    adjustments.rotation = v(rotation);
    adjustments.postCropVignetteAmount = v(postCropVignetteAmount);
    adjustments.postCropVignetteMidpoint = v(postCropVignetteMidpoint);
    adjustments.postCropVignetteFeather = v(postCropVignetteFeather);
    adjustments.grainAmount = v(grainAmount);
    adjustments.grainSize = v(grainSize);
    adjustments.grainRoughness = v(grainRoughness);
    adjustments.lensCorrectDistortion = lensCorrectDistortionBox->isChecked();
    adjustments.lensCorrectVignetting = lensCorrectVignettingBox->isChecked();
    adjustments.lensCorrectCA = lensCorrectCABox->isChecked();
    ensureGrainSeed(adjustments);
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
           &filmicHighlights,
           &temperature,
           &tint,
           &saturation,
           &vibrance,
           &texture,
           &clarity,
           &dehaze,
           &sharpening,
           &colorNoiseReduction,
           &colorNoiseReductionSmoothness,
           &rotation,
           &postCropVignetteAmount,
           &postCropVignetteMidpoint,
           &postCropVignetteFeather,
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
    set(filmicHighlights, p.filmicHighlights);
    set(temperature, p.temperature);
    set(tint, p.tint);
    set(saturation, p.saturation);
    set(vibrance, p.vibrance);
    set(texture, p.texture);
    set(clarity, p.clarity);
    set(dehaze, p.dehaze);
    set(sharpening, p.sharpening);
    set(colorNoiseReduction, p.colorNoiseReduction); // Strength (issue #59)
    set(colorNoiseReductionSmoothness, p.colorNoiseReductionSmoothness);
    set(rotation, p.rotation);
    set(postCropVignetteAmount, p.postCropVignetteAmount);
    set(postCropVignetteMidpoint, p.postCropVignetteMidpoint);
    set(postCropVignetteFeather, p.postCropVignetteFeather);
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

    // Lens-correction toggles (block signals so setParams doesn't emit/commit).
    for (auto* box : {lensCorrectDistortionBox, lensCorrectVignettingBox, lensCorrectCABox})
        box->blockSignals(true);
    lensCorrectDistortionBox->setChecked(p.lensCorrectDistortion);
    lensCorrectVignettingBox->setChecked(p.lensCorrectVignetting);
    lensCorrectCABox->setChecked(p.lensCorrectCA);
    for (auto* box : {lensCorrectDistortionBox, lensCorrectVignettingBox, lensCorrectCABox})
        box->blockSignals(false);

    // Demosaic combo (block signals so setParams doesn't emit/commit/re-decode).
    {
        QSignalBlocker block(demosaicCombo);
        const int idx = demosaicCombo->findData(static_cast<int>(p.demosaicAlgorithm));
        if (idx >= 0)
            demosaicCombo->setCurrentIndex(idx);
    }

    // Curve widget update (no signals needed — setPoints doesn't emit curveChanged)
    toneCurve->setPoints(ToneCurveWidget::Channel::Luma, p.curveLuma.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Red, p.curveR.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Green, p.curveG.points);
    toneCurve->setPoints(ToneCurveWidget::Channel::Blue, p.curveB.points);

    adjustments = p;
    ensureGrainSeed(adjustments);
    committed = adjustments;
    updateCurveChannelIndicators();
    emit paramsChanged(adjustments);
}

void AdjustmentPanel::setLensProfileName(const QString& name) {
    const bool available = !name.isEmpty();
    lensProfileLabel->setText(available ? name : tr("No lens profile"));
    for (auto* box : {lensCorrectDistortionBox, lensCorrectVignettingBox, lensCorrectCABox})
        box->setEnabled(available);
}

void AdjustmentPanel::setDemosaicAvailable(bool available) {
    demosaicCombo->setEnabled(available);
    demosaicCombo->setToolTip(
        available
            ? tr("Choose the RAW demosaic algorithm. Changing it re-decodes the image.")
            : tr("This sensor uses its own decode — Bayer demosaic algorithms do not apply."));
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
