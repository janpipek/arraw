#pragma once

#include <optional>

#include <DevelopSettings.h>
#include <ImageBuffer.h>

namespace arraw {

/// @brief Whether a render may enlarge a photograph beyond its own resolution.
enum class Upscale {
    Never,   ///< A request larger than the source is satisfied by the source.
    Allowed, ///< The photograph is interpolated up to the requested size.
};

/// @brief What a caller wants rendered.
///
/// Defaults to the whole photograph at its own resolution. Both a preview and
/// an export at a chosen size are the same request, which is what keeps them
/// from drifting apart (ADR 007).
struct RenderRequest {
    /// @brief Size to fit the result inside, after the crop.
    ///
    /// Not yet honoured; see ::arraw::develop.
    std::optional<ImageSize> targetSize = std::nullopt;

    /// @brief Whether a target size larger than the photograph enlarges it.
    Upscale upscale = Upscale::Never;
};

/// @brief Renders a decoded photograph through its develop settings.
///
/// The source may be in the working encoding, as anything Qt decoded is, or in
/// a camera's own primaries, as a RAW is. Either way the result is in the
/// working encoding, at ::arraw::workingFormat: the conversion out of camera
/// space is part of developing, not of decoding, because a white balance is
/// only a white balance in the space the sensor recorded (ADR 007).
///
/// With default settings a RAW is converted and nothing else, which is the
/// flat, faithful rendering the command line's help text describes.
///
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param settings Photographic settings to apply.
/// @param request What to render; the default is the whole photograph at its
/// own resolution.
/// @return A new buffer in the working encoding.
/// @throws std::invalid_argument if @p source is in an encoding development
/// cannot start from, or if @p request asks for a size, which is not yet
/// implemented.
[[nodiscard]] ImageBuffer develop(const ImageBuffer& source, const DevelopSettings& settings,
                                  const RenderRequest& request = {});

} // namespace arraw
