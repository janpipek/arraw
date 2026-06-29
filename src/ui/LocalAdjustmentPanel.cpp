#include "ui/LocalAdjustmentPanel.h"
#include "develop/LocalAdjustment.h"
#include "ui/AdjustmentSpinBox.h"

#include <algorithm>
#include <variant>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

LocalAdjustmentPanel::LocalAdjustmentPanel(QWidget* parent)
    : QWidget(parent) {
    auto* col = new QVBoxLayout(this);

    maskList = new QListWidget(this);
    maskList->setMaximumHeight(120);
    col->addWidget(maskList);

    auto* buttons = new QHBoxLayout;
    auto* addLinearButton = new QPushButton("Add Linear", this);
    auto* addRadialButton = new QPushButton("Add Radial", this);
    auto* addBrushButton = new QPushButton("Add Brush", this);
    deleteButton = new QPushButton("Delete", this);
    buttons->addWidget(addLinearButton);
    buttons->addWidget(addRadialButton);
    buttons->addWidget(addBrushButton);
    buttons->addWidget(deleteButton);
    col->addLayout(buttons);

    // Geometry fields, normalised display-frame coords. The stacked widget shows
    // the page matching the active mask's type; values stay in sync with the
    // on-image handles. A coordinate spinbox helper keeps the two pages uniform.
    auto makeCoordSpin = [this](const char* name, double lo, double hi) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(lo, hi);
        spin->setSingleStep(0.01);
        spin->setDecimals(3);
        spin->setObjectName(name);
        return spin;
    };

    geomStack = new QStackedWidget(this);

    // Linear page: P0/P1 endpoints (may run past the frame edge).
    auto* linPage = new QWidget(this);
    auto* linGrid = new QGridLayout(linPage);
    const char* linLabels[4] = {"P0 X", "P0 Y", "P1 X", "P1 Y"};
    const char* linNames[4] = {"p0x", "p0y", "p1x", "p1y"};
    for (int i = 0; i < 4; ++i) {
        linSpins[i] = makeCoordSpin(linNames[i], -1.0, 2.0);
        linGrid->addWidget(new QLabel(linLabels[i], linPage), i / 2, (i % 2) * 2);
        linGrid->addWidget(linSpins[i], i / 2, (i % 2) * 2 + 1);
    }
    geomStack->addWidget(linPage);

    // Radial page: centre, radii, angle, feather + invert.
    auto* radPage = new QWidget(this);
    auto* radGrid = new QGridLayout(radPage);

    struct RadDef {
        const char* label;
        const char* name;
        double lo, hi;
    };

    const RadDef radDefs[6] = {
        {"Center X", "cx", -1.0, 2.0},
        {"Center Y", "cy", -1.0, 2.0},
        {"Radius X", "rrx", 0.0, 2.0},
        {"Radius Y", "rry", 0.0, 2.0},
        {"Angle", "rangle", -180.0, 180.0},
        {"Feather", "rfeather", 0.0, 1.0},
    };
    for (int i = 0; i < 6; ++i) {
        radSpins[i] = makeCoordSpin(radDefs[i].name, radDefs[i].lo, radDefs[i].hi);
        radGrid->addWidget(new QLabel(radDefs[i].label, radPage), i / 2, (i % 2) * 2);
        radGrid->addWidget(radSpins[i], i / 2, (i % 2) * 2 + 1);
    }
    radSpins[4]->setSingleStep(1.0); // angle in degrees
    radInvert = new QCheckBox("Invert (affect outside)", radPage);
    radInvert->setObjectName("rinvert");
    radGrid->addWidget(radInvert, 3, 0, 1, 4);
    geomStack->addWidget(radPage);

    // Brush page (docs/adr/0047): tool settings — Size, Feather, Flow, and the
    // Add/Erase mode. These are not stored on the mask; they drive painting.
    auto* brushPage = new QWidget(this);
    auto* brushGrid = new QGridLayout(brushPage);
    auto makeBrushSlider = [brushGrid,
                            brushPage](const char* label, int row, int lo, int hi, int val) {
        brushGrid->addWidget(new QLabel(label, brushPage), row, 0);
        auto* sl = new QSlider(Qt::Horizontal, brushPage);
        sl->setRange(lo, hi);
        sl->setValue(val);
        brushGrid->addWidget(sl, row, 1);
        return sl;
    };
    brushSizeSlider = makeBrushSlider("Size", 0, 1, 100, 20);
    brushFeatherSlider = makeBrushSlider("Feather", 1, 0, 100, 50);
    brushFlowSlider = makeBrushSlider("Flow", 2, 1, 100, 50);
    brushErase = new QCheckBox("Erase", brushPage);
    brushGrid->addWidget(brushErase, 3, 0, 1, 2);
    geomStack->addWidget(brushPage);

    col->addWidget(geomStack);

    // Delta sliders for the active mask — one row per shared develop field plus
    // the relative temperature. The rows (label, number handling, target field)
    // come from the single source shared with History labelling.
    for (const LocalDeltaField& d : localDeltaFields()) {
        auto* row = new QWidget(this);
        auto* hbox = new QHBoxLayout(row);
        hbox->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel(d.label, row);
        lbl->setFixedWidth(72);
        auto* sl = new QSlider(Qt::Horizontal, row);
        sl->setRange(d.spec.min, d.spec.max);
        sl->setMinimumWidth(28);
        auto* spin = new AdjustmentSpinBox(d.spec, row);
        spin->setFixedWidth(64);
        hbox->addWidget(lbl);
        hbox->addWidget(sl, 1);
        hbox->addWidget(spin);
        col->addWidget(row);
        rows.push_back({sl, spin, d.spec, d.member});
    }
    col->addStretch(1);

    for (SliderRow& r : rows) {
        const FieldSpec spec = r.spec;
        QSlider* slider = r.slider;
        AdjustmentSpinBox* spin = r.spin;
        connect(slider, &QSlider::valueChanged, this, [this, spin, spec](int v) {
            QSignalBlocker block(spin);
            spin->setValue(spec.rawToDisplay(v));
            syncActiveFromSliders();
        });
        connect(slider, &QSlider::sliderReleased, this, [this] { commit(); });
        connect(
            spin,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this,
            [this, slider, spec](double d) {
                slider->setValue(spec.displayToRaw(d)); // runs the preview path
                commit();
            });
        connect(spin, &QAbstractSpinBox::editingFinished, this, [slider, spin, spec] {
            QSignalBlocker block(spin);
            spin->setValue(spec.rawToDisplay(slider->value()));
        });
    }

    auto wireGeom = [this](QDoubleSpinBox* spin) {
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
            syncActiveGeometry();
        });
        connect(spin, &QAbstractSpinBox::editingFinished, this, [this] { commit(); });
    };
    for (QDoubleSpinBox* spin : linSpins)
        wireGeom(spin);
    for (QDoubleSpinBox* spin : radSpins)
        wireGeom(spin);
    connect(radInvert, &QCheckBox::toggled, this, [this] {
        syncActiveGeometry();
        commit();
    });

    connect(addLinearButton, &QPushButton::clicked, this, &LocalAdjustmentPanel::addLinearMask);
    connect(addRadialButton, &QPushButton::clicked, this, &LocalAdjustmentPanel::addRadialMask);
    connect(addBrushButton, &QPushButton::clicked, this, &LocalAdjustmentPanel::addBrushMask);
    connect(deleteButton, &QPushButton::clicked, this, &LocalAdjustmentPanel::deleteActive);

    for (QSlider* s : {brushSizeSlider, brushFeatherSlider, brushFlowSlider})
        connect(s, &QSlider::valueChanged, this, [this] { emitBrushSettings(); });
    connect(brushErase, &QCheckBox::toggled, this, [this] { emitBrushSettings(); });
    connect(maskList, &QListWidget::currentRowChanged, this, [this](int row) {
        loadActiveIntoSliders();
        setSlidersEnabled(row >= 0);
        deleteButton->setEnabled(row >= 0);
        emit activeIndexChanged(row);
    });

    setActiveIndex(-1);
}

