#pragma once
#include <array>
#include <QImage>
#include <QWidget>

/**
 * Compact RGB histogram widget for shader-rendered preview samples.
 *
 * Histogram receives small display-space QImages from ImageViewport, bins them
 * on the GUI thread, and paints the result behind the global adjustment controls.
 * It is a presentation widget only; it does not inspect decoded RAW buffers or
 * own develop state.
 */
class Histogram : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(Histogram)
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
