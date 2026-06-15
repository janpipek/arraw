#include "LocalAdjustmentPanel.h"
#include "AdjustmentSpinBox.h"

#include <algorithm>
#include <variant>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace {
// Per-row number handling (see FieldSpec). All local deltas are ±100 except
// exposure (EV); local temperature is a relative ±100 shift, not Kelvin.
const FieldSpec kExposureSpec{-500, 500, 0, 0.01f, 0.01f, 2, " EV", true, 0.05f};
const FieldSpec kBipolarSpec {-100, 100, 0, 1.0f, 1.0f, 0, {}, true, 1.0f};
}  // namespace

LocalAdjustmentPanel::LocalAdjustmentPanel(QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this);

    maskList = new QListWidget(this);
    maskList->setMaximumHeight(120);
    col->addWidget(maskList);

    auto* buttons = new QHBoxLayout;
    auto* addButton = new QPushButton("Add Linear Mask", this);
    deleteButton = new QPushButton("Delete", this);
    buttons->addWidget(addButton);
    buttons->addWidget(deleteButton);
    col->addLayout(buttons);

    // Linear-mask geometry: P0/P1 in normalised display-frame coords. Editable
    // numerically here and by dragging on the image (kept in sync).
    auto* geomGrid = new QGridLayout;
    const char* geomLabels[4] = {"P0 X", "P0 Y", "P1 X", "P1 Y"};
    const char* geomNames[4]  = {"p0x", "p0y", "p1x", "p1y"};
    for (int i = 0; i < 4; ++i) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(-1.0, 2.0);   // a gradient line may run past the frame edge
        spin->setSingleStep(0.01);
        spin->setDecimals(3);
        spin->setObjectName(geomNames[i]);
        geomGrid->addWidget(new QLabel(geomLabels[i], this), i / 2, (i % 2) * 2);
        geomGrid->addWidget(spin, i / 2, (i % 2) * 2 + 1);
        geomSpins[i] = spin;
    }
    col->addLayout(geomGrid);

    // Delta sliders for the active mask — one row per SharedAdjustment field
    // plus the relative temperature.
    struct RowDef {
        const char* name;
        FieldSpec   spec;
        float LocalAdjustment::* member;
    };
    const RowDef defs[] = {
        {"Exposure",   kExposureSpec, &LocalAdjustment::exposure},
        {"Contrast",   kBipolarSpec,  &LocalAdjustment::contrast},
        {"Highlights", kBipolarSpec,  &LocalAdjustment::highlights},
        {"Shadows",    kBipolarSpec,  &LocalAdjustment::shadows},
        {"Whites",     kBipolarSpec,  &LocalAdjustment::whites},
        {"Blacks",     kBipolarSpec,  &LocalAdjustment::blacks},
        {"Temp",       kBipolarSpec,  &LocalAdjustment::temperature},
        {"Tint",       kBipolarSpec,  &LocalAdjustment::tint},
        {"Saturation", kBipolarSpec,  &LocalAdjustment::saturation},
        {"Vibrance",   kBipolarSpec,  &LocalAdjustment::vibrance},
    };
    for (const RowDef& d : defs) {
        auto* row  = new QWidget(this);
        auto* hbox = new QHBoxLayout(row);
        hbox->setContentsMargins(0, 0, 0, 0);
        auto* lbl  = new QLabel(d.name, row);
        lbl->setFixedWidth(72);
        auto* sl   = new QSlider(Qt::Horizontal, row);
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
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, slider, spec](double d) {
            slider->setValue(spec.displayToRaw(d));   // runs the preview path
            commit();
        });
        connect(spin, &QAbstractSpinBox::editingFinished, this,
                [slider, spin, spec] {
            QSignalBlocker block(spin);
            spin->setValue(spec.rawToDisplay(slider->value()));
        });
    }

    for (QDoubleSpinBox* spin : geomSpins) {
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this] { syncActiveGeometry(); });
        connect(spin, &QAbstractSpinBox::editingFinished, this,
                [this] { commit(); });
    }

    connect(addButton, &QPushButton::clicked, this,
            &LocalAdjustmentPanel::addLinearMask);
    connect(deleteButton, &QPushButton::clicked, this,
            &LocalAdjustmentPanel::deleteActive);
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

