#pragma once
#include <array>
#include <QImage>
#include <QWidget>

class Histogram : public QWidget {
    Q_OBJECT
public:
    explicit Histogram(QWidget* parent = nullptr);

    // Display-space sample rendered by the shader (ImageViewport::histogramsReady).
    void setSample(const QImage& img);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    static constexpr int kBins = 256;

    std::array<int, kBins> r{}, g{}, b{};
    int peak = 1;
};
