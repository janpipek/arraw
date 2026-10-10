#pragma once

#include <ImageBuffer.h>

namespace arraw {

/// @brief Gives the size of the next level of a pyramid, as ::arraw::halved makes it.
/// @param size Size of the level above.
/// @return Half of @p size in each direction, rounded up.
[[nodiscard]] inline ImageSize halfOf(ImageSize size) {
    return {(size.width + 1) / 2, (size.height + 1) / 2};
}

} // namespace arraw
