#include "NoiseReduction.h"

#include <algorithm>

float colorNoiseReductionSigmaPx(float amount) {
    // Amount 0..100 → Gaussian sigma 0..25 full-res pixels (linear). 0 is exact.
    return std::max(0.0f, amount) * 0.25f;
}
