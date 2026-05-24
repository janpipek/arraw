#pragma once
#include "ImagePipeline.h"
#include "ToneCurveWidget.h"
#include <array>
#include <QWidget>

class QSlider;
class QLabel;
class QComboBox;
class QVBoxLayout;
class QStackedWidget;
class Histogram;

class AdjustmentPanel : public QWidget {
    Q_OBJECT
public:
    explicit AdjustmentPanel(QWidget* parent = nullptr);

    void setHistogramImage(const ImageBuffer& buf);
    AdjustmentParams params() const { return adjustments; }
    void setParams(const AdjustmentParams& p);
    void resetAll();

signals:
    void paramsChanged(const AdjustmentParams&);
    void adjustmentCommitted(const AdjustmentParams& before, const AdjustmentParams& after);
    void straightenActive(bool active);

private:
    struct SliderRow {
        QSlider* slider;
        QLabel*  valueLabel;
    };

    SliderRow addSlider(QVBoxLayout* layout, const QString& name,
                        int min, int max, int defaultVal,
                        const QString& suffix = {});
    void connectSliders();
    void connectCurve();
    void connectHsl();
    void syncParams();

    Histogram*         histogram;
    QComboBox*         wbPresets;
    ToneCurveWidget*   toneCurve;
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
    AdjustmentParams beforeDrag;
};
