#include "PyBindings.h"

#include <Develop.h>
#include <Diagnostics.h>
#include <ImageExport.h>
#include <ImageImport.h>
#include <Photo.h>
#include <PhotoMarks.h>
#include <Sidecar.h>

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
           ", settings=" + reprValue(photo.settings()) + ", marks=" + reprValue(photo.marks()) +
           ")";
}

/// @brief Derives a photograph with new settings and marks, then applies flat keywords.
///
/// `marks` replaces the marks wholesale. `rating` and `label` then change them,
/// where `label=None` clears the label; every other keyword is a develop
/// setting. Those two names are reserved: a develop setting must never be
/// called `rating` or `label`, or it would be taken for a mark.
Photo photoWith(const Photo& photo, const std::optional<DevelopSettings>& settings,
                const std::optional<PhotoMarks>& newMarks, const nb::kwargs& keywords) {
    DevelopSettings result = settings.value_or(photo.settings());
    PhotoMarks marks = newMarks.value_or(photo.marks());
    const nb::kwargs flat = nb::steal<nb::kwargs>(PyDict_New());
    for (auto [key, value] : keywords) {
        const std::string name = nb::cast<std::string>(key);
        if (name == "rating") {
            marks.rating = convertValue<int>(value, name);
        } else if (name == "label") {
            marks.label = convertValue<std::optional<ColorLabel>>(value, name);
        } else {
            flat[key] = value;
        }
    }
    applyFlatSettings(result, flat);
    return {photo.path(), photo.metadata(), result, marks};
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

    nb::enum_<ColorLabel>(m, "ColorLabel", "Colour a photograph is labelled with while culling.")
        .value("RED", ColorLabel::Red)
        .value("YELLOW", ColorLabel::Yellow)
        .value("GREEN", ColorLabel::Green)
        .value("BLUE", ColorLabel::Blue)
        .value("PURPLE", ColorLabel::Purple);

    bindFrozen<PhotoMarks>(
        m, "PhotoMarks", "Culling marks of a photograph: rating -1 (rejected) to 5, and a label.",
        field("rating", &PhotoMarks::rating), field("label", &PhotoMarks::label));

    bindFrozen<SidecarContents>(m, "SidecarContents", "What an XMP sidecar holds.",
                                field("settings", &SidecarContents::settings),
                                field("marks", &SidecarContents::marks));

    nb::class_<Photo>(m, "Photo", "One photograph as a document: a file and how it is developed.")
        .def_prop_ro("path", &Photo::path)
        .def_prop_ro("metadata", &Photo::metadata)
        .def_prop_ro("settings", &Photo::settings)
        .def_prop_ro("marks", &Photo::marks)
        .def("with_", &photoWith, "settings"_a = nb::none(), nb::kw_only(), "marks"_a = nb::none(),
             "kwargs"_a,
             "Return a photograph with `settings` (and `marks`) replacing the current ones "
             "wholesale, then flat snake_case keywords applied, e.g. exposure=0.7. `rating` and "
             "`label` change the marks instead (label=None clears it).")
        .def(
            "load",
            [](const Photo& photo) {
                // What the file declares was reported when the photograph was
                // opened; decoding it again says nothing new (as the CLI does).
                return withoutGil([&] { return loadImage(photo.path()); });
            },
            "Decode the photograph's file into a buffer.")
        .def(nb::self == nb::self)
        .def("__repr__", &photoRepr);
    m.attr("Photo").attr("__hash__") = nb::none();

    m.def(
        "open",
        [](const std::filesystem::path& path, bool sidecar) {
            PythonLog log;
            return withoutGil([&] {
                if (sidecar) {
                    return openPhoto(path, log);
                }
                return Photo(path, readImageMetadata(path, log));
            });
        },
        "path"_a, nb::kw_only(), "sidecar"_a = true,
        "Open a photograph; reads its metadata, not its pixels. Its XMP sidecar supplies the "
        "settings and marks unless sidecar=False. A sidecar that cannot be read is logged as an "
        "error on the 'arraw' logger, not raised, and the defaults are used.");

    m.def(
        "sidecar_path",
        [](const std::filesystem::path& path) {
            return withoutGil([&] { return sidecarPath(path); });
        },
        "path"_a, "Name the XMP sidecar of a photograph; nothing is created.");

    m.def(
        "read_sidecar",
        [](const std::filesystem::path& path) {
            PythonLog log;
            return withoutGil([&] { return readSidecar(path, log); });
        },
        "path"_a, "Read the sidecar of a photograph, or None when it has none.");

    m.def(
        "write_sidecar", [](const Photo& photo) { withoutGil([&] { writeSidecar(photo); }); },
        "photo"_a, "Write a photograph's settings and marks into its sidecar, keeping the rest.");

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
            // Reported once, by open(), as in Photo.load.
            return withoutGil([&] {
                const ImageBuffer source = loadImage(photo.path());
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
