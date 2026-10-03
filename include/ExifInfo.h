#pragma once

#include <Diagnostics.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

namespace arraw {

/// @brief Unsigned fraction, as the EXIF RATIONAL type stores it.
///
/// Kept as numerator and denominator rather than a double, because a camera
/// records an exposure of 1/250 s and a photographer reads it so.
struct URational {
    /// @brief Dividend.
    std::uint32_t numerator = 0;

    /// @brief Divisor; zero when the file records an undefined fraction.
    std::uint32_t denominator = 1;

    /// @brief Divides the numerator by the denominator.
    /// @return The quotient, or NaN when the denominator is zero.
    [[nodiscard]] double value() const noexcept {
        return denominator == 0 ? std::numeric_limits<double>::quiet_NaN()
                                : static_cast<double>(numerator) / denominator;
    }

    friend bool operator==(const URational&, const URational&) = default;
};

/// @brief Signed fraction, as the EXIF SRATIONAL type stores it.
struct SRational {
    /// @brief Dividend.
    std::int32_t numerator = 0;

    /// @brief Divisor; zero when the file records an undefined fraction.
    std::int32_t denominator = 1;

    /// @brief Divides the numerator by the denominator.
    /// @return The quotient, or NaN when the denominator is zero.
    [[nodiscard]] double value() const noexcept {
        return denominator == 0 ? std::numeric_limits<double>::quiet_NaN()
                                : static_cast<double>(numerator) / denominator;
    }

    friend bool operator==(const SRational&, const SRational&) = default;
};

/// @brief Where a photograph was taken, as signed decimal numbers.
struct GpsPosition {
    /// @brief Degrees north of the equator, negative in the south (GPSLatitude, GPSLatitudeRef).
    double latitude = 0.0;

    /// @brief Degrees east of Greenwich, negative in the west (GPSLongitude, GPSLongitudeRef).
    double longitude = 0.0;

    /// @brief Metres above sea level, negative below it (GPSAltitude, GPSAltitudeRef).
    std::optional<double> altitude;

    friend bool operator==(const GpsPosition&, const GpsPosition&) = default;
};

/// @brief What a photograph's EXIF records about its capture.
///
/// Every field is optional, because a camera, a screenshot and an edited
/// export each record a different subset. Named after the EXIF tag it comes
/// from (ADR 028). Separate from ::arraw::ImageMetadata, which is what a
/// decode will produce and is needed to plan a render, while this is only
/// ever shown or copied.
struct ExifInfo {
    /// @brief Camera maker (Make).
    std::optional<std::string> make;

    /// @brief Camera model (Model).
    std::optional<std::string> model;

    /// @brief Lens name (LensModel, else the maker's own lens name).
    std::optional<std::string> lensModel;

    /// @brief Time the shutter was released, as EXIF writes it, "YYYY:MM:DD HH:MM:SS"
    /// (DateTimeOriginal).
    std::optional<std::string> dateTimeOriginal;

    /// @brief Offset of the capture time from UTC, as "+HH:MM" (OffsetTimeOriginal).
    std::optional<std::string> offsetTimeOriginal;

    /// @brief Shutter time in seconds (ExposureTime).
    std::optional<URational> exposureTime;

    /// @brief Aperture as an f-number (FNumber).
    std::optional<URational> fNumber;

    /// @brief Sensitivity as an ISO speed (ISOSpeedRatings, PhotographicSensitivity).
    std::optional<std::uint32_t> photographicSensitivity;

    /// @brief Focal length in millimetres (FocalLength).
    std::optional<URational> focalLength;

    /// @brief Focal length in millimetres of the 35 mm film frame that would
    /// give the same view (FocalLengthIn35mmFilm).
    std::optional<std::uint16_t> focalLengthIn35mmFilm;

    /// @brief Exposure compensation in EV (ExposureBiasValue).
    std::optional<SRational> exposureBiasValue;

    /// @brief Whether and how the flash fired, as EXIF's bit field (Flash).
    std::optional<std::uint16_t> flash;

    /// @brief Place of the capture (the GPSInfo latitude, longitude and altitude tags).
    std::optional<GpsPosition> gps;

    /// @brief Photographer (Artist).
    std::optional<std::string> artist;

    /// @brief Copyright notice (Copyright).
    std::optional<std::string> copyright;

    friend bool operator==(const ExifInfo&, const ExifInfo&) = default;
};

/// @brief Reads what a file records about its capture.
///
/// Reads any format the EXIF library understands: RAW (including CR3), JPEG,
/// TIFF and PNG. A file it cannot read, or one that records no EXIF, is not
/// an error: the result is empty and the log hears a ::arraw::Notice::ExifUnreadable.
/// May be called from several threads at once.
///
/// @param path File to read.
/// @param log Where to report a file that has no readable EXIF.
/// @return The fields the file records, each absent when it does not.
/// @throws std::runtime_error if the file does not exist.
[[nodiscard]] ExifInfo readExif(const std::filesystem::path& path,
                                DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
