#pragma once

#include <ImageBuffer.h>

#include <limits>
#include <type_traits>

namespace arraw {

/// @brief Converts one stored sample to the unit range development works in.
///
/// Shared by the CPU chain, which converts as it develops, and the GPU path,
/// which converts before it uploads, so that there is one conversion to get
/// right rather than two to keep in step.
template <typename Sample> [[nodiscard]] constexpr float toUnit(Sample value) {
    if constexpr (std::is_same_v<Sample, float>) {
        return value;
    } else {
        return static_cast<float>(value) / static_cast<float>(std::numeric_limits<Sample>::max());
    }
}

/// @brief Converts any buffer to RGBA float samples, meaning unchanged.
///
/// Integer samples are scaled into the unit range by ::arraw::toUnit, and a
/// buffer without alpha gains an opaque one, exactly as development treats
/// them. The encoding and the pending orientation are carried over: the
/// numbers change type, not meaning.
/// @param source Buffer to convert.
/// @return A new ::arraw::PixelFormat::RgbaF32 buffer; a copy if @p source
/// already is one.
[[nodiscard]] ImageBuffer toRgbaF32(const ImageBuffer& source);

} // namespace arraw
