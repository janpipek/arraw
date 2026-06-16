#pragma once
#include "FieldSpec.h"
#include "ImagePipeline.h"
#include <array>
#include <vector>
#include <QWidget>

class QListWidget;
class QSlider;
class QPushButton;
class QDoubleSpinBox;
class QCheckBox;
class QStackedWidget;
class AdjustmentSpinBox;

// The "Masks" tab: manage a photo's Local Adjustments (docs/adr/0010) — add,
// select, and delete masks, and edit the active mask's tonal/colour deltas.
// Mask geometry is placed and dragged on the image (ImageViewport); this panel
// owns the list and the delta sliders. The temperature slider here is a
// relative ±100 shift, not the global Kelvin slider.
class LocalAdjustmentPanel : public QWidget {
    Q_OBJECT
public:
    explicit LocalAdjustmentPanel(QWidget* parent = nullptr);

    std::vector<LocalAdjustment> localAdjustments() const { return adjustments; }

    // Replace the whole list (XMP load / undo). Does not emit changed/committed.
    void setLocalAdjustments(const std::vector<LocalAdjustment>& list);

    int activeIndex() const;
    void setActiveIndex(int index);

    // Apply mask geometry changed by on-image dragging (ImageViewport). Refreshes
    // the geometry fields if it is the active mask and emits changed for re-render.
    void updateMaskGeometry(int index, const Mask& mask);

public slots:
    void addLinearMask();
    void addRadialMask();
    void deleteActive();
    // Commit a finished on-image drag as one undo step (called on mouse release).
    void commitMaskEdit();

signals:
    void changed(const std::vector<LocalAdjustment>& list); // live (slider drag)
    void committed(
        const std::vector<LocalAdjustment>& before, // one undo step
        const std::vector<LocalAdjustment>& after);
    void activeIndexChanged(int index);

private:
    struct SliderRow {
        QSlider* slider;
        AdjustmentSpinBox* spin;
        FieldSpec spec;
        float LocalAdjustment::* member; // which delta this row edits
    };

    void addMask(LocalAdjustment la); // shared add: append, select, render, commit
    void rebuildList();
    void loadActiveIntoSliders(); // also loads the geometry fields
    void syncActiveFromSliders();
    void syncActiveGeometry(); // geometry fields -> active mask
    void setSlidersEnabled(bool on);
    void commit();
    LocalAdjustment* active();

    QListWidget* maskList;
    QPushButton* deleteButton;
    std::vector<SliderRow> rows;
    // Geometry fields, normalised display-frame coords (docs/adr/0010). A stacked
    // widget shows the page matching the active mask's type.
    QStackedWidget* geomStack;
    std::array<QDoubleSpinBox*, 4> linSpins; // Linear: p0.x, p0.y, p1.x, p1.y
    std::array<QDoubleSpinBox*, 6> radSpins; // Radial: cx, cy, rx, ry, angle, feather
    QCheckBox* radInvert;

    std::vector<LocalAdjustment> adjustments;
    std::vector<LocalAdjustment> committedState; // undo baseline
};
