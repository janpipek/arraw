#pragma once
#include "FieldSpec.h"
#include "ImagePipeline.h"
#include "ToneCurveWidget.h"
#include <array>
#include <vector>
#include <QHash>
#include <QWidget>

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
    Q_DISABLE_COPY_MOVE(AdjustmentPanel)
public:
    explicit AdjustmentPanel(QWidget* parent = nullptr);

    void setHistogramSamples(const QImage& finalSample, const QImage& curveInputSample);

    GlobalAdjustment params() const { return adjustments; }

    void setParams(const GlobalAdjustment& p);
    void resetAll();

signals:
    void paramsChanged(const GlobalAdjustment&);
    void adjustmentCommitted(const GlobalAdjustment& before, const GlobalAdjustment& after);
    void straightenActive(bool active);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct SliderRow {
        QSlider* slider;
        AdjustmentSpinBox* spin;
        QLabel* nameLabel;
        FieldSpec spec;
    };

    SliderRow addSlider(QVBoxLayout* layout, const QString& name, const FieldSpec& spec);
    std::vector<SliderRow*> allRows();
    void connectRow(SliderRow& row);
    void connectCurve();
    void syncParams();
    void commit();
    void resetRow(SliderRow& row);
    void updateCurveChannelIndicators();

    Histogram* histogram;
    QComboBox* wbPresets;
    ToneCurveWidget* toneCurve;
    std::array<QPushButton*, 4> curveChannelBtns; // indexed by ToneCurveWidget::Channel
    QStackedWidget* hslStack;

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

    GlobalAdjustment adjustments;
    GlobalAdjustment committed; // last committed state — baseline for the next undo entry

    QHash<QObject*, SliderRow*> resetTargets; // slider/label -> row, for double-click reset
};
