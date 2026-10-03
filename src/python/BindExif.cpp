#include "PyBindings.h"

#include <ExifInfo.h>

#include <filesystem>

namespace arraw::python {

void bindExif(nb::module_& m) {
    bindFrozen<URational, false>(m, "URational",
                                 "An unsigned fraction, as EXIF stores an exposure time or an "
                                 "f-number: a numerator and a denominator.",
                                 field("numerator", &URational::numerator),
                                 field("denominator", &URational::denominator))
        .def("value", &URational::value,
             "The quotient as a float, or NaN when the denominator is zero.");

    bindFrozen<SRational, false>(
        m, "SRational", "A signed fraction, as EXIF stores an exposure bias.",
        field("numerator", &SRational::numerator), field("denominator", &SRational::denominator))
        .def("value", &SRational::value,
             "The quotient as a float, or NaN when the denominator is zero.");

    bindFrozen<GpsPosition>(m, "GpsPosition",
                            "Where a photograph was taken: signed decimal degrees (negative "
                            "south and west) and metres above sea level.",
                            field("latitude", &GpsPosition::latitude),
                            field("longitude", &GpsPosition::longitude),
                            field("altitude", &GpsPosition::altitude));

    bindFrozen<ExifInfo>(
        m, "ExifInfo",
        "What a photograph's EXIF records about its capture; every field is None when absent.",
        field("make", &ExifInfo::make), field("model", &ExifInfo::model),
        field("lens_model", &ExifInfo::lensModel),
        field("date_time_original", &ExifInfo::dateTimeOriginal),
        field("offset_time_original", &ExifInfo::offsetTimeOriginal),
        field("exposure_time", &ExifInfo::exposureTime), field("f_number", &ExifInfo::fNumber),
        field("photographic_sensitivity", &ExifInfo::photographicSensitivity),
        field("focal_length", &ExifInfo::focalLength),
        field("focal_length_in_35mm_film", &ExifInfo::focalLengthIn35mmFilm),
        field("exposure_bias_value", &ExifInfo::exposureBiasValue),
        field("flash", &ExifInfo::flash), field("gps", &ExifInfo::gps),
        field("artist", &ExifInfo::artist), field("copyright", &ExifInfo::copyright));

    m.def(
        "read_exif",
        [](const std::filesystem::path& path) {
            PythonLog log;
            return withoutGil([&] { return readExif(path, log); });
        },
        "path"_a,
        "Read what a file records about its capture (EXIF). A file with no readable EXIF gives an "
        "ExifInfo with every field None, and a message on the 'arraw' logger; a file that does "
        "not exist raises.");
}

} // namespace arraw::python
