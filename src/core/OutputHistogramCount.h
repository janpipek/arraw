#pragma once

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <OutputHistogram.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace arraw {

/// @brief Rows of work between two looks at the abandon flag.
inline constexpr std::uint32_t outputCountBandRows = 64;

/// @brief Most pixels a histogram of a picture shown is counted over (6 MP); larger pictures are
/// counted on a grid (ADR 055, section 9).
inline constexpr std::uint64_t outputHistogramMaxPixels = 6'000'000;

/// @brief Chooses the grid step that keeps a count within ::arraw::outputHistogramMaxPixels.
/// @param size Pixel dimensions of the picture.
/// @return The smallest step >= 1 for which ceil(width / step) * ceil(height / step) is within
/// the cap; 1 for a picture at or under it.
[[nodiscard]] std::uint32_t outputHistogramStep(ImageSize size);

/// @brief Counts the codes of rows of pixels in an output encoding.
///
/// The one loop behind both ::arraw::outputHistogram overloads and the app, which calls it on a
/// QImage's bits without a copy. Single-threaded.
/// @param rows First byte of the first row.
/// @param size Pixel dimensions.
/// @param stride Bytes between the starts of consecutive rows.
/// @param format Sample layout of the rows; integer samples are in native byte order.
/// @param encoding Output encoding of the samples.
/// @param spans Empty to count whole rows; else exactly one span per row (an empty span for a row
/// with nothing to count), each within the width.
/// @param step Counts only the pixels whose x and y are multiples of it (a grid; no filtering), so
/// a span is honoured and `pixels` is the number counted; 1 counts all.
/// @param abandon If set and true at the start of a band of ::arraw::outputCountBandRows rows,
/// counting stops and the counts so far are returned.
/// @return The counts.
/// @throws std::invalid_argument if @p encoding is not an output encoding, or @p spans is neither
/// empty nor one per row, or a span leaves the row.
[[nodiscard]] OutputHistogram
countOutputCodes(const std::byte* rows, ImageSize size, std::size_t stride, PixelFormat format,
                 NamedEncoding encoding, std::span<const RowSpan> spans = {},
                 const std::atomic<bool>* abandon = nullptr, std::uint32_t step = 1);

} // namespace arraw
