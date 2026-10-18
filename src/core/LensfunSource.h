#pragma once

#include <ImageBuffer.h>
#include <LensCorrection.h>
#include <LensDatabase.h>
#include <LensFacts.h>
#include <LensMatch.h>

#include <array>
#include <memory>
#include <optional>

// lensfun's own header is included by LensfunSource.cpp alone; nothing here names a lensfun type
// (ADR 050, section 5).

namespace arraw {

/// @brief Aperture assumed when a file records none, as an f-number.
///
/// Vignetting depends on it; the provenance marks the assumption.
inline constexpr double assumedAperture = 8.0;

/// @brief Focus distance assumed when a file records none, in metres: lensfun's "infinity".
inline constexpr double assumedDistance = 1000.0;

/// @brief Resolves a matched lens's profile into radial tables, by sampling lensfun's own models.
///
/// For each wanted correction, lensfun's `lfModifier` is set up for the photograph (the lens's
/// profile interpolated to the focal length, aperture and distance, rescaled to the crop factor
/// and the size) and asked, along the ray from lensfun's calibrated centre to the farthest corner,
/// for its answer at ::arraw::lensTableNodes radii. Distortion is read as the sensor position of a
/// corrected one, lateral CA and vignetting at the sensor position, as lensfun composes them
/// (distortion first, then CA at the distorted position, vignetting at the sensor pixel). lensfun's
/// models are not reimplemented.
///
/// A correction the lens has no calibration for is left absent. The fill scale is set.
/// @param database Database the match was found in.
/// @param match The lens, the crop factor and the focal length.
/// @param facts What the file recorded of the aperture and distance.
/// @param size Size of the photograph, which fixes the aspect lensfun's models are sampled at.
/// @param which Corrections wanted.
/// @param nodes Nodes per table, at least 4; ::arraw::lensTableNodes unless a test measures others.
/// @return The correction, or nothing when the database does not list the lens or has no lensfun.
[[nodiscard]] std::optional<LensCorrection>
sampleLensfun(const LensDatabase& database, const LensMatch& match, const LensFacts& facts,
              ImageSize size, LensKinds which, std::size_t nodes = lensTableNodes);

namespace detail {

/// @brief lensfun's per-pixel answers for a matched lens, to hold the tables against.
///
/// Exists for the accuracy test of ::arraw::sampleLensfun, which compares the tables with the
/// model they sample; nothing else uses it.
class LensfunReference {
public:
    /// @brief Prepares lensfun's modifier as ::arraw::sampleLensfun does, for all three
    /// corrections.
    LensfunReference(const LensDatabase& database, const LensMatch& match, const LensFacts& facts,
                     ImageSize size);
    ~LensfunReference();
    LensfunReference(const LensfunReference&) = delete;
    LensfunReference& operator=(const LensfunReference&) = delete;

    /// @brief Checks whether the lens was found.
    [[nodiscard]] bool valid() const noexcept;

    /// @brief Gives the optical centre lensfun uses, in pixel-centre index coordinates.
    [[nodiscard]] std::array<double, 2> centre() const noexcept;

    /// @brief Gives where lensfun reads red, green and blue for an output pixel.
    /// @param x Column of the output pixel, in index coordinates (a pixel's centre is whole).
    /// @param y Row of the output pixel.
    /// @return Red, green and blue positions, each as x then y, in the same coordinates.
    [[nodiscard]] std::array<std::array<double, 2>, 3> sourcePositions(double x, double y) const;

    /// @brief Gives the gain lensfun applies to the sensor pixel at a position.
    [[nodiscard]] double gainAt(double x, double y) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detail
} // namespace arraw
