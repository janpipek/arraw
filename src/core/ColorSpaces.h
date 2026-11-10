#pragma once

#include <ColorEncoding.h>

#include <array>
#include <cstddef>
#include <stdexcept>

/// @brief Fixed transforms between the colour spaces arraw names.
namespace arraw::colorspaces {

/// @brief Linear sRGB (Rec.709 primaries) to linear Rec.2020.
///
/// Both spaces are defined against D65, so this is a change of primaries with
/// no chromatic adaptation: the product of sRGB-to-XYZ and XYZ-to-Rec.2020.
/// Every row sums to one, which is the same statement as white staying white.
inline constexpr Matrix3 srgbToWorking{{0.6274039F, 0.3292830F, 0.0433131F, //
                                        0.0690973F, 0.9195404F, 0.0113623F, //
                                        0.0163914F, 0.0880133F, 0.8955953F}};

/// @brief Linear Rec.2020 back to linear sRGB.
inline constexpr Matrix3 workingToSrgb = srgbToWorking.inverse();

/// @brief Relative luminance of a linear Rec.2020 colour, by channel.
///
/// The Rec.2020 coefficients, which sum to one — so a neutral colour's
/// luminance is the value each of its channels holds, and a colour rolled to a
/// given luminance has a neutral counterpart of exactly that value.
inline constexpr Colour workingLuminance{0.2627F, 0.6780F, 0.0593F};

/// @brief CIE XYZ (D65) to linear sRGB.
inline constexpr Matrix3 xyzToSrgb{{3.2404542F, -1.5371385F, -0.4985314F, //
                                    -0.9692660F, 1.8760108F, 0.0415560F,  //
                                    0.0556434F, -0.2040259F, 1.0572252F}};

/// @brief Double-precision 3x3 matrix, row by row, for the transforms derived from chromaticities.
using Matrix3d = std::array<double, 9>;

/// @brief Multiplies two double matrices.
/// @return @p outer applied after @p inner.
[[nodiscard]] constexpr Matrix3d multiply(const Matrix3d& outer, const Matrix3d& inner) {
    Matrix3d result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                sum += outer[row * 3 + k] * inner[k * 3 + column];
            }
            result[row * 3 + column] = sum;
        }
    }
    return result;
}

/// @brief Inverts a double matrix.
/// @throws std::invalid_argument if the matrix is singular.
[[nodiscard]] constexpr Matrix3d invert(const Matrix3d& m) {
    const double determinant = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                               m[1] * (m[3] * m[8] - m[5] * m[6]) +
                               m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (determinant == 0.0) {
        throw std::invalid_argument("A singular colour transform cannot be inverted");
    }
    const double s = 1.0 / determinant;
    return {(m[4] * m[8] - m[5] * m[7]) * s, (m[2] * m[7] - m[1] * m[8]) * s,
            (m[1] * m[5] - m[2] * m[4]) * s, (m[5] * m[6] - m[3] * m[8]) * s,
            (m[0] * m[8] - m[2] * m[6]) * s, (m[2] * m[3] - m[0] * m[5]) * s,
            (m[3] * m[7] - m[4] * m[6]) * s, (m[1] * m[6] - m[0] * m[7]) * s,
            (m[0] * m[4] - m[1] * m[3]) * s};
}

/// @brief CIE xy chromaticities of a set of primaries and the white they are balanced to.
struct Primaries {
    double redX, redY;     ///< Chromaticity of red.
    double greenX, greenY; ///< Chromaticity of green.
    double blueX, blueY;   ///< Chromaticity of blue.

    /// @brief Compares the chromaticities by value.
    bool operator==(const Primaries&) const = default;
};

/// D65 white point, shared by every space arraw names, so none needs chromatic adaptation.
inline constexpr double whiteX = 0.3127;
inline constexpr double whiteY = 0.3290;

/// @brief Linear RGB to XYZ (D65) for a set of primaries.
///
/// Each primary's XYZ column is scaled so that RGB (1,1,1) lands on the white point.
[[nodiscard]] constexpr Matrix3d rgbToXyz(const Primaries& p) {
    const auto z = [](double x, double y) { return 1.0 - x - y; };
    // Columns are the primaries as (x/y, 1, z/y), before scaling.
    const Matrix3d unscaled{p.redX / p.redY,
                            p.greenX / p.greenY,
                            p.blueX / p.blueY,
                            1.0,
                            1.0,
                            1.0,
                            z(p.redX, p.redY) / p.redY,
                            z(p.greenX, p.greenY) / p.greenY,
                            z(p.blueX, p.blueY) / p.blueY};
    const Matrix3d inverse = invert(unscaled);
    const double wx = whiteX / whiteY;
    const double wz = z(whiteX, whiteY) / whiteY;
    const double sr = inverse[0] * wx + inverse[1] + inverse[2] * wz;
    const double sg = inverse[3] * wx + inverse[4] + inverse[5] * wz;
    const double sb = inverse[6] * wx + inverse[7] + inverse[8] * wz;
    return {unscaled[0] * sr, unscaled[1] * sg, unscaled[2] * sb,
            unscaled[3] * sr, unscaled[4] * sg, unscaled[5] * sb,
            unscaled[6] * sr, unscaled[7] * sg, unscaled[8] * sb};
}

/// ITU-R BT.709 primaries, which sRGB shares.
inline constexpr Primaries rec709{0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
/// DCI-P3 primaries, as Display P3 uses them with a D65 white.
inline constexpr Primaries displayP3{0.680, 0.320, 0.265, 0.690, 0.150, 0.060};
/// Adobe RGB (1998) primaries.
inline constexpr Primaries adobeRgb{0.640, 0.330, 0.210, 0.710, 0.150, 0.060};
/// ITU-R BT.2020 primaries, the working space's.
inline constexpr Primaries rec2020{0.708, 0.292, 0.170, 0.797, 0.131, 0.046};

/// @brief Gives the primaries an encoding is defined on.
/// @throws std::invalid_argument if @p encoding is not a recognised value.
[[nodiscard]] constexpr const Primaries& primariesOf(NamedEncoding encoding) {
    switch (encoding) {
    case NamedEncoding::LinearRec2020:
    case NamedEncoding::Rec2020Gamma22:
        return rec2020;
    case NamedEncoding::Srgb:
        return rec709;
    case NamedEncoding::DisplayP3:
        return displayP3;
    case NamedEncoding::AdobeRgb:
        return adobeRgb;
    }
    throw std::invalid_argument("Unknown colour encoding");
}

/// @brief Linear RGB of one encoding to linear RGB of another, in double.
///
/// The product of the source's RGB-to-XYZ and the target's XYZ-to-RGB.
[[nodiscard]] constexpr Matrix3d between(NamedEncoding from, NamedEncoding to) {
    return multiply(invert(rgbToXyz(primariesOf(to))), rgbToXyz(primariesOf(from)));
}

} // namespace arraw::colorspaces
