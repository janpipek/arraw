#include "MetadataEmbedding.h"

#include "Exiv2Support.h"

#include <Diagnostics.h>
#include <Sidecar.h>

#include <exiv2/exiv2.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using namespace arraw;

namespace {

/// @brief A tag to copy, and the XMP property that says the same when the source has only XMP.
struct CaptureTag {
    /// @brief exiv2 key of the EXIF tag.
    const char* exif;

    /// @brief exiv2 key of the equivalent XMP property, or null.
    const char* xmp;
};

// Not here by design: MakerNote, serial numbers (Exif.Photo.BodySerialNumber,
// LensSerialNumber, CameraOwnerName) and everything that describes the source's pixels.
constexpr auto captureTags = std::to_array<CaptureTag>({
    {"Exif.Image.Make", "Xmp.tiff.Make"},
    {"Exif.Image.Model", "Xmp.tiff.Model"},
    {"Exif.Image.DateTime", nullptr},
    {"Exif.Photo.ExposureTime", "Xmp.exif.ExposureTime"},
    {"Exif.Photo.FNumber", "Xmp.exif.FNumber"},
    {"Exif.Photo.ExposureProgram", "Xmp.exif.ExposureProgram"},
    {"Exif.Photo.ISOSpeedRatings", "Xmp.exif.ISOSpeedRatings"},
    {"Exif.Photo.SensitivityType", nullptr},
    {"Exif.Photo.DateTimeOriginal", "Xmp.exif.DateTimeOriginal"},
    {"Exif.Photo.DateTimeDigitized", "Xmp.exif.DateTimeDigitized"},
    {"Exif.Photo.OffsetTime", nullptr},
    {"Exif.Photo.OffsetTimeOriginal", nullptr},
    {"Exif.Photo.OffsetTimeDigitized", nullptr},
    {"Exif.Photo.SubSecTime", nullptr},
    {"Exif.Photo.SubSecTimeOriginal", nullptr},
    {"Exif.Photo.SubSecTimeDigitized", nullptr},
    {"Exif.Photo.ExposureBiasValue", "Xmp.exif.ExposureBiasValue"},
    {"Exif.Photo.MaxApertureValue", "Xmp.exif.MaxApertureValue"},
    {"Exif.Photo.MeteringMode", "Xmp.exif.MeteringMode"},
    {"Exif.Photo.LightSource", "Xmp.exif.LightSource"},
    {"Exif.Photo.Flash", "Xmp.exif.Flash"},
    {"Exif.Photo.FocalLength", "Xmp.exif.FocalLength"},
    {"Exif.Photo.FocalLengthIn35mmFilm", "Xmp.exif.FocalLengthIn35mmFilm"},
    {"Exif.Photo.LensMake", "Xmp.exifEX.LensMake"},
    {"Exif.Photo.LensModel", "Xmp.exifEX.LensModel"},
    {"Exif.Photo.LensSpecification", "Xmp.exifEX.LensSpecification"},
    {"Exif.Photo.WhiteBalance", "Xmp.exif.WhiteBalance"},
    {"Exif.Photo.ExposureMode", "Xmp.exif.ExposureMode"},
    {"Exif.Photo.SceneCaptureType", "Xmp.exif.SceneCaptureType"},
});

/// XMP-only lens names, which have no EXIF twin worth a table row.
constexpr auto captureXmpOnly =
    std::to_array<std::string_view>({"Xmp.aux.Lens", "Xmp.aux.LensInfo"});

/// The XMP properties the descriptive group carries from the source and the sidecar.
constexpr auto descriptiveXmp = std::to_array<std::string_view>(
    {"Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.subject", "Xmp.dc.creator", "Xmp.dc.rights"});

/// The EXIF tags the descriptive group carries.
constexpr auto descriptiveExif =
    std::to_array<std::string_view>({"Exif.Image.Artist", "Exif.Image.Copyright"});

/// @brief Whether a string is among a fixed list.
template <std::size_t N>
bool isOneOf(const std::string& key, const std::array<std::string_view, N>& keys) {
    for (const auto candidate : keys) {
        if (key == candidate) {
            return true;
        }
    }
    return false;
}

/// @brief Where the warnings of one embedding go.
struct Warnings {
    /// @brief Log receiving them.
    DiagnosticLog& log;

    /// @brief Photograph the metadata comes from.
    const std::filesystem::path& source;

