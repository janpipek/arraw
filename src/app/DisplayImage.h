#pragma once

#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>

#include <QImage>
#include <QSize>

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

/// @brief Builds the request a preview develops with, fitted inside a viewport.
///
/// The one an export would make for the same size: fit inside, never enlarge,
/// so a small photograph shows at its own size (ADR 007). Shared by the CPU and
/// the GPU preview so that they cannot drift apart.
/// @param viewport Size of the area to fit inside, in device pixels.
/// @throws std::invalid_argument if the viewport is empty.
[[nodiscard]] RenderRequest previewRequest(QSize viewport);

/// @brief Renders a decoded photograph fitted inside a viewport, ready to show.
///
/// The photograph is developed through the same request an export would use,
/// fitted inside the viewport and never enlarged, so a small one shows at its
/// own size (ADR 007).
/// @param decoded Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param viewport Size of the area to fit inside, in device pixels.
/// @param devicePixelRatio Device pixels per logical pixel of the screen; set on
/// the result so that it covers the viewport once drawn.
/// @return An 8-bit sRGB image no larger than @p viewport, owning its pixels.
/// @throws std::invalid_argument if the viewport is empty, or as ::arraw::develop
/// and toDisplayImage do.
[[nodiscard]] QImage renderForViewport(const ImageBuffer& decoded, const DevelopState& state,
                                       QSize viewport, qreal devicePixelRatio);

} // namespace arraw::app
