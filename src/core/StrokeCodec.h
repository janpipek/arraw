#pragma once

#include "BrushStrokes.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace arraw {

/// A stroke read from a document, as written, with the points that were cut.
struct DecodedStroke {
    Stroke stroke;                 ///< Raw values; the caller normalises them.
    std::size_t pointsDropped = 0; ///< Points past the limit, not kept.
};

/// Writes a stroke as text: `radius hardness flow erase;x,y x,y ...` with the shortest
/// decimal that reads back to the same float.
///
/// The text form exists only as the baseline the prototype's bench B1 measured. ADR 044
/// section 9 chose base64 (::arraw::strokeBase64) for documents, so nothing but the bench and
/// its tests uses this.
[[nodiscard]] std::string strokeText(const Stroke& stroke);

/// Reads a stroke from text.
/// @param pointLimit Points to keep; the rest are checked, cut and counted.
/// @throws std::invalid_argument naming the reason when the text is malformed.
[[nodiscard]] DecodedStroke strokeFromText(std::string_view text,
                                           std::size_t pointLimit = maximumStrokePoints);

/// Writes a stroke as base64 of a compact binary form, the encoding ADR 044 section 9 chose for
/// documents: the style as float bits, then the
/// points as zigzag varints of the differences of their order-preserving bit patterns.
[[nodiscard]] std::string strokeBase64(const Stroke& stroke);

/// Reads a stroke from base64.
/// @param pointLimit Points to keep; the rest are checked, cut and counted.
/// @throws std::invalid_argument naming the reason when the input is malformed.
[[nodiscard]] DecodedStroke strokeFromBase64(std::string_view text,
                                             std::size_t pointLimit = maximumStrokePoints);

} // namespace arraw