int LocalAdjustmentPanel::activeIndex() const {
    return maskList->currentRow();
}

LocalAdjustment* LocalAdjustmentPanel::active() {
    const int i = activeIndex();
    return (i >= 0 && i < int(adjustments.size())) ? &adjustments[i] : nullptr;
}

void LocalAdjustmentPanel::setActiveIndex(int index) {
    QSignalBlocker block(maskList);
    maskList->setCurrentRow(index);
    loadActiveIntoSliders();
    setSlidersEnabled(index >= 0);
    deleteButton->setEnabled(index >= 0);
    emit activeIndexChanged(index);
}

void LocalAdjustmentPanel::setLocalAdjustments(const std::vector<LocalAdjustment>& list) {
    const int prev = activeIndex();
    adjustments = list;
    committedState = list;
    rebuildList();
    // Preserve the current selection where possible — a commit pushes an undo
    // command whose redo() re-applies the list, and that must not yank the
    // selection back to the first mask. Empty list -> nothing selected; an
    // unset/out-of-range prior selection clamps to the first mask (e.g. load).
    const int sel = list.empty() ? -1 : std::clamp(prev, 0, int(list.size()) - 1);
    setActiveIndex(sel);
    emit changed(adjustments); // re-render on load and undo/redo (mirrors setParams)
}

