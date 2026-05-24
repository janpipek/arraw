#include "ImagePipeline.h"

ImageBuffer downsample2x(const ImageBuffer& src) {
    if (!src.valid()) return {};

    ImageBuffer dst;
    dst.width  = src.width  / 2;
    dst.height = src.height / 2;
    dst.data.resize(dst.width * dst.height * 3, 0.0f);

    const int sw = src.width;
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            const int s0 = ((y * 2)     * sw + (x * 2))     * 3;
            const int s1 = ((y * 2)     * sw + (x * 2 + 1)) * 3;
            const int s2 = ((y * 2 + 1) * sw + (x * 2))     * 3;
            const int s3 = ((y * 2 + 1) * sw + (x * 2 + 1)) * 3;
            const int d  = (y * dst.width + x) * 3;
            for (int c = 0; c < 3; ++c)
                dst.data[d + c] = (src.data[s0 + c] + src.data[s1 + c] +
                                   src.data[s2 + c] + src.data[s3 + c]) * 0.25f;
        }
    }
    return dst;
}
