#include "NoiseReduction.h"

#include <algorithm>

float colorNoiseReductionSigmaPx(float smoothness) {
    // Smoothness 0..100 → Gaussian sigma 0..25 full-res pixels (linear). 0 is exact.
    return std::max(0.0f, smoothness) * 0.25f;
}

float colorNoiseReductionStrengthMix(float strength) {
    // Strength 0..100 → mix factor 0..1 (linear), clamped so the recombine blend
    // stays a convex combination of raw and blurred chroma.
    return std::clamp(strength, 0.0f, 100.0f) * 0.01f;
}