    /// @brief Records that something was left out.
    /// @param reason What was left out and why.
    void leftOut(const std::string& reason) const {
        log.record({.notice = Notice::MetadataNotCarried,
                    .severity = Severity::Warning,
                    .subject = source,
                    .values = {reason}});
    }
};

/// @brief Copies one EXIF tag, if the source has it.
/// @return Whether it was copied; a tag that cannot be is left out with a warning.
bool copyExif(const Exiv2::ExifData& from, Exiv2::ExifData& to, const std::string& key,
              const Warnings& warnings) {
    try {
        const auto found = from.findKey(Exiv2::ExifKey(key));
        if (found == from.end() || found->count() == 0) {
            return false;
        }
        to[key] = found->value();
        return true;
    } catch (const std::exception& problem) {
        warnings.leftOut("the tag " + key + " cannot be copied (" + problem.what() + ")");
        return false;
    }
}

/// @brief Replaces an XMP property with another file's value for it.
/// @return Whether it was copied; a property that cannot be is left out with a warning.
bool copyXmp(const Exiv2::Xmpdatum& datum, Exiv2::XmpData& to, const Warnings& warnings) {
    try {
        const auto existing = to.findKey(Exiv2::XmpKey(datum.key()));
        if (existing != to.end()) {
            to.erase(existing);
        }
        to.add(datum);
        return true;
    } catch (const std::exception& problem) {
        warnings.leftOut("the property " + datum.key() + " cannot be copied (" + problem.what() +
                         ")");
        return false;
    }
}

/// @brief Decodes the XMP of a sidecar.
/// @throws std::runtime_error if the file cannot be read or is not XMP.
Exiv2::XmpData readSidecarXmp(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot read the sidecar " + path.string());
    }
    const std::string packet{std::istreambuf_iterator<char>(stream), {}};
    Exiv2::XmpData data;
    if (Exiv2::XmpParser::decode(data, packet) != 0) {
        throw std::runtime_error("the sidecar " + path.string() + " is not XMP");
    }
    return data;
}

/// @brief Metadata read from the source; both empty when it could not be read.
struct SourceData {
    /// The source's EXIF.
    Exiv2::ExifData exif;
    /// The source's XMP.
    Exiv2::XmpData xmp;
};

/// @brief Adds the capture group of the source to the output.
/// @return Whether anything was copied.
bool addCapture(const SourceData& source, Exiv2::ExifData& exif, Exiv2::XmpData& xmp,
                const Warnings& warnings) {
    bool any = false;
    for (const auto& tag : captureTags) {
        if (copyExif(source.exif, exif, tag.exif, warnings)) {
            any = true;
        } else if (tag.xmp != nullptr) {
            const auto found = source.xmp.findKey(Exiv2::XmpKey(tag.xmp));
            if (found != source.xmp.end()) {
                any = copyXmp(*found, xmp, warnings) || any;
            }
        }
    }
    for (const auto& datum : source.xmp) {
        if (isOneOf(datum.key(), captureXmpOnly)) {
            any = copyXmp(datum, xmp, warnings) || any;
        }
    }
    return any;
}

/// @brief Adds the GPS group of the source to the output.
bool addLocation(const SourceData& source, Exiv2::ExifData& exif, Exiv2::XmpData& xmp,
                 const Warnings& warnings) {
    bool any = false;
    for (const auto& datum : source.exif) {
        if (datum.groupName() == "GPSInfo" && datum.count() > 0) {
            any = copyExif(source.exif, exif, datum.key(), warnings) || any;
        }
    }
    for (const auto& datum : source.xmp) {
        if (datum.groupName() == "exif" && datum.tagName().starts_with("GPS")) {
            any = copyXmp(datum, xmp, warnings) || any;
        }
    }
    return any;
}