void LocalAdjustmentPanel::setLocalAdjustments(
        const std::vector<LocalAdjustment>& list) {
    const int prev = activeIndex();
    adjustments = list;
    committedState = list;
    rebuildList();
    // Preserve the current selection where possible — a commit pushes an undo
    // command whose redo() re-applies the list, and that must not yank the
    // selection back to the first mask. Empty list -> nothing selected; an
    // unset/out-of-range prior selection clamps to the first mask (e.g. load).
    const int sel = list.empty() ? -1
                                  : std::clamp(prev, 0, int(list.size()) - 1);
    setActiveIndex(sel);
    emit changed(adjustments);   // re-render on load and undo/redo (mirrors setParams)
}

void LocalAdjustmentPanel::commitMaskEdit() {
    commit();   // fold an on-image drag gesture into a single undo step
}

void LocalAdjustmentPanel::addLinearMask() {
    LocalAdjustment la;
    la.mask = LinearMask{{0.3, 0.5}, {0.7, 0.5}};   // centred default; drag to place
    adjustments.push_back(la);
    rebuildList();
    setActiveIndex(int(adjustments.size()) - 1);
    emit changed(adjustments);   // push the new list to the viewport (draw handles)
    commit();
}

void LocalAdjustmentPanel::deleteActive() {
    const int i = activeIndex();
    if (i < 0 || i >= int(adjustments.size()))
        return;
    adjustments.erase(adjustments.begin() + i);
    rebuildList();
    setActiveIndex(std::min(i, int(adjustments.size()) - 1));
    emit changed(adjustments);   // push the updated list to the viewport
    commit();
}

void LocalAdjustmentPanel::rebuildList() {
    QSignalBlocker block(maskList);
    const int keep = maskList->currentRow();
    maskList->clear();
    for (int i = 0; i < int(adjustments.size()); ++i)
        maskList->addItem(QStringLiteral("Linear %1").arg(i + 1));
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

    QPointF p0, p1;
    if (a)
        if (const auto* m = std::get_if<LinearMask>(&a->mask)) {
            p0 = m->p0;
            p1 = m->p1;
        }
    const double coords[4] = {p0.x(), p0.y(), p1.x(), p1.y()};
    for (int i = 0; i < 4; ++i) {
        QSignalBlocker b(geomSpins[i]);
        geomSpins[i]->setValue(coords[i]);
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

void LocalAdjustmentPanel::updateMaskGeometry(int index, const LinearMask& mask) {
    if (index < 0 || index >= int(adjustments.size()))
        return;
    adjustments[index].mask = mask;
    if (index == activeIndex())
        loadActiveIntoSliders();   // refresh the P0/P1 fields (signals blocked)
    emit changed(adjustments);
}

void LocalAdjustmentPanel::syncActiveGeometry() {
    LocalAdjustment* a = active();
    if (!a)
        return;
    if (auto* m = std::get_if<LinearMask>(&a->mask)) {
        m->p0 = QPointF(geomSpins[0]->value(), geomSpins[1]->value());
        m->p1 = QPointF(geomSpins[2]->value(), geomSpins[3]->value());
        emit changed(adjustments);
    }
}

void LocalAdjustmentPanel::setSlidersEnabled(bool on) {
    for (SliderRow& r : rows) {
        r.slider->setEnabled(on);
        r.spin->setEnabled(on);
    }
    for (QDoubleSpinBox* spin : geomSpins)
        spin->setEnabled(on);
}

void LocalAdjustmentPanel::commit() {
    if (committedState != adjustments) {
        emit committed(committedState, adjustments);
        committedState = adjustments;
    }
}
