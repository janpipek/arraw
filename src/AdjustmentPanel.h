#pragma once
#include "ImagePipeline.h"
#include "ToneCurveWidget.h"
#include "FieldSpec.h"
#include <array>
#include <vector>
#include <QWidget>
#include <QHash>

class QSlider;
class QLabel;
class QComboBox;
class QPushButton;
class QVBoxLayout;
class QStackedWidget;
class QEvent;
class Histogram;
class AdjustmentSpinBox;

class AdjustmentPanel : public QWidget {
    Q_OBJECT
public:
    explicit AdjustmentPanel(QWidget* parent = nullptr);

    void setHistogramSamples(const QImage& finalSample, const QImage& curveInputSample);
    AdjustmentParams params() const { return adjustments; }
    void setParams(const AdjustmentParams& p);
    void resetAll();

signals:
    void paramsChanged(const AdjustmentParams&);
    void adjustmentCommitted(const AdjustmentParams& before, const AdjustmentParams& after);
    void straightenActive(bool active);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct SliderRow {
        QSlider*           slider;
        AdjustmentSpinBox* spin;
        QLabel*            nameLabel;
        FieldSpec          spec;
    };

    SliderRow addSlider(QVBoxLayout* layout, const QString& name,
                        const FieldSpec& spec);
    std::vector<SliderRow*> allRows();
    void connectRow(SliderRow& row);
    void connectCurve();
    void syncParams();
    void commit();
    void resetRow(SliderRow& row);
    void updateCurveChannelIndicators();

    Histogram*         histogram;
    QComboBox*         wbPresets;
    ToneCurveWidget*   toneCurve;
    std::array<QPushButton*, 4> curveChannelBtns;   // indexed by ToneCurveWidget::Channel
    QStackedWidget*    hslStack;

    SliderRow exposure;
    SliderRow contrast;
    SliderRow highlights;
    SliderRow shadows;
    SliderRow whites;
    SliderRow blacks;
    SliderRow temperature;
    SliderRow tint;
    SliderRow saturation;
    SliderRow vibrance;
    SliderRow sharpening;
    SliderRow rotation;

    // HSL: 8 ranges per channel (Hue page=0, Sat page=1, Lum page=2)
    std::array<SliderRow, 8> hslHue;
    std::array<SliderRow, 8> hslSat;
    std::array<SliderRow, 8> hslLum;

    AdjustmentParams adjustments;
    AdjustmentParams committed;    // last committed state — baseline for the next undo entry

    QHash<QObject*, SliderRow*> resetTargets;   // slider/label -> row, for double-click reset
};
