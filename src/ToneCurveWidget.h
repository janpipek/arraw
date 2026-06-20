#pragma once
#include "ImagePipeline.h"
#include <array>
#include <vector>
#include <QImage>
#include <QWidget>

class QPainter;

/**
 * Interactive tone-curve editor used by AdjustmentPanel.
 *
 * ToneCurveWidget owns the editable curve points and histogram backdrop for the
 * selected luma/R/G/B channel, emits live curve changes, and marks edit
 * boundaries for undo. AdjustmentPanel translates those widget edits into
 * GlobalAdjustment; DevelopSession owns the committed active-image parameters.
 */
class ToneCurveWidget : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ToneCurveWidget)
public:
    enum class Channel { Luma, Red, Green, Blue };

    explicit ToneCurveWidget(QWidget* parent = nullptr);

    void setChannel(Channel ch);

    Channel channel() const { return currentChannel; }

    void setPoints(Channel ch, const std::vector<QPointF>& pts);
    const std::vector<QPointF>& points(Channel ch) const;

    void resetChannel(Channel ch);

    // Gamma-encoded curve-input sample (ImageViewport::histogramsReady);
    // binned per channel and drawn behind the curve.
    void setHistogramSample(const QImage& img);

    QSize sizeHint() const override { return {200, 160}; }

    QSize minimumSizeHint() const override { return {120, 96}; }

signals:
    void curveChanged(ToneCurveWidget::Channel ch, const std::vector<QPointF>& pts);
    void editingStarted();
    void editingFinished();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    QPointF toWidget(QPointF curve) const;
    QPointF toCurve(QPointF widget) const;
    int hitTest(QPointF widgetPos) const;
    int insertPointOnCurve(QPointF widgetPos);
    void drawHistogram(QPainter& p, const QRectF& area) const;
    void drawCurve(QPainter& p, Channel ch, const QPen& pen);

    std::vector<QPointF>& currentPoints();
    const std::vector<QPointF>& currentPoints() const;
    std::vector<QPointF>& pointsFor(Channel ch);
    const std::array<float, 256>& lutFor(Channel ch);

    // Abandon any in-progress drag. dragIndex is only validated against the
    // current points at press time, so it must be cleared whenever the points
    // it indexes are replaced or the current channel switches (e.g. an undo or
    // a channel shortcut fired mid-drag) — otherwise the next mouse-move would
    // index a stale/shorter vector out of bounds.
    void cancelDrag();

    static QColor channelColor(Channel ch);

    Channel currentChannel = Channel::Luma;
    std::vector<QPointF> pointsLuma = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<QPointF> pointsR = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<QPointF> pointsG = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<QPointF> pointsB = {{0.0, 0.0}, {1.0, 1.0}};

    int dragIndex = -1;
    bool dragging = false;

    // Spline LUT caches for paintEvent — recomputed only when points change.
    std::array<std::vector<QPointF>, 4> lutSource;
    std::array<std::array<float, 256>, 4> lutCache{};

    // Curve input histogram, log-normalised to 0..1, indexed by Channel.
    static constexpr int kHistBins = 128;
    std::array<std::array<float, kHistBins>, 4> hist{};
    bool hasHist = false;

    static constexpr int kPad = 10;
    static constexpr int kRadius = 5;
    static constexpr int kRemoveMargin = 24; // drag a point this far outside to delete
};
