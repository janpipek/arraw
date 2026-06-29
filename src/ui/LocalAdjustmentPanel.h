#pragma once
#include "develop/FieldSpec.h"
#include "develop/LocalAdjustment.h"
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

/**
 * The Masks tab for local develop edits (docs/adr/0010).
 *
 * LocalAdjustmentPanel lets the user add, select, delete, and tune Local
 * Adjustments. It owns the panel-local list copy and delta controls for editing,
 * while ImageViewport edits mask geometry on the image and DevelopSession owns
 * the active image's canonical list. The temperature slider here is a relative
 * +/-100 shift, not the global Kelvin slider.
 */
class LocalAdjustmentPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(LocalAdjustmentPanel)
public:
    explicit LocalAdjustmentPanel(QWidget* parent = nullptr);

    const std::vector<LocalAdjustment>& localAdjustments() const { return adjustments; }

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
    void addBrushMask();
    void deleteActive();
    // Commit a finished on-image drag as one undo step (called on mouse release).
    void commitMaskEdit();

signals:
    void changed(const std::vector<LocalAdjustment>& list); // live (slider drag)
    void committed(
        const std::vector<LocalAdjustment>& before, // one undo step
        const std::vector<LocalAdjustment>& after);
    void activeIndexChanged(int index);
    // Brush tool settings (docs/adr/0047): radius as a fraction of the buffer long
    // edge, feather/flow 0..1, and the Add/Erase mode. Tool state, not per-mask.
    void brushSettingsChanged(double radiusFraction, double feather, double flow, bool erase);

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
    void emitBrushSettings(); // push the brush page values to the viewport
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
    // Brush page (docs/adr/0047): tool settings shown when a Brush mask is active.
    QSlider* brushSizeSlider;
    QSlider* brushFeatherSlider;
    QSlider* brushFlowSlider;
    QCheckBox* brushErase;

    std::vector<LocalAdjustment> adjustments;
    std::vector<LocalAdjustment> committedState; // undo baseline
};
