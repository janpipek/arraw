#pragma once

namespace arraw {

/// @brief Source orientation, using the eight EXIF orientation values.
enum class ImageOrientation {
    Normal = 1,
    MirrorHorizontal = 2,
    Rotate180 = 3,
    MirrorVertical = 4,
    Transpose = 5,
    Rotate90 = 6,
    Transverse = 7,
    Rotate270 = 8,
};

} // namespace arraw
