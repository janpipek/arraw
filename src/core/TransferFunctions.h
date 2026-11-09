#pragma once

#include <ColorEncoding.h>

#include <cmath>
#include <stdexcept>

/// @brief Exact transfer curves of the named encodings, in double.
///
/// The one place the curves are written (ADR 057). Every curve is odd, `f(-v) = -f(v)`, so a
/// colour outside the gamut survives a round trip; the standards define them for `v >= 0` only.
namespace arraw::transfer {

/// @brief Signature of a curve from one channel value to another.
using Curve = double (*)(double);

/// Encoded value of the sRGB curve's break, where its linear segment meets its power segment.
inline constexpr double srgbEncodedBreak = 0.04045;

/// Slope of the sRGB curve's linear segment.
inline constexpr double srgbSlope = 12.92;

/// @brief Decodes a value of the sRGB curve (also Display P3's) to linear.
inline double decodeSrgb(double encoded) {
    const double magnitude = std::abs(encoded);
    const double linear = magnitude <= srgbEncodedBreak
                              ? magnitude / srgbSlope
                              : std::pow((magnitude + 0.055) / 1.055, 2.4);
    return std::copysign(linear, encoded);
}

/// @brief Encodes a linear value with the sRGB curve, the exact inverse of ::decodeSrgb.
inline double encodeSrgb(double linear) {
    const double magnitude = std::abs(linear);
    const double encoded = magnitude <= srgbEncodedBreak / srgbSlope
                               ? magnitude * srgbSlope
                               : 1.055 * std::pow(magnitude, 1.0 / 2.4) - 0.055;
    return std::copysign(encoded, linear);
}

/// Exponent of the Adobe RGB (1998) curve, 563/256.
inline constexpr double adobeGamma = 563.0 / 256.0;

/// @brief Decodes a value of the Adobe RGB (1998) curve to linear.
inline double decodeAdobe(double encoded) {
    return std::copysign(std::pow(std::abs(encoded), adobeGamma), encoded);
}

/// @brief Encodes a linear value with the Adobe RGB (1998) curve.
inline double encodeAdobe(double linear) {
    return std::copysign(std::pow(std::abs(linear), 1.0 / adobeGamma), linear);
}

/// Exponent of the perceptual coordinate, 2.2.
inline constexpr double perceptualGamma = 2.2;

/// @brief Decodes a value of the perceptual 2.2 gamma to linear.
inline double decodeGamma22(double encoded) {
    return std::copysign(std::pow(std::abs(encoded), perceptualGamma), encoded);
}

/// @brief Encodes a linear value with the perceptual 2.2 gamma.
inline double encodeGamma22(double linear) {
    return std::copysign(std::pow(std::abs(linear), 1.0 / perceptualGamma), linear);
}

/// @brief Leaves a value as it is, the curve of a linear encoding.
inline double identity(double value) {
    return value;
}

/// @brief Gives the curve that turns an encoding's values into linear ones.
/// @throws std::invalid_argument if @p encoding is not a recognised value.
[[nodiscard]] inline Curve decoderFor(NamedEncoding encoding) {
    switch (encoding) {
    case NamedEncoding::LinearRec2020:
        return identity;
    case NamedEncoding::Srgb:
    case NamedEncoding::DisplayP3:
        return decodeSrgb;
    case NamedEncoding::AdobeRgb:
        return decodeAdobe;
    case NamedEncoding::Rec2020Gamma22:
        return decodeGamma22;
    }
    throw std::invalid_argument("Unknown colour encoding");
}

/// @brief Gives the curve that turns linear values into an encoding's.
/// @throws std::invalid_argument if @p encoding is not a recognised value.
[[nodiscard]] inline Curve encoderFor(NamedEncoding encoding) {
    switch (encoding) {
    case NamedEncoding::LinearRec2020:
        return identity;
    case NamedEncoding::Srgb:
    case NamedEncoding::DisplayP3:
        return encodeSrgb;
    case NamedEncoding::AdobeRgb:
        return encodeAdobe;
    case NamedEncoding::Rec2020Gamma22:
        return encodeGamma22;
    }
    throw std::invalid_argument("Unknown colour encoding");
}

} // namespace arraw::transfer
