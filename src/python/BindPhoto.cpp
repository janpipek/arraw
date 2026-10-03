#include "PyBindings.h"

#include <Develop.h>
#include <DevelopState.h>
#include <Diagnostics.h>
#include <ImageExport.h>
#include <ImageImport.h>
#include <Photo.h>
#include <PhotoMarks.h>
#include <Sidecar.h>

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
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
           ", state=" + reprValue(photo.state()) + ", marks=" + reprValue(photo.marks()) + ")";
}

/// @brief Derives a photograph with a new state and marks, then applies flat keywords.
///
/// `state` replaces the develop state wholesale, and the flat keywords then edit its settings.
/// `marks` replaces the marks wholesale. `rating` and `label` then change them,
/// where `label=None` clears the label; every other keyword is a develop
/// setting. Those two names are reserved: a develop setting must never be
/// called `rating` or `label`, or it would be taken for a mark.
Photo photoWith(const Photo& photo, const std::optional<DevelopState>& newState,
                const std::optional<PhotoMarks>& newMarks, const nb::kwargs& keywords) {
    DevelopState result = newState.value_or(photo.state());
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
    applyFlatSettings(result.settings, flat);
    return {photo.path(), photo.metadata(), result, marks};
}

/// @brief Reads one side of a requested size: a positive Python int that fits a 32-bit side.
/// @throws nb::type_error if @p value is not an int, a bool included.
/// @throws nb::value_error if it is not positive or is too large.
std::uint32_t readSide(const nb::handle& value, const char* what) {
    if (!PyLong_Check(value.ptr()) || PyBool_Check(value.ptr())) {
        throw nb::type_error(what);
    }
    int overflow = 0;
    const long long number = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
    if (overflow == 0 && number == -1 && PyErr_Occurred() != nullptr) {
        throw nb::python_error();
    }
    if (overflow != 0 || number <= 0 ||
        number > static_cast<long long>(std::numeric_limits<std::uint32_t>::max())) {
        throw nb::value_error("size must be positive and fit in 32 bits");
    }
    return static_cast<std::uint32_t>(number);
}

/// @brief Builds a render request from the keywords of `develop`, strictly typed.
///
/// An int is the long edge, a pair of ints is a box to fit inside, and a float
/// is a scale factor, the three forms of ADR 007's `--resize`. A bool is none of them.
/// @param size The `size` keyword, or None for the photograph's own resolution.
/// @param filter The `filter` keyword, which must be a ResizeFilter member.
/// @param allowUpscale The `allow_upscale` keyword.
/// @throws nb::type_error if @p size or @p filter has the wrong type.
/// @throws nb::value_error if @p size is not positive, too large, or not finite.
RenderRequest requestFrom(const nb::object& size, const nb::object& filter, bool allowUpscale) {
    RenderRequest request;
    // Not converted: an int is not a ResizeFilter (ADR 018).
    ResizeFilter kernel{};
    if (!nb::try_cast<ResizeFilter>(filter, kernel, false)) {
        throw nb::type_error("filter must be an arraw.ResizeFilter");
    }
    request.filter = kernel;
    request.upscale = allowUpscale ? Upscale::Allowed : Upscale::Never;
    if (size.is_none()) {
        return request;
    }
    PyObject* const object = size.ptr();
    if (PyBool_Check(object)) {
        throw nb::type_error("size must be an int, a (width, height) tuple or a float, not a bool");
    }
    if (PyLong_Check(object)) {
        const std::uint32_t edge = readSide(size, "size must be an int");
        request.size = RenderRequest::FitInside{edge, edge};
    } else if (PyFloat_Check(object)) {
        const double factor = PyFloat_AsDouble(object);
        if (!std::isfinite(factor) || factor <= 0.0) {
            throw nb::value_error("a size scale factor must be finite and greater than 0");
        }
        request.size = RenderRequest::Scale{factor};
    } else if (PyTuple_Check(object) && PyTuple_GET_SIZE(object) == 2) {
        const std::uint32_t width =
            readSide(nb::handle(PyTuple_GET_ITEM(object, 0)), "size must be a pair of ints");
        const std::uint32_t height =
            readSide(nb::handle(PyTuple_GET_ITEM(object, 1)), "size must be a pair of ints");
        request.size = RenderRequest::FitInside{width, height};
    } else {
        throw nb::type_error("size must be an int (long edge), a (width, height) tuple "
                             "(fit inside) or a float (scale factor)");
    }
    return request;
}

} // namespace

