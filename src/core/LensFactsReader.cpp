#include "LensFactsReader.h"

#include "Exiv2Support.h"

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

using namespace arraw;

namespace {

/// @brief Finds a tag by its exiv2 key; null when absent or empty.
const Exiv2::Exifdatum* find(const Exiv2::ExifData& data, const char* key) {
    const auto found = data.findKey(Exiv2::ExifKey(key));
    return found == data.end() || found->count() == 0 ? nullptr : &*found;
}

/// @brief Reads a text tag, trimmed.
std::optional<std::string> text(const Exiv2::ExifData& data, const char* key) {
    const auto* datum = find(data, key);
    if (datum == nullptr) {
        return std::nullopt;
    }
    return datum->toString();
}

/// @brief Reads a number tag at an index, for any numeric type.
std::optional<double> number(const Exiv2::ExifData& data, const char* key, std::size_t index = 0) {
    const auto* datum = find(data, key);
    if (datum == nullptr || index >= datum->count()) {
        return std::nullopt;
    }
    switch (datum->typeId()) {
    case Exiv2::unsignedRational:
    case Exiv2::signedRational: {
        const auto fraction = datum->toRational(index);
        if (fraction.second == 0) {
            return std::nullopt;
        }
        return static_cast<double>(fraction.first) / fraction.second;
    }
    case Exiv2::unsignedByte:
    case Exiv2::unsignedShort:
    case Exiv2::unsignedLong:
    case Exiv2::signedShort:
    case Exiv2::signedLong:
        return static_cast<double>(datum->toInt64(index));
    default:
        return std::nullopt;
    }
}

/// @brief Reads a positive number, or nothing.
std::optional<double> positive(std::optional<double> value) {
    return value && *value > 0.0 && std::isfinite(*value) ? value : std::nullopt;
}

/// @brief Reads Canon's focal range, which it writes in units of its own.
///
/// `Lens` holds the long and short focal lengths and the units per millimetre; older tables
/// carry them as separate tags.
void canonFocalRange(const Exiv2::ExifData& data, LensFacts& facts) {
    std::optional<double> longest;
    std::optional<double> shortest;
    std::optional<double> units;
    if (const auto* lens = find(data, "Exif.CanonCs.Lens"); lens != nullptr && lens->count() >= 3) {
        longest = number(data, "Exif.CanonCs.Lens", 0);
        shortest = number(data, "Exif.CanonCs.Lens", 1);
        units = number(data, "Exif.CanonCs.Lens", 2);
    } else {
        longest = number(data, "Exif.CanonCs.Lens");
        shortest = number(data, "Exif.CanonCs.ShortFocal");
        units = number(data, "Exif.CanonCs.FocalUnits");
    }
    if (!positive(units) || !positive(longest) || !positive(shortest)) {
        return;
    }
    facts.minFocal = *shortest / *units;
    facts.maxFocal = *longest / *units;
}

/// @brief Reads where the lens was focused, in metres.
///
/// The standard tag where a camera writes it; else Canon's, which keeps the distance in
/// centimetres between two bounds, 0 for unknown and 65535 for beyond measuring.
std::optional<double> focusDistance(const Exiv2::ExifData& data) {
    // A camera writes 0 or all ones where it cannot say.
    if (const auto standard = positive(number(data, "Exif.Photo.SubjectDistance"));
        standard && *standard < 1.0e6) {
        return standard;
    }
    const auto upper = positive(number(data, "Exif.CanonFi.FocusDistanceUpper"));
    const auto lower = positive(number(data, "Exif.CanonFi.FocusDistanceLower"));
    constexpr double unmeasurable = 65535.0;
    if (upper && lower && *upper < unmeasurable && *lower < unmeasurable) {
        return (*upper + *lower) / 200.0;
    }
    return std::nullopt;
}

/// @brief Reads the name exiv2 gives the lens, from the maker note.
std::optional<std::string> makerNoteName(const Exiv2::ExifData& data) {
    try {
        const auto found = Exiv2::lensName(data);
        if (found != data.end() && found->count() > 0) {
            return found->print(&data);
        }
    } catch (const Exiv2::Error&) {
        // A maker note too damaged to name its lens names none.
    }
    return std::nullopt;
}

/// @brief Reduces a name to its letters and digits, lower-case, for comparing spellings.
std::string compact(std::string_view name) {
    std::string result;
    for (const char character : name) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    return result;
}

} // namespace

void lensfacts::addName(std::vector<std::string>& names, std::string name) {
    constexpr std::string_view padding = " \t\r\n";
    const auto first = name.find_first_not_of(padding);
    if (first == std::string::npos) {
        return;
    }
    name = name.substr(first, name.find_last_not_of(padding) - first + 1);
    const std::string key = compact(name);
    if (key.empty() || std::ranges::any_of(names, [&](const std::string& existing) {
            return compact(existing) == key;
        })) {
        return;
    }
    names.push_back(std::move(name));
}

void lensfacts::complete(const std::filesystem::path& path, LensFacts& facts) {
    exiv2support::prepare();
    try {
        const auto image = Exiv2::ImageFactory::open(exiv2support::path(path), false);
        image->readMetadata();
        const Exiv2::ExifData& data = image->exifData();

        if (const auto maker = text(data, "Exif.Image.Make")) {
            facts.cameraMaker = *maker;
        }
        if (const auto model = text(data, "Exif.Image.Model")) {
            facts.cameraModel = *model;
        }
        // The EXIF's names lead; what the decoder found follows them.
        std::vector<std::string> names;
        for (const char* key : {"Exif.Photo.LensModel", "Exif.Canon.LensModel"}) {
            if (const auto name = text(data, key)) {
                lensfacts::addName(names, *name);
            }
        }
        if (const auto name = makerNoteName(data)) {
            lensfacts::addName(names, *name);
        }
        for (std::string& name : facts.lensNames) {
            lensfacts::addName(names, std::move(name));
        }
        facts.lensNames = std::move(names);

        canonFocalRange(data, facts);
        if (const auto focal = positive(number(data, "Exif.Photo.FocalLength"))) {
            facts.focalLength = focal;
        }
        if (const auto aperture = positive(number(data, "Exif.Photo.FNumber"))) {
            facts.aperture = aperture;
        }
        if (const auto film = positive(number(data, "Exif.Photo.FocalLengthIn35mmFilm"))) {
            facts.focalLength35 = film;
        }
        if (const auto distance = focusDistance(data)) {
            facts.focusDistance = distance;
        }
    } catch (const std::exception&) {
        // Facts help to match a lens and never decide whether a file opens.
    }
}
