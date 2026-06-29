#include "core/NoiseReduction.h"

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

float luminanceNoiseReductionAmountMix(float amount) {
    // Amount 0..100 → mix factor 0..1 (linear), clamped — the dual of the chroma
    // Strength mix, blending denoised luma over the original.
    return std::clamp(amount, 0.0f, 100.0f) * 0.01f;
}

float luminanceNoiseReductionRangeSigma(float detail) {
    // Detail 0..100 → bilateral range sigma, decreasing: Detail 0 is the loosest
    // edge-stop (smooths across most luma steps), Detail 100 the tightest (protects
    // nearly every edge). Bounds are fractions of the [0,1] perceptual luma range;
    // the floor stays positive so the blur never degenerates to a no-op.
    constexpr float kLoosest = 0.20f;  // Detail 0
    constexpr float kTightest = 0.02f; // Detail 100
    const float t = std::clamp(detail, 0.0f, 100.0f) * 0.01f;
    return kLoosest + (kTightest - kLoosest) * t;
}
