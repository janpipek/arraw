#pragma once
#include <array>

// White balance as a per-channel multiplicative gain in linear Rec.2020
// (docs/adr/0025): the gain is blackbody-derived so the Kelvin numbers mean
// something, normalised so neutral (5500 K, tint 0) returns {1,1,1}. Applied as
// c *= gain, so a black pixel stays black by construction.
//
// Pure model→gain math (no image buffers): turns the temperature/tint develop
// params into the numbers the GPU uploads as a uniform, so it lives in the
// develop layer alongside the model, not in the CPU pixel pipeline.
struct WhiteBalance {
    // The neutral illuminant the gain is measured against: WhiteBalance{} is identity.
    static constexpr float kNeutralKelvin = 5500.0f;

    float kelvin = kNeutralKelvin; // absolute (2000..12000)
    float tint = 0.0f;             // slider units (-100..100, + = green, - = magenta)

    // Per-channel gain for this setting, normalised so green == 1 before tint.
    std::array<float, 3> gain() const;

    // Inverse of gain() for the WB picker: given a pre-WB pixel that the user
    // declares neutral, recover the setting that would neutralise it. Takes
    // doubles because the caller averages a pixel neighbourhood in double and
    // the solve is double throughout; only the resulting setting is float.
    static WhiteBalance fromNeutral(double r, double g, double b);
};
