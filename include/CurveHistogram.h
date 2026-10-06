#pragma once

#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace arraw {

/// @brief Number of bins of a ::arraw::CurveHistogram, each 1/256 of the perceptual coordinate.
inline constexpr std::size_t curveHistogramBins = 256;

/// @brief Distribution of the curve input: what a curve widget draws behind its curves.
///
/// Counts over the perceptual coordinate the tone curves act in, from 0 at
/// bin 0 to 1 at the last bin (ADR 010, ADR 035). Bin `i` holds the values `v`
/// with `floor(v * curveHistogramBins) == i`; a value of exactly 1 and anything
/// above white land in the last bin, anything at or below black and NaN in the
/// first, as the curves treat them. Pixels that are fully transparent are not
/// counted; every other pixel counts once.
struct CurveHistogram {
    /// @brief Pixel counts of one channel, by bin.
    using Bins = std::array<std::uint64_t, curveHistogramBins>;

    /// @brief Luminance, as the luma curve reads it: the working luminance of
    /// the linear colour, in the perceptual coordinate. The curve floors a
    /// luminance at 2^-14 before it reads it, which puts anything darker at about
    /// bin 3; the histogram does not floor, and counts it by its value (a negative
    /// luminance in the first bin).
    Bins luma{};

    /// @brief Red channel, as the red curve would read it with the luma curve at identity.
    Bins red{};

    /// @brief Green channel, likewise.
    Bins green{};

    /// @brief Blue channel, likewise.
    Bins blue{};

    /// @brief Number of pixels counted: the sum of any one channel's bins.
    std::uint64_t pixels = 0;

    friend bool operator==(const CurveHistogram&, const CurveHistogram&) = default;
};

/// @brief Counts a curve-input sample into a histogram.
///
/// Computed on the host, from what ::arraw::sample hands back for
/// ::arraw::Tap::CurveInput. The luma is worked out the way the luma curve
/// works out its input: each channel decoded to linear, weighted by the
/// working luminance coefficients, and the sum encoded again.
/// @param curveInput Samples in ::arraw::perceptualEncoding, of any pixel format.
/// @return The counts.
/// @throws std::invalid_argument if @p curveInput is in another encoding.
[[nodiscard]] CurveHistogram curveHistogram(const ImageBuffer& curveInput);

/// @brief Long edge of the default render a curve histogram counts, in pixels.
///
/// 256 bins need nowhere near a full-resolution render: about a million
/// pixels at most, for the cost of a preview rather than of an export.
inline constexpr std::uint32_t curveHistogramLongEdge = 1024;

/// @brief Default render a curve histogram counts.
///
/// The frame fitted inside ::arraw::curveHistogramLongEdge, never enlarged, so a smaller source is
/// counted at its own size. Its filter is Bilinear, which is what ::arraw::curveHistogram uses
/// whatever the request says.
inline constexpr RenderRequest curveHistogramRequest{
    .size = RenderRequest::FitInside{curveHistogramLongEdge, curveHistogramLongEdge},
    .filter = ResizeFilter::Bilinear,
};

/// @brief Samples a photograph at the curve input and counts the result.
///
/// ::arraw::sample at ::arraw::Tap::CurveInput, then the overload above. A
/// preview passes its reduced source and a small request, so that the
/// histogram costs a preview-sized render rather than a full one.
///
/// The resize always uses ::arraw::ResizeFilter::Bilinear, overriding the
/// request's filter: Lanczos rings at hard edges, and its overshoot past black
/// and white would land in the end bins, which a photographer reads as
/// clipping (ADR 035). Call ::arraw::sample directly to count another filter.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param request What to render, see ::arraw::develop; its filter is ignored.
/// The default fits the frame inside ::arraw::curveHistogramLongEdge.
/// @param progress Channel for progress and cancellation, or null; see ::arraw::sample.
/// @return The counts over the rendered frame.
/// @throws std::invalid_argument as ::arraw::sample.
/// @throws ::arraw::Cancelled if @p progress was cancelled before the sample finished.
[[nodiscard]] CurveHistogram curveHistogram(const ImageBuffer& source, const DevelopState& state,
                                            const RenderRequest& request = curveHistogramRequest,
                                            ProgressChannel* progress = nullptr);

} // namespace arraw