/// @brief Adds marks, and the descriptive fields of the source and its sidecar.
bool addDescriptive(const SourceData& source, const ExportMetadata& metadata, Exiv2::ExifData& exif,
                    Exiv2::XmpData& xmp, const Warnings& warnings) {
    bool any = false;
    const PhotoMarks& marks = metadata.marks;
    // No stars is "not rated", which XMP says by leaving the property out.
    if (marks.rating != 0) {
        xmp["Xmp.xmp.Rating"] = std::to_string(marks.rating);
        any = true;
    }
    if (marks.label) {
        for (const auto& [label, name] : colorLabelNames) {
            if (label == *marks.label) {
                xmp["Xmp.xmp.Label"] = std::string(name);
                any = true;
            }
        }
    }
    for (const auto key : descriptiveExif) {
        any = copyExif(source.exif, exif, std::string(key), warnings) || any;
    }
    for (const auto& datum : source.xmp) {
        if (isOneOf(datum.key(), descriptiveXmp)) {
            any = copyXmp(datum, xmp, warnings) || any;
        }
    }
    std::error_code ignored;
    const auto sidecar = sidecarPath(metadata.source);
    if (metadata.useSidecar && std::filesystem::exists(sidecar, ignored)) {
        // The sidecar is the photographer's latest word, so it wins per property.
        try {
            for (const auto& datum : readSidecarXmp(sidecar)) {
                if (isOneOf(datum.key(), descriptiveXmp)) {
                    any = copyXmp(datum, xmp, warnings) || any;
                }
            }
        } catch (const std::exception& problem) {
            warnings.leftOut(std::string(problem.what()) +
                             ", so its title, caption, keywords, creator and rights are left out");
        }
    }
    return any;
}

} // namespace

bool arraw::carriesAnything(const MetadataSelection& selection) noexcept {
    return selection.capture || selection.location || selection.descriptive;
}

QByteArray arraw::embedMetadata(const QByteArray& encoded, const ExportMetadata& metadata,
                                const EncodedFacts& facts, DiagnosticLog& log) {
    exiv2support::prepare();
    const Warnings warnings{log, metadata.source};
    if (const int rating = metadata.marks.rating;
        metadata.selection.descriptive && (rating < rejectedRating || rating > highestRating)) {
        // A caller's bug, whatever the source holds.
        throw std::invalid_argument("The rating of exported metadata must be -1 to 5");
    }

    // Reading what to carry is best effort: an export that works without metadata
    // must not stop working because of its source.
    // An unreadable source still leaves what arraw knows itself, the marks, and
    // the sidecar's descriptive fields: only what the source holds is lost.
    SourceData source;
    try {
        const auto image = Exiv2::ImageFactory::open(exiv2support::path(metadata.source));
        if (!image) {
            throw std::runtime_error("not an image exiv2 knows");
        }
        image->readMetadata();
        source.exif = image->exifData();
        source.xmp = image->xmpData();
    } catch (const std::exception& problem) {
        warnings.leftOut(std::string("the source cannot be read (") + problem.what() + ")");
    }

    try {
        const auto output =
            Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte*>(encoded.constData()),
                                      static_cast<std::size_t>(encoded.size()));
        if (!output) {
            throw std::runtime_error("cannot open the encoded image");
        }
        output->readMetadata();
        auto& exif = output->exifData();
        auto& xmp = output->xmpData();

        bool any = false;
        const auto& selection = metadata.selection;
        if (selection.capture) {
            any = addCapture(source, exif, xmp, warnings) || any;
        }
        if (selection.location) {
            any = addLocation(source, exif, xmp, warnings) || any;
        }
        if (selection.descriptive) {
            any = addDescriptive(source, metadata, exif, xmp, warnings) || any;
        }
        if (!any) {
            return {};
        }

        // The pixels are upright and are the output's own, whatever the source's were.
        exif["Exif.Image.Orientation"] = std::uint16_t{1};
        exif["Exif.Photo.PixelXDimension"] = static_cast<std::uint32_t>(facts.width);
        exif["Exif.Photo.PixelYDimension"] = static_cast<std::uint32_t>(facts.height);
        exif["Exif.Photo.ColorSpace"] =
            std::uint16_t{facts.srgb ? std::uint16_t{1} : std::uint16_t{65535}};
        exif["Exif.Image.Software"] = std::string("arraw ") + ARRAW_VERSION;
        output->writeMetadata();

        auto& io = output->io();
        io.open();
        io.seek(0, Exiv2::BasicIo::beg);
        const Exiv2::DataBuf bytes = io.read(io.size());
        return QByteArray(reinterpret_cast<const char*>(bytes.c_data()),
                          static_cast<qsizetype>(bytes.size()));
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& problem) {
        throw std::runtime_error("Cannot write the metadata of " + metadata.source.string() +
                                 " into the export: " + problem.what());
    }
}
