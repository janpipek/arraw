#pragma once

namespace arraw {

/// @brief Smallest and largest hue a Colour Grading zone may be tinted toward, in degrees.
///
/// The wheel is closed: 0 and 360 are the same hue, and both are accepted, so
/// that a hue that wrapped to 360 still fits. The hue is an Oklab hue angle,
/// not Lightroom's (see ::arraw::GradeZone::hue). Settings outside the range
/// are refused where they enter (sidecar, JSON, command line, Python); the
/// pixel maths wraps one that reaches it anyway (ADR 034).
inline constexpr float minimumGradeHue = 0.0F;

/// @copydoc minimumGradeHue
inline constexpr float maximumGradeHue = 360.0F;

/// @brief Weakest and strongest tint a Colour Grading zone may add.
///
/// Zero adds nothing whatever the hue; a hundred adds the most chroma the
/// tint is built to add (ADR 034).
inline constexpr float weakestGrade = 0.0F;

/// @copydoc weakestGrade
inline constexpr float strongestGrade = 100.0F;

/// @brief Furthest Balance may hand the tonal range to either outer zone.
///
/// Balance runs from minus this, which gives the Shadows zone more of the
/// range, to plus this, which gives it to the Highlights zone, as Lightroom's
/// slider does.
inline constexpr float gradeBalanceLimit = 100.0F;

/// @brief Sharpest and softest transitions Blending may ask for between the zones.
inline constexpr float sharpestGradeBlending = 0.0F;

/// @copydoc sharpestGradeBlending
inline constexpr float softestGradeBlending = 100.0F;

/// @brief Tint of one tonal zone: which hue, and how much of it.
struct GradeZone {
    /// @brief Direction of the tint on the colour wheel, in degrees.
    ///
    /// An Oklab hue angle, measured in Oklab's a-b plane from the +a axis
    /// toward +b: roughly 30 is red, 110 yellow, 140 green, 260 blue. This is
    /// not Lightroom's wheel (about 0 red, 60 yellow, 120 green, 240 blue), so
    /// a Lightroom hue does not carry over as the same number (ADR 034).
    float hue = 0.0F;

    /// @brief Strength of the tint, zero for none to a hundred.
    float saturation = 0.0F;

    friend bool operator==(const GradeZone&, const GradeZone&) = default;
};

/// @brief Three-zone toning of the photograph, after its colour or its grey, in domain units.
///
/// A colour control, not a tone control: each zone tints by hue and
/// saturation and leaves lightness where it was, so there is no per-zone
/// brightness (ADR 034). With every saturation at zero nothing changes,
/// whatever the hues, Balance and Blending say.
struct ColorGradingSettings {
    /// @brief Tint of the dark tones.
    GradeZone shadows{};

    /// @brief Tint of the middle tones.
    GradeZone midtones{};

    /// @brief Tint of the light tones.
    GradeZone highlights{};

    /// @brief Where the Shadows zone hands over to the Highlights zone.
    ///
    /// Negative gives the Shadows zone more of the tonal range, positive the
    /// Highlights zone; zero splits it evenly.
    float balance = 0.0F;

    /// @brief How softly the zones blend into each other, zero to a hundred.
    float blending = 50.0F;

    friend bool operator==(const ColorGradingSettings&, const ColorGradingSettings&) = default;
};

} // namespace arraw
