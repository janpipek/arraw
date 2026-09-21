#pragma once

#include <QColorSpace>

#include <ImageBuffer.h>

namespace arraw {

/// @brief Maps a colour encoding to the Qt colour space that defines it.
///
/// One definition serves both import and export, so the two directions cannot
/// disagree about what an encoding means.
/// @param encoding Encoding to resolve.
/// @return The matching colour space.
/// @throws std::invalid_argument if @p encoding is not a recognised value.
[[nodiscard]] QColorSpace colorSpaceFor(ColorEncoding encoding);

} // namespace arraw
