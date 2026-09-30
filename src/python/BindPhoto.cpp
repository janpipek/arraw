#include "PyBindings.h"

#include <Develop.h>
#include <Diagnostics.h>
#include <ImageExport.h>
#include <Photo.h>

#include <exception>
#include <string>

namespace arraw::python {

void PythonLog::record(const Diagnostic& diagnostic) {
    const nb::gil_scoped_acquire gil;
    try {
        int level = 20; // logging.INFO
        switch (diagnostic.severity) {
        case Severity::Info:
            level = 20;
            break;
        case Severity::Warning:
            level = 30; // logging.WARNING
            break;
        case Severity::Error:
            level = 40; // logging.ERROR
            break;
        }
        nb::module_::import_("logging").attr("getLogger")("arraw").attr("log")(
            level, describe(diagnostic));
    } catch (const nb::python_error&) {
        // A failing logging handler must not fail the photograph.
    } catch (const std::exception&) {
        // Nor may a C++ failure escape into core code that expects a log that never throws.
        PyErr_Clear();
    }
}

namespace {

/// @brief Formats a photograph for repr.
std::string photoRepr(const Photo& photo) {
    return "Photo(path=" + reprValue(photo.path()) + ", metadata=" + reprValue(photo.metadata()) +
           ", settings=" + reprValue(photo.settings()) + ")";
}

/// @brief Derives a photograph with new settings, then applies flat keywords.
Photo photoWith(const Photo& photo, const std::optional<DevelopSettings>& settings,
                const nb::kwargs& keywords) {
    DevelopSettings result = settings.value_or(photo.settings());
    applyFlatSettings(result, keywords);
    return photo.with(result);
}

} // namespace

void bindPhoto(nb::module_& m) {
    nb::enum_<Severity>(m, "Severity", "How serious a diagnostic is.")
        .value("INFO", Severity::Info)
        .value("WARNING", Severity::Warning)
        .value("ERROR", Severity::Error);

    nb::enum_<ImageFileFormat>(m, "ImageFileFormat", "File format an image can be saved as.")
        .value("JPEG", ImageFileFormat::Jpeg)
        .value("PNG", ImageFileFormat::Png)
        .value("TIFF", ImageFileFormat::Tiff);

    nb::class_<Photo>(m, "Photo", "One photograph as a document: a file and how it is developed.")
        .def_prop_ro("path", &Photo::path)
        .def_prop_ro("metadata", &Photo::metadata)
        .def_prop_ro("settings", &Photo::settings)
        .def("with_", &photoWith, "settings"_a = nb::none(), "kwargs"_a,
             "Return a photograph with `settings` replacing the current ones wholesale, then "
             "flat snake_case keywords applied, e.g. exposure=0.7.")
        .def(
            "load",
            [](const Photo& photo) {
                PythonLog log;
                return withoutGil([&] { return loadImage(photo.path(), log); });
            },
            "Decode the photograph's file into a buffer.")
        .def(nb::self == nb::self)
        .def("__repr__", &photoRepr);
    m.attr("Photo").attr("__hash__") = nb::none();

    m.def(
        "open",
        [](const std::filesystem::path& path) {
            PythonLog log;
            return withoutGil([&] { return openPhoto(path, log); });
        },
        "path"_a, "Open a photograph; reads its metadata, not its pixels.");

    m.def(
        "develop",
        [](const ImageBuffer& source, const std::optional<DevelopSettings>& settings) {
            return withoutGil(
                [&] { return develop(source, settings.value_or(DevelopSettings{})); });
        },
        "source"_a, "settings"_a = nb::none(),
        "Develop a decoded buffer on the CPU; default settings leave the colour unchanged.");

    m.def(
        "develop",
        [](const Photo& photo, const std::optional<DevelopSettings>& settings) {
            PythonLog log;
            return withoutGil([&] {
                const ImageBuffer source = loadImage(photo.path(), log);
                return develop(source, settings.value_or(photo.settings()));
            });
        },
        "source"_a, "settings"_a = nb::none(),
        "Decode a photograph and develop it with its own settings unless `settings` is given.");

    const ExportOptions exportDefaults{};
    m.def(
        "save",
        [](const ImageBuffer& image, const std::filesystem::path& path,
           std::optional<ImageFileFormat> format, NamedEncoding encoding, int bitDepth, int quality,
           bool embedProfile) {
            const ExportOptions options{format, encoding, bitDepth, quality, embedProfile};
            withoutGil([&] { exportImage(image, path, options); });
        },
        "image"_a, "path"_a, nb::kw_only(), "format"_a = exportDefaults.format,
        "encoding"_a = exportDefaults.encoding, "bit_depth"_a = exportDefaults.bitDepth,
        "quality"_a = exportDefaults.quality, "embed_profile"_a = exportDefaults.embedProfile,
        "Write an image as JPEG, PNG or TIFF; the format comes from the extension unless given.");
}

} // namespace arraw::python