void LocalAdjustmentPanel::commitMaskEdit() {
    commit(); // fold an on-image drag gesture into a single undo step
}

void LocalAdjustmentPanel::addMask(LocalAdjustment la) {
    adjustments.push_back(std::move(la));
    rebuildList();
    setActiveIndex(int(adjustments.size()) - 1);
    emit changed(adjustments); // push the new list to the viewport (draw handles)
    commit();
}

void LocalAdjustmentPanel::addLinearMask() {
    LocalAdjustment la;
    la.mask = LinearMask{{0.3, 0.5}, {0.7, 0.5}}; // centred default; drag to place
    addMask(la);
}

void LocalAdjustmentPanel::addRadialMask() {
    LocalAdjustment la;
    la.mask = RadialMask{}; // centred default circle; drag to place/resize
    addMask(la);
}

void LocalAdjustmentPanel::addBrushMask() {
    LocalAdjustment la;
    la.mask = BrushMask{}; // empty raster; paint it on the image (docs/adr/0047)
    addMask(la);
}

void LocalAdjustmentPanel::emitBrushSettings() {
    // Size maps to a fraction of the buffer long edge (max 30%); feather/flow 0..1.
    const double radiusFraction = brushSizeSlider->value() / 100.0 * 0.30;
    const double feather = brushFeatherSlider->value() / 100.0;
    const double flow = brushFlowSlider->value() / 100.0;
    emit brushSettingsChanged(radiusFraction, feather, flow, brushErase->isChecked());
}

void LocalAdjustmentPanel::deleteActive() {
    const int i = activeIndex();
    if (i < 0 || i >= int(adjustments.size()))
        return;
    adjustments.erase(adjustments.begin() + i);
    rebuildList();
    setActiveIndex(std::min(i, int(adjustments.size()) - 1));
    emit changed(adjustments); // push the updated list to the viewport
    commit();
}

void LocalAdjustmentPanel::rebuildList() {
    QSignalBlocker block(maskList);
    const int keep = maskList->currentRow();
    maskList->clear();
    for (int i = 0; i < int(adjustments.size()); ++i)
        maskList->addItem(maskDisplayName(adjustments, i));
    if (keep >= 0 && keep < int(adjustments.size()))
        maskList->setCurrentRow(keep);
}

