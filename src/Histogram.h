#pragma once
#include "ImagePipeline.h"
#include <QWidget>
#include <array>

class Histogram : public QWidget {
    Q_OBJECT
public:
    explicit Histogram(QWidget* parent = nullptr);
    void setImage(const ImageBuffer& buf);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    static constexpr int kBins = 256;
    std::array<int, kBins> r{}, g{}, b{};
    int peak = 1;
};
