// The perceptual exponents, shared by the passes that use them. Included,
// never compiled on its own.
//
// 1 / 2.2f and 2.2f as C++ rounds them to float, spelled out so that no
// compiler folds the division at another precision: TonePlan.h's
// toPerceptual and toLinear, and Denoise.cpp's perceptual luminance.

const float perceptualExponent = 0.454545438;
const float linearExponent = 2.20000005;
