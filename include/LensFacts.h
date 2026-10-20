#pragma once

#include <optional>
#include <string>
#include <vector>

namespace arraw {

/// @brief What a RAW file says about the camera and lens it was shot with, for finding a lens
/// profile.
///
/// Read once with the rest of ::arraw::ImageMetadata (ADR 050, section 5) and kept apart from
/// ::arraw::ExifInfo, which is only shown (ADR 028). Every field is a claim of the file, which a
/// camera may leave out or get wrong; matching a lens never trusts one string alone.
struct LensFacts {
    /// @brief Camera maker, as the EXIF records it, else as the RAW decoder names it.
    std::string cameraMaker;

    /// @brief Camera model, as the EXIF records it, else as the RAW decoder names it.
    std::string cameraModel;

    /// @brief Names the file gives the lens, best first and without repeats.
    ///
    /// The EXIF's `LensModel`, then the maker note's own lens name where it differs, then the
    /// name a lens ID stands for. A third-party lens often reports a Canon ID, so a later name
    /// can name the wrong lens; the focal range and aperture below are what catch that.
    std::vector<std::string> lensNames;

    /// @brief Shortest focal length of the lens, in millimetres, when the maker note says.
    std::optional<double> minFocal;

    /// @brief Longest focal length of the lens, in millimetres, when the maker note says.
    std::optional<double> maxFocal;

    /// @brief Widest aperture of the lens at the focal length used, as an f-number, when known.
    std::optional<double> maxAperture;

    /// @brief Focal length used, in millimetres.
    std::optional<double> focalLength;

    /// @brief Aperture used, as an f-number.
    std::optional<double> aperture;

    /// @brief Distance focused at, in metres, when the file records one.
    std::optional<double> focusDistance;

    /// @brief Focal length used in 35 mm film terms, in millimetres.
    std::optional<double> focalLength35;

    friend bool operator==(const LensFacts&, const LensFacts&) = default;
};

} // namespace arraw
