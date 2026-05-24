#pragma once
#include "ImagePipeline.h"
#include <QWidget>

class QSlider;
class QLabel;
class QVBoxLayout;
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

private:
    struct SliderRow {
        QSlider* slider;
        QLabel*  valueLabel;
    };

    SliderRow addSlider(QVBoxLayout* layout, const QString& name,
                        int min, int max, int defaultVal);
    void connectAll();

    Histogram* histogram;

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

    AdjustmentParams adjustments;
};
