#pragma once
#include <array>
#include <QImage>
#include <QWidget>

/**
 * Per-channel bin counts for one shader-rendered sample.
 *
 * The pure (GPU-free) half of the histogram: it turns a small display-space
 * QImage into RGB bin counts plus over/underflow tallies. Format_RGBX32FPx4 is
 * treated as pre-clamp sRGB-linear (values < 0 underflow, > 1 overflow, the
 * rest map through the sRGB gamma to a [0,255] bin); any other format is taken
 * as already-encoded 8-bit and binned by channel value. Split out from the
 * Histogram widget so the readback→bin contract (docs/adr/0004) is unit-tested
 * without a GPU (issue #51).
 */
struct HistogramBins {
    static constexpr int kBins = 256;

    std::array<int, kBins> r{}, g{}, b{};
    int rOver = 0, gOver = 0, bOver = 0;
    int rUnder = 0, gUnder = 0, bUnder = 0;
    int peak = 1;

    // Replace the counts with those of img (re-binnable; clears first).
    void bin(const QImage& img);
};

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
    HistogramBins bins;
};
