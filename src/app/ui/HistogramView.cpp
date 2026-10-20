#include "HistogramView.h"

#include "ThemeColors.h"

#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cstdint>

namespace arraw::app {

namespace {

/// Height of the plot's widget.
constexpr int viewHeight = 110;
/// Width of each gutter that holds a marker: a 3 px column and a pixel either side.
constexpr int gutterWidth = 5;
/// Width of the marker column.
constexpr int markerWidth = 3;
/// Narrowest the plot gets, in pixels; each interior bin is then at least half a pixel.
constexpr int minimumPlotWidth = 127;

/// A channel to draw: its bins and its colour.
struct Layer {
    const OutputHistogram::Bins* bins;
    QColor colour;
};

/// @brief Lists the channels drawn, in drawing order.
std::array<Layer, 3> layersOf(const OutputHistogram& histogram, HistogramChannels channels,
                              const QColor& lumaColour, int& count) {
    if (channels == HistogramChannels::Luma) {
        count = 1;
        return {Layer{&histogram.luma, lumaColour}, {}, {}};
    }
    count = 3;
    return {Layer{&histogram.red, theme::histogramRed},
            Layer{&histogram.green, theme::histogramGreen},
            Layer{&histogram.blue, theme::histogramBlue}};
}

} // namespace

HistogramView::HistogramView(QWidget* parent) : QWidget(parent) {
    setObjectName("histogramView");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(viewHeight);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(tr("Histogram"));
    updateDescription();
}

void HistogramView::setHistogram(std::optional<OutputHistogram> histogram) {
    if (histogram == histogram_) {
        return;
    }
    histogram_ = std::move(histogram);
    updateDescription();
    update();
}

void HistogramView::setChannels(HistogramChannels channels) {
    if (channels == channels_) {
        return;
    }
    channels_ = channels;
    update();
}

QRectF HistogramView::plotRect() const {
    return QRectF(gutterWidth, 1, width() - 2 * gutterWidth, height() - 2);
}

QRectF HistogramView::markerRect(bool highlights) const {
    const QRectF plot = plotRect();
    return QRectF(highlights ? plot.right() : 0.0, plot.top(), gutterWidth, plot.height());
}

QSize HistogramView::sizeHint() const {
    return {256 + 2 * gutterWidth, viewHeight};
}

QSize HistogramView::minimumSizeHint() const {
    return {minimumPlotWidth + 2 * gutterWidth, viewHeight};
}

void HistogramView::updateDescription() {
    if (!histogram_) {
        setAccessibleDescription(tr("No histogram"));
        return;
    }
    setAccessibleDescription(clippedText(*histogram_, ClipKind::Shadows) + QStringLiteral(". ") +
                             clippedText(*histogram_, ClipKind::Highlights));
}

void HistogramView::paintEvent(QPaintEvent* /*event*/) {
    const qreal ratio = devicePixelRatioF();
    QImage canvas((size() * ratio).expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(theme::histogramGround);

    const QRectF plot = plotRect();
    {
        QPainter painter(&canvas);
        // A fixed light tone: the plot's ground is dark whatever the theme.
        const QColor lumaColour = theme::text;
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plot.adjusted(0.5, 0.5, -0.5, -0.5));

        if (!histogram_ && !isEnabled()) {
            // No photograph: the dock is disabled (ADR 048).
            painter.setPen(theme::placeholderText);
            painter.drawText(plot, Qt::AlignCenter, tr("No photograph"));
        }

        if (histogram_ && plot.width() > 0 && plot.height() > 0) {
            int count = 0;
            const auto layers = layersOf(*histogram_, channels_, lumaColour, count);
            const std::uint64_t scale = histogramScale(*histogram_, channels_);
            const qreal binWidth = plot.width() / static_cast<qreal>(outputHistogramBins - 2);
            const qreal inner = plot.height() - 2;

            if (scale > 0) {
                const qreal unit = inner / static_cast<qreal>(scale);
                painter.setCompositionMode(channels_ == HistogramChannels::Rgb
                                               ? QPainter::CompositionMode_Plus
                                               : QPainter::CompositionMode_SourceOver);
                for (int layer = 0; layer < count; ++layer) {
                    const OutputHistogram::Bins& bins = *layers[layer].bins;
                    QPainterPath area;
                    area.moveTo(plot.left(), plot.bottom() - 1);
                    for (std::size_t bin = 1; bin < outputHistogramBins - 1; ++bin) {
                        const qreal top = plot.bottom() - 1 -
                                          std::min(inner, static_cast<qreal>(bins[bin]) * unit);
                        const qreal x = plot.left() + static_cast<qreal>(bin - 1) * binWidth;
                        area.lineTo(x, top);
                        area.lineTo(x + binWidth, top);
                    }
                    area.lineTo(plot.right(), plot.bottom() - 1);
                    area.closeSubpath();
                    QColor fill = layers[layer].colour;
                    if (channels_ == HistogramChannels::Luma) {
                        fill.setAlpha(200);
                    }
                    painter.fillPath(area, fill);
                }

                // Where bins are cut off, a line at the top.
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                QColor cap = theme::histogramCap;
                cap.setAlpha(220);
                for (int layer = 0; layer < count; ++layer) {
                    const OutputHistogram::Bins& bins = *layers[layer].bins;
                    for (std::size_t bin = 1; bin < outputHistogramBins - 1; ++bin) {
                        if (bins[bin] > scale) {
                            painter.fillRect(
                                QRectF(plot.left() + static_cast<qreal>(bin - 1) * binWidth,
                                       plot.top() + 1, std::max(binWidth, 1.0), 1.0),
                                cap);
                        }
                    }
                }
            }

            // Markers of the end bins, on the plot's scale; with an empty interior, on their own.
            std::uint64_t markerScale = scale;
            if (markerScale == 0) {
                for (int layer = 0; layer < count; ++layer) {
                    const OutputHistogram::Bins& bins = *layers[layer].bins;
                    markerScale = std::max({markerScale, bins.front(), bins.back()});
                }
            }
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            if (markerScale > 0) {
                for (const bool highlights : {false, true}) {
                    const QRectF gutter = markerRect(highlights);
                    const qreal columnLeft = gutter.left() + (gutterWidth - markerWidth) / 2.0;
                    const qreal columnWidth = static_cast<qreal>(markerWidth) / count;
                    for (int layer = 0; layer < count; ++layer) {
                        const OutputHistogram::Bins& bins = *layers[layer].bins;
                        const std::uint64_t value = highlights ? bins.back() : bins.front();
                        if (value == 0) {
                            continue;
                        }
                        const qreal height =
                            std::clamp(static_cast<qreal>(value) / static_cast<qreal>(markerScale) *
                                           gutter.height(),
                                       1.0, gutter.height());
                        painter.fillRect(QRectF(columnLeft + layer * columnWidth,
                                                gutter.bottom() - height, columnWidth, height),
                                         layers[layer].colour);
                    }
                }
            }
        }
    }

    QPainter(this).drawImage(0, 0, canvas);
}

} // namespace arraw::app
