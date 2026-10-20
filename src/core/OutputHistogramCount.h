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
/// @param abandon If set and true at the start of a band of ::arraw::outputCountBandRows rows,
/// counting stops and the counts so far are returned.
/// @return The counts.
/// @throws std::invalid_argument if @p encoding is not an output encoding, or @p spans is neither
/// empty nor one per row, or a span leaves the row.
[[nodiscard]] OutputHistogram countOutputCodes(const std::byte* rows, ImageSize size,
                                               std::size_t stride, PixelFormat format,
                                               NamedEncoding encoding,
                                               std::span<const RowSpan> spans = {},
                                               const std::atomic<bool>* abandon = nullptr);

} // namespace arraw
