#include "ImagePipeline.h"
#include <cmath>

ImageBuffer srgbToLinearBuffer(const QImage& img) {
    if (img.isNull()) return {};
    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    const int w = rgb.width(), h = rgb.height();
    ImageBuffer buf;
    buf.width  = w;
    buf.height = h;
    buf.data.resize(size_t(w * h * 3));
    for (int y = 0; y < h; ++y) {
        const uchar* row = rgb.constScanLine(y);
        float* dst = buf.data.data() + y * w * 3;
        for (int x = 0; x < w * 3; ++x) {
            const float v = row[x] * (1.0f / 255.0f);
            dst[x] = v <= 0.04045f ? v / 12.92f
                                   : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
    }
    return buf;
}

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
