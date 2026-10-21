#pragma once

#include <OutputHistogram.h>

#include <QColor>
#include <QRgb>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace arraw::app {

/// @brief Channels a histogram plot draws.
enum class HistogramChannels {
    Rgb, ///< Red, green and blue, overlaid.
    Luma ///< Luminance of the output space.
};

/// @brief Kind of clipping.
enum class ClipKind {
    Shadows,   ///< A channel at code 0.
    Highlights ///< A channel at code 255.
};

/// @brief Rank of the interior bin whose count sets the plot's height: the third tallest.
inline constexpr std::size_t histogramScaleRank = 3;

/// @brief Set of the colour channels.
struct ChannelSet {
    /// @brief Whether red is in the set.
    bool red = false;

    /// @brief Whether green is in the set.
    bool green = false;

    /// @brief Whether blue is in the set.
    bool blue = false;

    /// @brief Tells whether any channel is in the set.
    [[nodiscard]] bool any() const noexcept {
        return red || green || blue;
    }

    friend bool operator==(const ChannelSet&, const ChannelSet&) = default;
};

/// @brief Channels that clip, for each kind.
struct ClippedChannels {
    /// @brief Channels with pixels at code 0.
    ChannelSet shadows;

    /// @brief Channels with pixels at code 255.
    ChannelSet highlights;

    friend bool operator==(const ClippedChannels&, const ClippedChannels&) = default;
};

/// @brief Shares of the pixels that clip, in percent, from 0 to 100.
struct ClippedShare {
    /// @brief Pixels with any channel clipped.
    double any = 0.0;

    /// @brief Pixels with red clipped.
    double red = 0.0;

    /// @brief Pixels with green clipped.
    double green = 0.0;

    /// @brief Pixels with blue clipped.
    double blue = 0.0;
};

/// @brief Computes the count that fills the height of a histogram plot (ADR 055, section 7).
///
/// A soft cap: the ::arraw::app::histogramScaleRank-th tallest of the interior bins (1 to 254)
/// of a channel, so a spike at either end, or one tall bin, does not flatten the rest. For RGB,
/// the largest of the three channels' scales.
/// @param histogram Counts to scale.
/// @param channels Channels that are drawn.
/// @return The count that reaches the top of the plot; 0 if every interior bin of the drawn
/// channels is empty.
[[nodiscard]] std::uint64_t histogramScale(const OutputHistogram& histogram,
                                           HistogramChannels channels);

/// @brief Names the channels that have pixels in their end bins.
/// @param histogram Counts to read.
/// @return The channels with a non-zero bin 0, and with a non-zero bin 255.
[[nodiscard]] ClippedChannels clippedChannels(const OutputHistogram& histogram);

/// @brief Computes the shares of pixels that clip, in percent.
/// @param histogram Counts to read.
/// @param kind Kind of clipping.
/// @return The shares; all zero for a histogram of no pixels.
[[nodiscard]] ClippedShare clippedShare(const OutputHistogram& histogram, ClipKind kind);

/// @brief Gives the colour of a clipping triangle for the channels that clip.
///
/// Red, green or blue alone; two as their sum (yellow, cyan, magenta); all three white.
/// @param channels Channels that clip; at least one.
[[nodiscard]] QColor clipColour(ChannelSet channels);

/// @brief Describes the clipping of a kind in words, for a tooltip.
/// @param histogram Counts to describe.
/// @param kind Kind of clipping.
/// @return For example "Highlights clipped: 1.2 % of pixels (R 1.1 %, G 0.4 %, B 0.2 %)".
[[nodiscard]] QString clippedText(const OutputHistogram& histogram, ClipKind kind);

/// @brief Formats the colour under the pointer, in percent of full scale.
/// @param colour Colour of the pixel; empty off the picture.
/// @return For example "R 45.1 %   G 40.2 %   B 38.0 %", with dashes for an empty colour.
[[nodiscard]] QString readoutText(std::optional<QRgb> colour);

} // namespace arraw::app
