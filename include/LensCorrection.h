#pragma once

#include <DevelopedFrame.h>
#include <ImageBuffer.h>
#include <LensMatch.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief What a lens profile resolves to, independent of where it came from (ADR 050, section 3).

namespace arraw {

/// @brief Which of the three corrections a profile is wanted for.
struct LensKinds {
    bool distortion = false; ///< Geometric distortion.
    bool vignetting = false; ///< Light falloff towards the corners.
    bool lateralCa = false;  ///< Lateral chromatic aberration, the colour fringing at the edges.

    /// @brief Checks whether any correction is wanted.
    [[nodiscard]] constexpr bool any() const noexcept {
        return distortion || vignetting || lateralCa;
    }

    friend bool operator==(const LensKinds&, const LensKinds&) = default;
};

/// @brief Where one correction's numbers came from, and what they assumed.
///
/// What a status line and `info` show beside a correction: the lens and the conditions the
/// profile was resolved for. A value the file did not record is marked, since a profile resolved
/// at a guessed aperture or distance is a worse profile.
struct LensProvenance {
    LensIdentity lens;            ///< The lens whose profile it is.
    double cropFactor = 1.0;      ///< Crop factor the profile was applied with.
    double focalLength = 0.0;     ///< Focal length resolved at, in millimetres.
    bool focalLengthRead = false; ///< Whether the file recorded it.
    double aperture = 0.0;        ///< Aperture resolved at, as an f-number.
    bool apertureRead = false;    ///< Whether the file recorded it.
    double distance = 0.0;        ///< Focus distance resolved at, in metres.
    bool distanceRead = false;    ///< Whether the file recorded it.
    std::string database;         ///< Stamp of the database it was resolved from.

    friend bool operator==(const LensProvenance&, const LensProvenance&) = default;
};

/// @brief Number of nodes in each radial table.
///
/// Chosen by measurement (tests/test_LensCorrection.cpp): with this many, linear interpolation of
/// lensfun's models stays under 0.05 sensor pixels at every corner and edge of a 24 MP frame for
/// the strongest profile of the test database.
inline constexpr std::size_t lensTableNodes = 129;

/// @brief A lens's corrections as radial tables, in coordinates that do not depend on resolution.
///
/// Positions are fractions of the frame (0 to 1 across and down), the radius is the distance from
/// the optical centre to the farthest corner, with the frame's aspect taken into account, and each
/// table holds ::arraw::lensTableNodes values evenly spaced from radius 0 to 1 (ADR 050, section
/// 3). So the same tables serve a pyramid level, a half-size decode and the full decode. An empty
/// table means that correction is absent. The tables are:
///
/// - `distortion`: at the radius of a point in the corrected image (before the fill scale), the
///   factor to its radius in the sensor image;
/// - `redScale`, `blueScale`: at the radius of a point in the sensor image, the factor by which
///   the red or blue channel's own position is scaled about the centre against green's;
/// - `vignetting`: at the radius of a point in the sensor image, the gain that restores its light.
struct LensCorrection {
    std::vector<float> distortion; ///< Radial scale of distortion, or empty.
    std::vector<float> redScale;   ///< Radial scale of red against green, or empty.
    std::vector<float> blueScale;  ///< Radial scale of blue against green, or empty.
    std::vector<float> vignetting; ///< Radial gain, or empty.

    double centerU =
        0.5; ///< Optical centre across, as a fraction of the width (lensfun's calibrated one).
    double centerV = 0.5; ///< Optical centre down, as a fraction of the height.
    double aspect = 1.0;  ///< Width over height of the frames the tables were sampled for.
    double fill = 1.0;    ///< Zoom that fills the corrected frame; see ::arraw::withFillScale.

    std::optional<LensProvenance> distortionFrom; ///< Where the distortion came from, if present.
    std::optional<LensProvenance> lateralCaFrom;  ///< Where the lateral CA came from, if present.
    std::optional<LensProvenance> vignettingFrom; ///< Where the vignetting came from, if present.

    /// @brief Checks whether red and blue have tables.
    [[nodiscard]] bool hasLateralCa() const noexcept {
        return !redScale.empty() && !blueScale.empty();
    }

    /// @brief Checks whether any correction is present, and so whether the Lens stage has work.
    [[nodiscard]] bool active() const noexcept {
        return !distortion.empty() || hasLateralCa() || !vignetting.empty();
    }

    friend bool operator==(const LensCorrection&, const LensCorrection&) = default;
};

/// @brief Reads a radial table at a radius, interpolating between its nodes.
/// @param table Table of ::arraw::lensTableNodes values; non-empty.
/// @param radius Radius, 0 at the centre and 1 at the farthest corner. Beyond 1 the last value
/// holds.
/// @return The table's value there.
[[nodiscard]] double tableValue(const std::vector<float>& table, double radius) noexcept;

/// @brief Gives the zoom that keeps every sample of the corrected frame inside the sensor image.
///
/// The smallest zoom of at least 1 at which every point of the corrected frame's boundary maps
/// to a sensor position inside the frame (ADR 050, section 3). One only when the distortion is
/// absent or only shrinks. Depends on the distortion table, the centre and the aspect alone, so
/// every resolution agrees on it.
/// @param correction Tables and centre.
/// @param aspect Width over height of the frame.
/// @return The zoom, at least 1.
[[nodiscard]] double fillScaleOf(const LensCorrection& correction, double aspect);

/// @brief Sets a correction's fill scale from its distortion table.
/// @param correction Correction to complete.
/// @return It, with ::arraw::LensCorrection::fill set to ::arraw::fillScaleOf at its aspect.
[[nodiscard]] LensCorrection withFillScale(LensCorrection correction);

/// @brief Gives the normalised radius of a sensor-frame point: its distance from the optical centre
/// over the centre's distance to the farthest corner, the aspect taken into account.
///
/// What the sensor-radius tables (red, blue, vignetting) are read at.
/// @param correction Centre and aspect.
/// @param point Point in the sensor frame.
/// @return The radius, 0 at the centre and 1 at the farthest corner.
[[nodiscard]] double radiusOf(const LensCorrection& correction, SensorPoint point) noexcept;

/// @brief Gives the sensor-frame point under a position of the corrected frame (ADR 009).
///
/// The corrected frame is the lens stage's output: the sensor image zoomed by the fill scale and
/// its distortion taken out. With no distortion it is the same point.
/// @param correction Resolved correction, its fill scale set.
/// @param position Position in the corrected frame; it may lie off the frame.
/// @return The sensor-frame point, narrowed to float.
[[nodiscard]] SensorPoint sensorFrom(const LensCorrection& correction,
                                     CorrectedPosition position) noexcept;

/// @brief Gives the position of the corrected frame that shows a sensor-frame point.
///
/// The inverse of ::arraw::sensorFrom, by Newton's method on the radial table. It fails where the
/// distortion cannot be undone: a table that is not increasing, or a point so far out that no
/// radius within twice the frame maps to it.
/// @param correction Resolved correction, its fill scale set.
/// @param point Point in the sensor frame.
/// @return The corrected position, or nothing when there is none.
[[nodiscard]] std::optional<CorrectedPosition> correctedFrom(const LensCorrection& correction,
                                                             SensorPoint point) noexcept;

} // namespace arraw
