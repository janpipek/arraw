#include "ExifInfo.h"

#include "Exiv2Support.h"

#include <exiv2/exiv2.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace arraw;

namespace {

/// @brief Finds a tag, by its exiv2 key.
/// @return The datum, or null when the tag is absent or holds no value.
const Exiv2::Exifdatum* find(const Exiv2::ExifData& data, const char* key) {
    const auto found = data.findKey(Exiv2::ExifKey(key));
    return found == data.end() || found->count() == 0 ? nullptr : &*found;
}

/// @brief Reads an ASCII tag as text without the padding cameras add.
std::optional<std::string> text(const Exiv2::ExifData& data, const char* key) {
    const auto* datum = find(data, key);
    if (!datum) {
        return std::nullopt;
    }
    std::string value = datum->toString();
    constexpr auto padding = std::string_view(" \t\r\n\0", 5);
    const auto last = value.find_last_not_of(padding);
    if (last == std::string::npos) {
        return std::nullopt;
    }
    value.erase(last + 1);
    value.erase(0, value.find_first_not_of(padding));
    return value;
}

/// @brief Reads an unsigned fraction.
///
/// A signed fraction is accepted when it is not negative, as some writers use
/// the wrong type for a tag that cannot be.
std::optional<URational> urational(const Exiv2::ExifData& data, const char* key) {
    const auto* datum = find(data, key);
    // A malformed file can declare a tag with no value at all.
    if (!datum || datum->count() == 0) {
        return std::nullopt;
    }
    if (datum->typeId() == Exiv2::unsignedRational) {
        const auto rational = datum->toRational();
        // Exiv2 narrows an unsigned fraction to signed; a negative numerator is
        // an unsigned one above 2^31, which it does not distinguish.
        if (rational.first >= 0 && rational.second >= 0) {
            return URational{static_cast<std::uint32_t>(rational.first),
                             static_cast<std::uint32_t>(rational.second)};
        }
        const auto& typed = static_cast<const Exiv2::URationalValue&>(datum->value());
        return URational{typed.value_[0].first, typed.value_[0].second};
    }
    if (datum->typeId() == Exiv2::signedRational) {
        const auto rational = datum->toRational();
        if (rational.first >= 0 && rational.second >= 0) {
            return URational{static_cast<std::uint32_t>(rational.first),
                             static_cast<std::uint32_t>(rational.second)};
        }
    }
    return std::nullopt;
}

/// @brief Reads a signed fraction.
std::optional<SRational> srational(const Exiv2::ExifData& data, const char* key) {
    const auto* datum = find(data, key);
    if (!datum ||
        (datum->typeId() != Exiv2::signedRational && datum->typeId() != Exiv2::unsignedRational)) {
        return std::nullopt;
    }
    const auto rational = datum->toRational();
    return SRational{rational.first, rational.second};
}

/// @brief Reads an integer tag that fits a type.
template <class T> std::optional<T> integer(const Exiv2::ExifData& data, const char* key) {
    const auto* datum = find(data, key);
    if (!datum) {
        return std::nullopt;
    }
    switch (datum->typeId()) {
    case Exiv2::unsignedByte:
    case Exiv2::unsignedShort:
    case Exiv2::unsignedLong:
    case Exiv2::signedShort:
    case Exiv2::signedLong: {
        const std::int64_t value = datum->toInt64();
        if (value >= std::numeric_limits<T>::min() && value <= std::numeric_limits<T>::max()) {
            return static_cast<T>(value);
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

/// @brief Reads degrees, minutes and seconds (or fewer) as decimal degrees.
std::optional<double> degrees(const Exiv2::ExifData& data, const char* key) {
    const auto* datum = find(data, key);
    if (!datum || datum->typeId() != Exiv2::unsignedRational || datum->count() > 3) {
        return std::nullopt;
    }
    double result = 0.0;
    double divisor = 1.0;
    for (std::size_t i = 0; i < datum->count(); ++i) {
        const auto part = datum->toRational(i);
        if (part.second == 0 || part.first < 0) {
            return std::nullopt;
        }
        result += static_cast<double>(part.first) / part.second / divisor;
        divisor *= 60.0;
    }
    return result;
}

/// @brief Reads a coordinate and its hemisphere into a signed value.
/// @param positive Reference letter of the positive hemisphere.
/// @param negative Reference letter of the negative one.
/// @param limit Largest magnitude the coordinate can have.
std::optional<double> coordinate(const Exiv2::ExifData& data, const char* valueKey,
                                 const char* referenceKey, char positive, char negative,
                                 double limit) {
    const auto magnitude = degrees(data, valueKey);
    const auto reference = text(data, referenceKey);
    if (!magnitude || !reference || *magnitude > limit) {
        return std::nullopt;
    }
    const char letter =
        static_cast<char>(std::toupper(static_cast<unsigned char>((*reference)[0])));
    if (letter == positive) {
        return *magnitude;
    }
    if (letter == negative) {
        return -*magnitude;
    }
    return std::nullopt;
}

/// @brief Reads the position tags, requiring a latitude and a longitude.
std::optional<GpsPosition> position(const Exiv2::ExifData& data) {
    const auto latitude =
        coordinate(data, "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef", 'N', 'S', 90.0);
    const auto longitude = coordinate(data, "Exif.GPSInfo.GPSLongitude",
                                      "Exif.GPSInfo.GPSLongitudeRef", 'E', 'W', 180.0);
    if (!latitude || !longitude) {
        return std::nullopt;
    }
    GpsPosition result{*latitude, *longitude, std::nullopt};
    if (const auto altitude = urational(data, "Exif.GPSInfo.GPSAltitude");
        altitude && altitude->denominator != 0) {
        // The reference is one byte: 0 above sea level, 1 below.
        const bool below = integer<int>(data, "Exif.GPSInfo.GPSAltitudeRef").value_or(0) == 1;
        result.altitude = below ? -altitude->value() : altitude->value();
    }
    return result;
}

/// @brief Reads the lens name, from the standard tag or else the maker's own.
std::optional<std::string> lens(const Exiv2::ExifData& data) {
    if (auto standard = text(data, "Exif.Photo.LensModel")) {
        return standard;
    }
    try {
        const auto found = Exiv2::lensName(data);
        if (found != data.end() && found->count() > 0) {
            std::string name = found->print(&data);
            const auto last = name.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) {
                name.erase(last + 1);
                return name;
            }
        }
    } catch (const Exiv2::Error&) {
        // A maker note too damaged to name its lens leaves the lens unknown.
    }
    return std::nullopt;
}

/// @brief Collects the fields from a file's EXIF.
ExifInfo collect(const Exiv2::ExifData& data) {
    ExifInfo info;
    info.make = text(data, "Exif.Image.Make");
    info.model = text(data, "Exif.Image.Model");
    info.lensModel = lens(data);
    info.dateTimeOriginal = text(data, "Exif.Photo.DateTimeOriginal");
    info.offsetTimeOriginal = text(data, "Exif.Photo.OffsetTimeOriginal");
    info.exposureTime = urational(data, "Exif.Photo.ExposureTime");
    info.fNumber = urational(data, "Exif.Photo.FNumber");
    info.photographicSensitivity = integer<std::uint32_t>(data, "Exif.Photo.ISOSpeedRatings");
    info.focalLength = urational(data, "Exif.Photo.FocalLength");
    info.focalLengthIn35mmFilm = integer<std::uint16_t>(data, "Exif.Photo.FocalLengthIn35mmFilm");
    info.exposureBiasValue = srational(data, "Exif.Photo.ExposureBiasValue");
    info.flash = integer<std::uint16_t>(data, "Exif.Photo.Flash");
    info.gps = position(data);
    info.artist = text(data, "Exif.Image.Artist");
    info.copyright = text(data, "Exif.Image.Copyright");
    return info;
}

/// @brief Tells the log a file gave no EXIF.
void reportUnreadable(DiagnosticLog& log, const std::filesystem::path& path, Severity severity,
                      std::string reason) {
    log.record({.notice = Notice::ExifUnreadable,
                .severity = severity,
                .subject = path,
                .values = {std::move(reason)}});
}

} // namespace

ExifInfo arraw::readExif(const std::filesystem::path& path, DiagnosticLog& log) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        const std::error_code reason =
            error ? error : std::make_error_code(std::errc::no_such_file_or_directory);
        throw std::runtime_error(path.string() + ": " + reason.message());
    }
    exiv2support::prepare();
    try {
        const auto image = Exiv2::ImageFactory::open(exiv2support::path(path), false);
        image->readMetadata();
        const ExifInfo info = collect(image->exifData());
        if (info == ExifInfo{}) {
            reportUnreadable(log, path, Severity::Info, "the file records no EXIF");
        }
        return info;
    } catch (const std::exception& problem) {
        reportUnreadable(log, path, Severity::Warning,
                         std::string("its EXIF could not be read (") + problem.what() + ")");
    }
    return {};
}
