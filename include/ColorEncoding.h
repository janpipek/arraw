#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <variant>

namespace arraw {

/// @brief Row-major 3x3 colour transform.
///
/// Colour conversions compose, so the pipeline multiplies matrices rather than
/// running a pass per conversion: a white balance and a camera matrix are one
/// multiply per pixel, not two.
struct Matrix3 {
    /// @brief Coefficients, row by row.
    std::array<float, 9> values{};

    /// @brief Builds the identity transform.
    [[nodiscard]] static constexpr Matrix3 identity() {
        return Matrix3{{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F}};
    }

    /// @brief Builds a diagonal transform from per-channel gains.
    /// @param gains Multipliers for R, G, and B.
    [[nodiscard]] static constexpr Matrix3 scale(std::array<float, 3> gains) {
        return Matrix3{{gains[0], 0.0F, 0.0F, 0.0F, gains[1], 0.0F, 0.0F, 0.0F, gains[2]}};
    }

    /// @brief Reads one coefficient.
    /// @param row Row index, 0 to 2.
    /// @param column Column index, 0 to 2.
    [[nodiscard]] constexpr float at(std::size_t row, std::size_t column) const {
        return values[row * 3 + column];
    }

    /// @brief Composes two transforms.
    /// @param inner Transform applied first.
    /// @return The transform equivalent to @p inner followed by this one.
    [[nodiscard]] constexpr Matrix3 operator*(const Matrix3& inner) const {
        Matrix3 result;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                float sum = 0.0F;
                for (std::size_t k = 0; k < 3; ++k) {
                    sum += at(row, k) * inner.at(k, column);
                }
                result.values[row * 3 + column] = sum;
            }
        }
        return result;
    }

    /// @brief Applies the transform to one colour.
    /// @param colour Channel values in the source space.
    /// @return Channel values in the destination space.
    [[nodiscard]] constexpr std::array<float, 3> operator*(std::array<float, 3> colour) const {
        return {at(0, 0) * colour[0] + at(0, 1) * colour[1] + at(0, 2) * colour[2],
                at(1, 0) * colour[0] + at(1, 1) * colour[1] + at(1, 2) * colour[2],
                at(2, 0) * colour[0] + at(2, 1) * colour[1] + at(2, 2) * colour[2]};
    }

    /// @brief Builds the transform that undoes this one.
    /// @return The inverse transform.
    /// @throws std::invalid_argument if the transform collapses a dimension
    /// and cannot be undone.
    [[nodiscard]] constexpr Matrix3 inverse() const {
        const float a = at(0, 0), b = at(0, 1), c = at(0, 2);
        const float d = at(1, 0), e = at(1, 1), f = at(1, 2);
        const float g = at(2, 0), h = at(2, 1), i = at(2, 2);

        const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (determinant == 0.0F) {
            throw std::invalid_argument("A singular colour transform cannot be inverted");
        }
        const float scale = 1.0F / determinant;

        return Matrix3{{(e * i - f * h) * scale, (c * h - b * i) * scale, (b * f - c * e) * scale,
                        (f * g - d * i) * scale, (a * i - c * g) * scale, (c * d - a * f) * scale,
                        (d * h - e * g) * scale, (b * g - a * h) * scale, (a * e - b * d) * scale}};
    }

    friend bool operator==(const Matrix3&, const Matrix3&) = default;
};

/// @brief Colour space with standard primaries and a known transfer function.
enum class NamedEncoding {
    LinearRec2020, ///< Rec.2020 primaries with a linear transfer function.
    Srgb,
    DisplayP3,
    AdobeRgb,
};

/// @brief Per-channel multipliers, in camera channel order.
using Gains = std::array<float, 3>;

/// @brief One colour's three channel values, in whatever encoding holds it.
using Colour = std::array<float, 3>;

/// @brief Colour space of one camera's sensor, described by its own file.
///
/// Neither the primaries nor the transfer function are standard, so the
/// description has to travel with the pixels: nothing downstream can recover
/// it from the samples. See ADR 007 for where each field comes from and why
/// the matrix alone is not enough.
struct CameraNative {
    /// @brief Camera RGB to linear Rec.2020.
    Matrix3 toWorking = Matrix3::identity();

    /// @brief Row scales normalised out of @ref toWorking.
    ///
    /// LibRaw divides each row of the camera matrix by its own sum before
    /// inverting it, which discards how unbalanced the sensor's response is.
    /// Without these, two different calibrations are indistinguishable and a
    /// temperature cannot be resolved into channel gains.
    Gains daylightScale{1.0F, 1.0F, 1.0F};

    /// @brief Gains the camera recorded, normalised so green is 1.
    ///
    /// Multipliers, the reciprocal of DNG's `AsShotNeutral` convention of
    /// neutral channel values.
    Gains asShotMultipliers{1.0F, 1.0F, 1.0F};

    /// @brief Gains the decode actually applied.
    ///
    /// Differs from @ref asShotMultipliers for a file that declares no
    /// neutral, where a fixed daylight substitute is used instead (ADR 005).
    Gains appliedMultipliers{1.0F, 1.0F, 1.0F};

    friend bool operator==(const CameraNative&, const CameraNative&) = default;
};

/// @brief Meaning of an ::ImageBuffer's RGB sample values.
///
/// Either a standard space, or one camera's own primaries carrying the
/// description needed to leave them.
using ColorEncoding = std::variant<NamedEncoding, CameraNative>;

/// @brief Encoding that development happens in; see ADR 003.
inline constexpr NamedEncoding workingEncoding = NamedEncoding::LinearRec2020;

/// @brief Checks whether an encoding is the one development happens in.
/// @param encoding Encoding to test.
/// @return `true` for the working encoding, `false` for any other space,
/// camera-native included.
[[nodiscard]] inline bool isWorkingEncoding(const ColorEncoding& encoding) {
    const auto* named = std::get_if<NamedEncoding>(&encoding);
    return named != nullptr && *named == workingEncoding;
}

} // namespace arraw
