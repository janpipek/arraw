#pragma once
#include "ImagePipeline.h"
#include <QWidget>
#include <array>

class Histogram : public QWidget {
    Q_OBJECT
public:
    explicit Histogram(QWidget* parent = nullptr);

    void setImage(const ImageBuffer& buf);
    void setAdjustments(const AdjustmentParams& p);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void recompute();

    static constexpr int kBins  = 256;
    static constexpr int kStride = 4;   // sample every 4th pixel for speed

    ImageBuffer      rawBuf;
    AdjustmentParams params;

    std::array<int, kBins> r{}, g{}, b{};
    int peak = 1;
};
