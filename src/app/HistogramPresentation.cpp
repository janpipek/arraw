#include "HistogramPresentation.h"

#include "ui/ThemeColors.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <functional>

namespace arraw::app {

namespace {

/// @brief Gives the bins of a channel by index: 0 red, 1 green, 2 blue.
const OutputHistogram::Bins& channelBins(const OutputHistogram& histogram, std::size_t channel) {
    switch (channel) {
    case 0:
        return histogram.red;
    case 1:
        return histogram.green;
    default:
        return histogram.blue;
    }
}

/// @brief Gives the scale of one channel: its third tallest interior bin.
std::uint64_t channelScale(const OutputHistogram::Bins& bins) {
    std::array<std::uint64_t, outputHistogramBins - 2> interior{};
    std::copy(bins.begin() + 1, bins.end() - 1, interior.begin());
    std::nth_element(interior.begin(), interior.begin() + (histogramScaleRank - 1), interior.end(),
                     std::greater<>());
    return interior[histogramScaleRank - 1];
}

/// @brief Formats a share in percent, with "<0.1" for a share that rounds to nothing.
QString percentText(double percent) {
    if (percent > 0.0 && percent < 0.05) {
        return QStringLiteral("<0.1");
    }
    return QString::number(percent, 'f', 1);
}

/// @brief Computes a count as a percentage of the pixels.
double share(std::uint64_t count, std::uint64_t pixels) {
    return pixels == 0 ? 0.0 : 100.0 * static_cast<double>(count) / static_cast<double>(pixels);
}

} // namespace

std::uint64_t histogramScale(const OutputHistogram& histogram, HistogramChannels channels) {
    if (channels == HistogramChannels::Luma) {
        return channelScale(histogram.luma);
    }
    std::uint64_t scale = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        scale = std::max(scale, channelScale(channelBins(histogram, channel)));
    }
    return scale;
}

ClippedChannels clippedChannels(const OutputHistogram& histogram) {
    const std::size_t last = outputHistogramBins - 1;
    ClippedChannels result;
    result.shadows = {histogram.red[0] != 0, histogram.green[0] != 0, histogram.blue[0] != 0};
    result.highlights = {histogram.red[last] != 0, histogram.green[last] != 0,
                         histogram.blue[last] != 0};
    return result;
}

ClippedShare clippedShare(const OutputHistogram& histogram, ClipKind kind) {
    const std::size_t bin = kind == ClipKind::Shadows ? 0 : outputHistogramBins - 1;
    const std::uint64_t any =
        kind == ClipKind::Shadows ? histogram.shadowsClipped : histogram.highlightsClipped;
    return {share(any, histogram.pixels), share(histogram.red[bin], histogram.pixels),
            share(histogram.green[bin], histogram.pixels),
            share(histogram.blue[bin], histogram.pixels)};
}

QColor clipColour(ChannelSet channels) {
    if (channels.red && channels.green && channels.blue) {
        return theme::brightText;
    }
    const auto pick = [](bool on, const QColor& colour) { return on ? colour : QColor(0, 0, 0); };
    const QColor red = pick(channels.red, theme::histogramRed);
    const QColor green = pick(channels.green, theme::histogramGreen);
    const QColor blue = pick(channels.blue, theme::histogramBlue);
    return {std::max({red.red(), green.red(), blue.red()}),
            std::max({red.green(), green.green(), blue.green()}),
            std::max({red.blue(), green.blue(), blue.blue()})};
}

QString clippedText(const OutputHistogram& histogram, ClipKind kind) {
    const ClippedShare shares = clippedShare(histogram, kind);
    const QString label = kind == ClipKind::Shadows
                              ? QCoreApplication::translate("HistogramPresentation", "Shadows")
                              : QCoreApplication::translate("HistogramPresentation", "Highlights");
    if (shares.any <= 0.0) {
        return QCoreApplication::translate("HistogramPresentation", "%1 clipped: none").arg(label);
    }
    return QCoreApplication::translate("HistogramPresentation",
                                       "%1 clipped: %2 % of pixels (R %3 %, G %4 %, B %5 %)")
        .arg(label, percentText(shares.any), percentText(shares.red), percentText(shares.green),
             percentText(shares.blue));
}

QString readoutText(std::optional<QRgb> colour) {
    const auto part = [&](char letter, int code) {
        const QString value =
            colour ? QString::number(code * 100.0 / 255.0, 'f', 1) : QString(QChar(u'–'));
        return QStringLiteral("%1 %2 %").arg(QLatin1Char(letter), value);
    };
    const QRgb rgb = colour.value_or(0);
    return part('R', qRed(rgb)) + QStringLiteral("   ") + part('G', qGreen(rgb)) +
           QStringLiteral("   ") + part('B', qBlue(rgb));
}

} // namespace arraw::app
