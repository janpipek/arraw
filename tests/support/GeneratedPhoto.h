#pragma once

#include <QColor>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPolygonF>
#include <QString>

#include <filesystem>
#include <stdexcept>

namespace arraw::test {

/// @brief Writes a PNG of a landscape-like picture to look at: a sky gradient, a sun, hills, a
/// grey card and a few coloured shapes.
///
/// Big enough for the window to show handles several pixels apart, which the committed
/// fixtures (32 by 24 pixels and the like) are not.
/// @param path File to write.
/// @param width Width in pixels.
/// @param height Height in pixels.
/// @throws std::runtime_error if the file cannot be written.
inline void writeGeneratedPhoto(const std::filesystem::path& path, int width, int height) {
    QImage image(width, height, QImage::Format_RGB888);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const double w = width;
    const double h = height;

    QLinearGradient sky(0.0, 0.0, 0.0, h * 0.65);
    sky.setColorAt(0.0, QColor(58, 112, 190));
    sky.setColorAt(1.0, QColor(214, 226, 238));
    painter.fillRect(QRectF(0.0, 0.0, w, h), sky);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 246, 214));
    painter.drawEllipse(QPointF(w * 0.78, h * 0.22), h * 0.07, h * 0.07);

    painter.setBrush(QColor(78, 112, 74));
    QPolygonF farHills;
    farHills << QPointF(0.0, h * 0.62) << QPointF(w * 0.25, h * 0.46) << QPointF(w * 0.5, h * 0.60)
             << QPointF(w * 0.8, h * 0.44) << QPointF(w, h * 0.58) << QPointF(w, h)
             << QPointF(0.0, h);
    painter.drawPolygon(farHills);
    painter.setBrush(QColor(110, 138, 84));
    QPolygonF nearHills;
    nearHills << QPointF(0.0, h * 0.78) << QPointF(w * 0.35, h * 0.64) << QPointF(w * 0.7, h * 0.80)
              << QPointF(w, h * 0.70) << QPointF(w, h) << QPointF(0.0, h);
    painter.drawPolygon(nearHills);

    painter.setBrush(QColor(128, 128, 128));
    painter.drawRect(QRectF(w * 0.08, h * 0.70, w * 0.14, h * 0.18));
    painter.setBrush(QColor(196, 62, 54));
    painter.drawRect(QRectF(w * 0.40, h * 0.74, w * 0.10, h * 0.12));
    painter.setBrush(QColor(236, 188, 52));
    painter.drawEllipse(QPointF(w * 0.62, h * 0.82), h * 0.06, h * 0.06);
    painter.end();

    if (!image.save(QString::fromStdU16String(path.u16string()), "PNG")) {
        throw std::runtime_error("Cannot write the generated photograph");
    }
}

} // namespace arraw::test
