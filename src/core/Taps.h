#pragma once

#include <ColorEncoding.h>
#include <Develop.h>
#include <ImageBuffer.h>

namespace arraw {

/// @brief Gives the encoding a tap's samples are handed back in.
/// @param tap Tap to look up.
/// @return ::arraw::perceptualEncoding for ::arraw::Tap::CurveInput.
/// @throws std::invalid_argument if @p tap is not a tap.
[[nodiscard]] NamedEncoding tapEncoding(Tap tap);

/// @brief Encodes the working-encoding colours a tapped render produced into the tap's encoding.
///
/// The last step of ::arraw::sample on both backends: the chain writes the
/// tap's colour in linear light, geometry and the resize run on it as they
/// would on a render, and this then encodes each colour channel, leaving alpha
/// as it is. One host function, so the backends cannot differ in it.
/// @param linear Tapped pixels, ::arraw::workingFormat in the working encoding.
/// @param tap Tap they were taken at.
/// @return A new ::arraw::workingFormat buffer in ::arraw::tapEncoding of @p tap,
/// with the orientation of @p linear.
/// @throws std::invalid_argument if @p linear is not working-format pixels in the
/// working encoding, or @p tap is not a tap.
[[nodiscard]] ImageBuffer encodeTap(const ImageBuffer& linear, Tap tap);

} // namespace arraw