void bindPhoto(nb::module_& m) {
    nb::enum_<Severity>(m, "Severity", "How serious a diagnostic is.")
        .value("INFO", Severity::Info)
        .value("WARNING", Severity::Warning)
        .value("ERROR", Severity::Error);

    nb::enum_<ResizeFilter>(m, "ResizeFilter", "Resampling kernel for a develop to a size.")
        .value("LANCZOS3", ResizeFilter::Lanczos3, "Windowed sinc of radius 3: sharp.")
        .value("BILINEAR", ResizeFilter::Bilinear, "Tent kernel: soft, never rings.");

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

    bindFrozen<ForeignNamespace>(
        m, "ForeignNamespace",
        "A namespace other tools left properties in: its URI, the prefix as written, and how "
        "many top-level properties it holds.",
        field("uri", &ForeignNamespace::uri), field("prefix", &ForeignNamespace::prefix),
        field("properties", &ForeignNamespace::properties));

    bindFrozen<DevelopState>(
        m, "DevelopState",
        "Everything that says how one photograph is developed: its global settings now, per-image "
        "edits later.",
        field("settings", &DevelopState::settings));

    bindFrozen<SidecarContents>(
        m, "SidecarContents", "What an XMP sidecar holds, and which other tools wrote in it.",
        field("state", &SidecarContents::state), field("marks", &SidecarContents::marks),
        field("creator_tool", &SidecarContents::creatorTool),
        field("others", &SidecarContents::others));

    m.def("xmp_namespace_owner", &xmpNamespaceOwner, "uri"_a,
          "Name the tool or standard behind an XMP namespace URI, or None when unknown.");

    nb::class_<Photo>(m, "Photo", "One photograph as a document: a file and how it is developed.")
        .def_prop_ro("path", &Photo::path)
        .def_prop_ro("metadata", &Photo::metadata)
        .def_prop_ro("state", &Photo::state)
        .def_prop_ro("marks", &Photo::marks)
        .def("with_", &photoWith, "state"_a = nb::none(), nb::kw_only(), "marks"_a = nb::none(),
             "kwargs"_a,
             "Return a photograph with `state` (and `marks`) replacing the current ones "
             "wholesale, then flat snake_case keywords applied, e.g. exposure=0.7, which edit the "
             "settings of the state. `rating` and "
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
        "state and marks unless sidecar=False. A sidecar that cannot be read is logged as an "
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
        "photo"_a, "Write a photograph's state and marks into its sidecar, keeping the rest.");

    m.def(
        "write_sidecar_marks",
        [](const std::filesystem::path& path, const PhotoMarks& marks) {
            withoutGil([&] { writeSidecarMarks(path, marks); });
        },
        "path"_a, "marks"_a,
        "Write only the marks of a photograph into its sidecar, keeping its settings and the "
        "rest; a sidecar is created when there is none.");

    const RenderRequest requestDefaults{};
    m.def(
        "develop",
        [](const ImageBuffer& source, const std::optional<DevelopState>& state,
           const nb::object& size, const nb::object& filter, bool allowUpscale) {
            const RenderRequest request = requestFrom(size, filter, allowUpscale);
            return withoutGil(
                [&] { return develop(source, state.value_or(DevelopState{}), request); });
        },
        "source"_a, "state"_a = nb::none(), nb::kw_only(), "size"_a = nb::none(),
        "filter"_a = requestDefaults.filter,
        "allow_upscale"_a = (requestDefaults.upscale == Upscale::Allowed),
        nb::sig("def develop(source: ImageBuffer, state: DevelopState | None = None, *, "
                "size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = "
                "arraw._arraw.ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer"),
        "Develop a decoded buffer on the CPU; default state leaves the colour unchanged. "
        "`size` renders the cropped result smaller: an int is the long edge, a (width, height) "
        "tuple a box to fit inside, a float a scale factor. Sizes only shrink unless "
        "`allow_upscale`.");

    m.def(
        "develop",
        [](const Photo& photo, const std::optional<DevelopState>& state, const nb::object& size,
           const nb::object& filter, bool allowUpscale) {
            const RenderRequest request = requestFrom(size, filter, allowUpscale);
            // Reported once, by open(), as in Photo.load.
            return withoutGil([&] {
                const ImageBuffer source = loadImage(photo.path());
                return develop(source, state.value_or(photo.state()), request);
            });
        },
        "source"_a, "state"_a = nb::none(), nb::kw_only(), "size"_a = nb::none(),
        "filter"_a = requestDefaults.filter,
        "allow_upscale"_a = (requestDefaults.upscale == Upscale::Allowed),
        nb::sig("def develop(source: Photo, state: DevelopState | None = None, *, size: int "
                "| tuple[int, int] | float | None = None, filter: ResizeFilter = "
                "arraw._arraw.ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer"),
        "Decode a photograph and develop it with its own state unless `state` is given; "
        "`size`, `filter` and `allow_upscale` are as for a decoded buffer.");

    m.def(
        "resolved_size",
        [](const nb::object& size, const ImageSize& cropped, bool allowUpscale) {
            const RenderRequest request =
                requestFrom(size, nb::cast(ResizeFilter::Lanczos3), allowUpscale);
            return resolvedSize(request, cropped);
        },
        "size"_a, "cropped"_a, nb::kw_only(),
        "allow_upscale"_a = (requestDefaults.upscale == Upscale::Allowed),
        nb::sig("def resolved_size(size: int | tuple[int, int] | float, cropped: ImageSize, *, "
                "allow_upscale: bool = False) -> ImageSize"),
        "Resolve a develop `size` against the size after the crop, as develop does.");

    const ExportOptions exportDefaults{};
    m.def(
        "save",
        [](const ImageBuffer& image, const std::filesystem::path& path,
           std::optional<ImageFileFormat> format, NamedEncoding encoding, int bitDepth, int quality,
           bool embedProfile, int sharpening) {
            const ExportOptions options{format,  encoding,     bitDepth,
                                        quality, embedProfile, sharpening};
            withoutGil([&] { exportImage(image, path, options); });
        },
        "image"_a, "path"_a, nb::kw_only(), "format"_a = exportDefaults.format,
        "encoding"_a = exportDefaults.encoding, "bit_depth"_a = exportDefaults.bitDepth,
        "quality"_a = exportDefaults.quality, "embed_profile"_a = exportDefaults.embedProfile,
        "sharpening"_a = exportDefaults.sharpening,
        "Write an image as JPEG, PNG or TIFF; the format comes from the extension unless given. "
        "`sharpening` (0-100, default 0 = off) applies an unsharp mask to the final pixels.");
}

} // namespace arraw::python
