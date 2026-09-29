#pragma once

#include <ImageBuffer.h>

#include <QImage>

namespace arraw::app {

/// @brief Converts a developed photograph into an image the screen can show.
///
/// Assumes an sRGB display rather than asking the monitor for its profile.
/// The conversion lives in the app for now; the library will own one, shared
/// with export, once it has a public way to convert between encodings.
/// @param developed Result of ::arraw::develop, in the working encoding and format.
/// @return An 8-bit sRGB image owning its own pixels, independent of @p developed.
/// @throws std::invalid_argument if @p developed is not in the working encoding
/// and format, or is too large for QImage.
[[nodiscard]] QImage toDisplayImage(const ImageBuffer& developed);

} // namespace arraw::app
