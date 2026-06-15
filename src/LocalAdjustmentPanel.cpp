#include "LocalAdjustmentPanel.h"
#include "AdjustmentSpinBox.h"

#include <algorithm>
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
    adjustments = list;
    committedState = list;
    rebuildList();
    setActiveIndex(adjustments.empty() ? -1 : 0);
}

void LocalAdjustmentPanel::addLinearMask() {
    LocalAdjustment la;
    la.mask = LinearMask{{0.3, 0.5}, {0.7, 0.5}};   // centred default; drag to place
    adjustments.push_back(la);
    rebuildList();
    setActiveIndex(int(adjustments.size()) - 1);
    commit();
}

void LocalAdjustmentPanel::deleteActive() {
    const int i = activeIndex();
    if (i < 0 || i >= int(adjustments.size()))
        return;
    adjustments.erase(adjustments.begin() + i);
    rebuildList();
    setActiveIndex(std::min(i, int(adjustments.size()) - 1));
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
}

void LocalAdjustmentPanel::syncActiveFromSliders() {
    LocalAdjustment* a = active();
    if (!a)
        return;
    for (SliderRow& r : rows)
        a->*(r.member) = r.spec.toParam(r.slider->value());
    emit changed(adjustments);
}

void LocalAdjustmentPanel::setSlidersEnabled(bool on) {
    for (SliderRow& r : rows) {
        r.slider->setEnabled(on);
        r.spin->setEnabled(on);
    }
}

void LocalAdjustmentPanel::commit() {
    if (committedState != adjustments) {
        emit committed(committedState, adjustments);
        committedState = adjustments;
    }
}
