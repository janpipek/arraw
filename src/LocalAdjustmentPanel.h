#pragma once
#include "ImagePipeline.h"
#include "FieldSpec.h"
#include <array>
#include <vector>
#include <QWidget>

class QListWidget;
class QSlider;
class QPushButton;
class QDoubleSpinBox;
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

    int  activeIndex() const;
    void setActiveIndex(int index);

public slots:
    void addLinearMask();
    void deleteActive();

signals:
    void changed(const std::vector<LocalAdjustment>& list);     // live (slider drag)
    void committed(const std::vector<LocalAdjustment>& before,  // one undo step
                   const std::vector<LocalAdjustment>& after);
    void activeIndexChanged(int index);

private:
    struct SliderRow {
        QSlider*           slider;
        AdjustmentSpinBox* spin;
        FieldSpec          spec;
        float LocalAdjustment::* member;   // which delta this row edits
    };

    void rebuildList();
    void loadActiveIntoSliders();      // also loads the geometry fields
    void syncActiveFromSliders();
    void syncActiveGeometry();         // P0/P1 fields -> active mask
    void setSlidersEnabled(bool on);
    void commit();
    LocalAdjustment* active();

    QListWidget*           maskList;
    QPushButton*           deleteButton;
    std::vector<SliderRow> rows;
    // Linear-mask geometry, normalised display-frame coords (docs/adr/0010).
    std::array<QDoubleSpinBox*, 4> geomSpins;   // p0.x, p0.y, p1.x, p1.y

    std::vector<LocalAdjustment> adjustments;
    std::vector<LocalAdjustment> committedState;   // undo baseline
};
