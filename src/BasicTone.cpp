#include "BasicTone.h"
#include <algorithm>
#include <cmath>

namespace tone {
namespace {

float smoothstep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

float mapLuminance(float luminance, const SharedAdjustment& adjustment) {
    constexpr float kGamma = 2.2f;
    constexpr float kSoftAnchor = 0.1f;
    constexpr float kMaxRegionalShift = 0.1f;

    float encoded = std::pow(std::max(luminance, 0.0f), 1.0f / kGamma);
    if (encoded > 0.0f && encoded < 1.0f) {
        const float exposureOdds = std::exp2(adjustment.exposure);
        encoded = exposureOdds * encoded / (1.0f - encoded + exposureOdds * encoded);

        const float contrastPower = std::exp2(adjustment.contrast / 100.0f);
        const float dark = std::pow(encoded, contrastPower);
        const float light = std::pow(1.0f - encoded, contrastPower);
        encoded = dark / (dark + light);
    }

    const float shadowMask = 1.0f - smoothstep(0.35f, 0.65f, encoded);
    const float anchoredShadow = encoded / (encoded + kSoftAnchor);
    encoded += adjustment.shadows / 100.0f * kMaxRegionalShift * anchoredShadow * shadowMask;

    if (encoded < 1.0f) {
        const float highlightMask = smoothstep(0.35f, 0.65f, encoded);
        const float distanceFromWhite = 1.0f - encoded;
        const float anchoredHighlight = distanceFromWhite / (distanceFromWhite + kSoftAnchor);
        encoded += adjustment.highlights / 100.0f * kMaxRegionalShift * anchoredHighlight
                   * highlightMask;
    }

    const float blackMask = 1.0f - smoothstep(0.1f, 0.4f, encoded);
    encoded += adjustment.blacks / 100.0f * 0.1f * blackMask;

    const float whiteMask = smoothstep(0.6f, 0.9f, encoded);
    encoded += adjustment.whites / 100.0f * 0.1f * whiteMask;
    return std::pow(std::max(encoded, 0.0f), kGamma);
}

LutAtlas makeLutAtlas(const GlobalAdjustment& adjustment) {
    LutAtlas atlas;
    const SharedAdjustment neutral;
    for (int row = 0; row < kLutRows; ++row) {
        const SharedAdjustment* rowAdjustment = &neutral;
        if (row == 0)
            rowAdjustment = &adjustment;
        else if (row <= int(adjustment.localAdjustments.size()))
            rowAdjustment = &adjustment.localAdjustments[size_t(row - 1)];

        constexpr float step = 1.0f / float(kLutSize - 1);
        const float endpoint = mapLuminance(1.0f, *rowAdjustment);
        const float highSlope = (mapLuminance(1.0f + step, *rowAdjustment) - endpoint) / step;
        for (int i = 0; i < kLutSize; ++i) {
            const int offset = (row * kLutSize + i) * 4;
            atlas.rgba[offset] = mapLuminance(i * step, *rowAdjustment);
            atlas.rgba[offset + 1] = highSlope;
            atlas.rgba[offset + 3] = 1.0f;
        }
    }
    return atlas;
}

} // namespace tone
