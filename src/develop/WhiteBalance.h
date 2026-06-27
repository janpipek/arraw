#pragma once
#include <array>

// White balance as a per-channel multiplicative gain in linear Rec.2020
// (docs/adr/0025): the gain is blackbody-derived so the Kelvin numbers mean
// something, normalised so neutral (5500 K, tint 0) returns {1,1,1}. Applied as
// c *= gain, so a black pixel stays black by construction. `kelvin` is absolute
// (2000..12000), `tint` is in slider units (-100..100, + = green, - = magenta).
//
// Pure model→gain math (no image buffers): turns the temperature/tint develop
// params into the numbers the GPU uploads as a uniform, so it lives in the
// develop layer alongside the model, not in the CPU pixel pipeline.
std::array<float, 3> whiteBalanceGain(float kelvin, float tint);

// Inverse of whiteBalanceGain for the WB picker: given a pre-WB pixel that the
// user declares neutral, recover the kelvin/tint that would neutralise it.
void whiteBalanceFromNeutral(float r, float g, float b, float& kelvin, float& tint);
