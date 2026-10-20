#pragma once

#include <ColorEncoding.h>
#include <CurveHistogram.h>
#include <ImageBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace arraw {

/// @brief Number of bins of an ::arraw::OutputHistogram, one per 8-bit code.
inline constexpr std::size_t outputHistogramBins = 256;

/// @brief Half-open range of the columns of one row that a count covers.
struct RowSpan {
    /// @brief First column counted.
    std::uint32_t begin = 0;

    /// @brief Column after the last one counted; at most the width.
    std::uint32_t end = 0;

    friend bool operator==(const RowSpan&, const RowSpan&) = default;
};

/// @brief Distribution of the codes of a finished picture, in its output encoding.
///
/// Counts the 8-bit picture a screen shows or an 8-bit export writes (ADR 055): bin `i` of a
/// channel holds the pixels whose code in that channel is `i`. Float and 16-bit samples are
/// first rounded to the nearest code, `round(clamp(v, 0, 1) * 255)`, NaN counting as 0. Pixels
/// that are fully transparent are not counted; every other pixel counts once.
struct OutputHistogram {
    /// @brief Pixel counts of one channel, by code; the same type as ::arraw::CurveHistogram::Bins.
    using Bins = std::array<std::uint64_t, outputHistogramBins>;

    /// @brief Output encoding the codes are in.
    NamedEncoding encoding = NamedEncoding::Srgb;

    /// @brief Luminance of the output space, encoded with its transfer function and rounded to a
    /// code: a grey of code `g` has luma `g`.
    Bins luma{};

    /// @brief Red codes.
    Bins red{};

    /// @brief Green codes.
    Bins green{};

    /// @brief Blue codes.
    Bins blue{};

    /// @brief Number of pixels counted: the sum of any one channel's bins.
    std::uint64_t pixels = 0;

    /// @brief Number of pixels with any of R, G, B at code 0.
    std::uint64_t shadowsClipped = 0;

    /// @brief Number of pixels with any of R, G, B at code 255.
    std::uint64_t highlightsClipped = 0;

    friend bool operator==(const OutputHistogram&, const OutputHistogram&) = default;
};

static_assert(std::is_same_v<OutputHistogram::Bins, CurveHistogram::Bins>,
              "one painting helper draws both histograms");
static_assert(outputHistogramBins == curveHistogramBins);

/// @brief Counts a picture that is already in an output encoding.
/// @param picture Samples of any pixel format, in ::arraw::NamedEncoding::Srgb,
/// ::arraw::NamedEncoding::DisplayP3 or ::arraw::NamedEncoding::AdobeRgb.
/// @return The counts, with ::arraw::OutputHistogram::encoding set to the buffer's.
/// @throws std::invalid_argument if the buffer is in the working, perceptual or a camera encoding.
[[nodiscard]] OutputHistogram outputHistogram(const ImageBuffer& picture);

/// @brief Converts a developed render to an output encoding, as an 8-bit export does, and counts
/// it.
///
/// The conversion is the one ::arraw::exportImage makes at 8 bits without sharpening, so the
/// counts equal those of the exported picture, code for code.
/// @param developed Render in any named encoding other than the perceptual one.
/// @param encoding Output encoding to count in.
/// @return The counts.
/// @throws std::invalid_argument if @p encoding is not an output encoding, or the buffer is
/// camera-native or in a layout that cannot be converted.
[[nodiscard]] OutputHistogram outputHistogram(const ImageBuffer& developed, NamedEncoding encoding);

} // namespace arraw
