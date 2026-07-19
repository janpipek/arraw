#include "pipeline/ImagePipeline.h"
#include "core/Orientation.h"
#include "core/WorkingSpace.h"
#include <algorithm>
#include <cmath>

ImageBuffer downsample2x(const ImageBuffer& src) {
    if (!src.valid())
        return {};

    ImageBuffer dst;
    dst.width = src.width / 2;
    dst.height = src.height / 2;
    dst.data.resize(dst.width * dst.height * 3, 0.0f);

    const int sw = src.width;
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            const int s0 = ((y * 2) * sw + (x * 2)) * 3;
            const int s1 = ((y * 2) * sw + (x * 2 + 1)) * 3;
            const int s2 = ((y * 2 + 1) * sw + (x * 2)) * 3;
            const int s3 = ((y * 2 + 1) * sw + (x * 2 + 1)) * 3;
            const int d = (y * dst.width + x) * 3;
            for (int c = 0; c < 3; ++c)
                dst.data[d + c] = (src.data[s0 + c] + src.data[s1 + c] + src.data[s2 + c]
                                   + src.data[s3 + c])
                                  * 0.25f;
        }
    }
    return dst;
}

QSize developedThumbSize(
    int srcW, int srcH, const QRectF& crop, int maxEdge, orient::Orientation orientation) {
    // An odd quarter-turn presents the buffer with width and height swapped, and
    // the crop is normalised in that oriented frame (docs/adr/0029).
    if (orient::swapsAspect(orientation))
        std::swap(srcW, srcH);
    const int cw = std::max(1, int(std::lround(srcW * crop.width())));
    const int ch = std::max(1, int(std::lround(srcH * crop.height())));

    const int longEdge = std::max(cw, ch);
    if (longEdge <= maxEdge)
        return {cw, ch}; // never upscale

    const double s = double(maxEdge) / double(longEdge);
    return {std::max(1, int(std::lround(cw * s))), std::max(1, int(std::lround(ch * s)))};
}

void normalizeExposure(ImageBuffer& buf) {
    if (!buf.valid())
        return;

    std::vector<float> luma;
    luma.reserve(size_t(buf.width * buf.height / 16 + 1));

    constexpr int kStride = 16;

    const int pixels = buf.width * buf.height;
    for (int i = 0; i < pixels; i += kStride) {
        const float* p = buf.data.data() + i * 3;
        const float y = p[0] * kLumaR + p[1] * kLumaG + p[2] * kLumaB;
        if (std::isfinite(y) && y > 0.0f)
            luma.push_back(y);
    }

    if (luma.empty())
        return;

    const size_t idx = std::min(luma.size() - 1, size_t(luma.size() * 0.995f));
    std::nth_element(luma.begin(), luma.begin() + idx, luma.end());

    const float highlight = luma[idx];
    if (highlight <= 0.0f)
        return;

    constexpr float kTargetHighlight = 0.78f;
    const float gain = std::clamp(kTargetHighlight / highlight, 0.5f, 4.0f);
    for (float& v : buf.data)
        v = std::max(0.0f, v * gain);
}
