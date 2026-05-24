#pragma once
#include "ImagePipeline.h"
#include <QWidget>
#include <vector>

class ToneCurveWidget : public QWidget {
    Q_OBJECT
public:
    enum class Channel { Luma, Red, Green, Blue };

    explicit ToneCurveWidget(QWidget* parent = nullptr);

    void setChannel(Channel ch);
    Channel channel() const { return currentChannel; }

    void setPoints(Channel ch, const std::vector<QPointF>& pts);
    const std::vector<QPointF>& points(Channel ch) const;

    void resetChannel(Channel ch);

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

    std::vector<QPointF>& currentPoints();
    const std::vector<QPointF>& currentPoints() const;

    Channel currentChannel = Channel::Luma;
    std::vector<QPointF> pointsLuma = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<QPointF> pointsR    = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<QPointF> pointsG    = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<QPointF> pointsB    = {{0.0, 0.0}, {1.0, 1.0}};

    int  dragIndex  = -1;
    bool dragging   = false;

    static constexpr int kPad    = 10;
    static constexpr int kRadius = 5;
};