void LocalAdjustmentPanel::loadActiveIntoSliders() {
    const LocalAdjustment* a = active();
    for (SliderRow& r : rows) {
        QSignalBlocker bs(r.slider);
        QSignalBlocker bp(r.spin);
        const float value = a ? a->*(r.member) : 0.0f;
        r.slider->setValue(r.spec.fromParam(value));
        r.spin->setValue(r.spec.rawToDisplay(r.slider->value()));
    }

    // Geometry — show and load the page matching the active mask's type.
    if (a && std::holds_alternative<BrushMask>(a->mask)) {
        geomStack->setCurrentIndex(2);
        emitBrushSettings(); // sync the viewport's brush tool to the panel values
    } else if (a && std::holds_alternative<RadialMask>(a->mask)) {
        const RadialMask& r = std::get<RadialMask>(a->mask);
        geomStack->setCurrentIndex(1);
        const double vals[6]
            = {r.center.x(), r.center.y(), r.radiusX, r.radiusY, r.angle, r.feather};
        for (int i = 0; i < 6; ++i) {
            QSignalBlocker b(radSpins[i]);
            radSpins[i]->setValue(vals[i]);
        }
        QSignalBlocker bi(radInvert);
        radInvert->setChecked(r.invert);
    } else {
        geomStack->setCurrentIndex(0);
        QPointF p0, p1;
        if (a)
            if (const auto* m = std::get_if<LinearMask>(&a->mask)) {
                p0 = m->p0;
                p1 = m->p1;
            }
        const double coords[4] = {p0.x(), p0.y(), p1.x(), p1.y()};
        for (int i = 0; i < 4; ++i) {
            QSignalBlocker b(linSpins[i]);
            linSpins[i]->setValue(coords[i]);
        }
    }
}

void LocalAdjustmentPanel::syncActiveFromSliders() {
    LocalAdjustment* a = active();
    if (!a)
        return;
    for (SliderRow& r : rows)
        a->*(r.member) = r.spec.toParam(r.slider->value());
    emit changed(adjustments);
}

void LocalAdjustmentPanel::updateMaskGeometry(int index, const Mask& mask) {
    if (index < 0 || index >= int(adjustments.size()))
        return;
    adjustments[index].mask = mask;
    if (index == activeIndex())
        loadActiveIntoSliders(); // refresh the P0/P1 fields (signals blocked)
    emit changed(adjustments);
}

void LocalAdjustmentPanel::syncActiveGeometry() {
    LocalAdjustment* a = active();
    if (!a)
        return;
    if (auto* m = std::get_if<LinearMask>(&a->mask)) {
        m->p0 = QPointF(linSpins[0]->value(), linSpins[1]->value());
        m->p1 = QPointF(linSpins[2]->value(), linSpins[3]->value());
        emit changed(adjustments);
    } else if (auto* r = std::get_if<RadialMask>(&a->mask)) {
        r->center = QPointF(radSpins[0]->value(), radSpins[1]->value());
        r->radiusX = radSpins[2]->value();
        r->radiusY = radSpins[3]->value();
        r->angle = radSpins[4]->value();
        r->feather = radSpins[5]->value();
        r->invert = radInvert->isChecked();
        emit changed(adjustments);
    }
}

void LocalAdjustmentPanel::setSlidersEnabled(bool on) {
    for (SliderRow& r : rows) {
        r.slider->setEnabled(on);
        r.spin->setEnabled(on);
    }
    for (QDoubleSpinBox* spin : linSpins)
        spin->setEnabled(on);
    for (QDoubleSpinBox* spin : radSpins)
        spin->setEnabled(on);
    radInvert->setEnabled(on);
}

void LocalAdjustmentPanel::commit() {
    if (committedState != adjustments) {
        emit committed(committedState, adjustments);
        committedState = adjustments;
    }
}
