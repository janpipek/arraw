#include "PyBindings.h"

#include <CurveHistogram.h>
#include <Develop.h>
#include <DevelopState.h>
#include <Diagnostics.h>
#include <Edits.h>
#include <ImageExport.h>
#include <ImageImport.h>
#include <LocalAdjustmentEdits.h>
#include <Photo.h>
#include <PhotoMarks.h>
#include <Sidecar.h>

#include <nanobind/ndarray.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

/// @brief Applies snake_case keywords one after the other by the rules of ::arraw::withValue.
///
/// Each keyword is converted to the type its setting takes and set in the order given, so a
/// rule that reads the state as it stands (a crop aspect after a rectangle, a straighten after a
/// turn) sees what the keywords before it did.
/// @throws nb::type_error for an unknown keyword or a value of the wrong type.
/// @throws std::invalid_argument (a ValueError) as ::arraw::withValue.
DevelopState editedState(const ImageMetadata& metadata, DevelopState state,
                         const nb::kwargs& keywords) {
    for (auto [key, value] : keywords) {
        const std::string name = nb::cast<std::string>(key);
        const FieldDescriptor* match = findSettingByKeyword(name);
        if (match == nullptr) {
            throw nb::type_error(("unknown develop setting '" + name + "'").c_str());
        }
        DevelopSettings scratch = state.settings;
        visitField(*match, scratch, [&](auto& leaf) {
            using Leaf = std::remove_cvref_t<decltype(leaf)>;
            if constexpr (std::is_same_v<Leaf, ToneCurve>) {
                // A curve may be given as a ToneCurve or as its list of (x, y) points.
                if (!nb::isinstance<ToneCurve>(value)) {
                    ToneCurve curve{convertValue<std::vector<CurvePoint>>(value, name)};
                    normaliseCurvePoints(curve.points);
                    state = withValue(metadata, std::move(state), match->key, curve);
                    return;
                }
            }
            state =
                withValue(metadata, std::move(state), match->key, convertValue<Leaf>(value, name));
        });
    }
    return state;
}

/// @brief Finds the local control a snake_case keyword names.
/// @return The row of ::arraw::localAdjustmentDescriptors, or null for an unknown keyword.
const LocalDescriptor* findLocalByKeyword(std::string_view name) {
    for (const LocalDescriptor& row : localAdjustmentDescriptors) {
        if (row.pythonName == name) {
            return &row;
        }
    }
    return nullptr;
}

/// @brief Reads the keywords of a new mask that are not deltas, and then the deltas.
///
/// The deltas are set by the table's Python names, as the number is given; whether it is in
/// range is for ::arraw::withLocalAdjustmentAdded to say, which clamps.
/// @throws nb::type_error for an unknown keyword or a value of the wrong type.
LocalDeltas deltasFrom(const nb::kwargs& keywords) {
    LocalDeltas deltas;
    for (auto [key, value] : keywords) {
        const std::string name = nb::cast<std::string>(key);
        const LocalDescriptor* match = findLocalByKeyword(name);
        if (match == nullptr) {
            throw nb::type_error(("unknown local adjustment keyword '" + name + "'").c_str());
        }
        deltas.*match->member = convertValue<float>(value, name);
    }
    return deltas;
}

/// @brief Adds a mask to a photograph's state, as a new photograph.
/// @throws std::invalid_argument (a ValueError) if the list is full or a number is not finite.
Photo withMaskAdded(const Photo& photo, Mask shape, std::string name, const nb::object& opacity,
                    bool invert, bool enabled, const nb::kwargs& deltas) {
    LocalAdjustment adjustment;
    adjustment.shape = std::move(shape);
    adjustment.name = std::move(name);
    adjustment.enabled = enabled;
    adjustment.invert = invert;
    adjustment.opacity = convertValue<float>(opacity, "opacity");
    adjustment.deltas = deltasFrom(deltas);
    return {photo.path(), photo.metadata(),
            withLocalAdjustmentAdded(photo.state(), std::move(adjustment)), photo.marks()};
}

/// @brief Changes one adjustment of a photograph by the keywords, in the order given, as a new
/// photograph.
///
/// `name`, `enabled`, `invert`, `opacity` and `shape` change the adjustment's own fields (a
/// shape must be of the kind the adjustment has), every other keyword is a delta by its Python
/// name. Each is applied by the edit rules of ::arraw::withLocalDelta and its siblings, which
/// clamp a number to its range.
/// @throws nb::type_error for an unknown keyword or a value of the wrong type.
/// @throws std::invalid_argument (a ValueError) for an unknown id, a shape of another kind, or a
/// number that is not finite.
Photo withLocalChanged(const Photo& photo, LocalAdjustmentId id, const nb::kwargs& keywords) {
    DevelopState state = photo.state();
    if (findLocalAdjustment(state, id) == nullptr) {
        throw std::invalid_argument("no local adjustment with id " + std::to_string(id.value));
    }
    for (auto [key, value] : keywords) {
        const std::string name = nb::cast<std::string>(key);
        if (name == "name") {
            state = withLocalAdjustmentRenamed(std::move(state), id,
                                               convertValue<std::string>(value, name));
        } else if (name == "enabled") {
            state =
                withLocalAdjustmentEnabled(std::move(state), id, convertValue<bool>(value, name));
        } else if (name == "invert") {
            state =
                withLocalAdjustmentInverted(std::move(state), id, convertValue<bool>(value, name));
        } else if (name == "opacity") {
            state = withLocalOpacity(std::move(state), id, convertValue<float>(value, name));
        } else if (name == "shape") {
            state = withLocalShape(std::move(state), id, convertValue<Mask>(value, name));
        } else if (const LocalDescriptor* match = findLocalByKeyword(name)) {
            state =
                withLocalDelta(std::move(state), id, match->key, convertValue<double>(value, name));
        } else {
            throw nb::type_error(("unknown local adjustment keyword '" + name + "'").c_str());
        }
    }
    return {photo.path(), photo.metadata(), std::move(state), photo.marks()};
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
        "Everything that says how one photograph is developed: its global settings, and its local "
        "adjustments (masks), a tuple of LocalAdjustment with ids, in the order they sum in. "
        "`next_local_adjustment_id` is the id the next added mask takes; it never goes down.",
        field("settings", &DevelopState::settings),
        field("local_adjustments", &DevelopState::localAdjustments),
        field("next_local_adjustment_id", &DevelopState::nextLocalAdjustmentId))
        .def_prop_ro(
            "local_adjustments",
            [](const DevelopState& state) { return hashable(state.localAdjustments); },
            nb::sig("def local_adjustments(self) -> tuple[LocalAdjustment, ...]"),
            "The masked adjustments, in the order they sum in: a tuple of LocalAdjustment.");

    bindFrozen<SidecarContents>(
        m, "SidecarContents",
        "What an XMP sidecar holds, and which other tools wrote in it. `state` is None when the "
        "sidecar records no develop settings (marks only, or another tool's).",
        field("state", &SidecarContents::state), field("marks", &SidecarContents::marks),
        field("creator_tool", &SidecarContents::creatorTool),
        field("others", &SidecarContents::others));

    m.def(
        "default_state",
        [](const ImageMetadata& metadata) { return defaultStateFor(metadata.encoding); },
        "metadata"_a,
        "The state a photograph of this kind starts from: colour noise reduction 25 for a RAW, "
        "the neutral DevelopState() for anything else. What open() gives a photograph with no "
        "sidecar.");
    m.def(
        "default_state",
        [](const ImageBuffer& buffer) { return defaultStateFor(buffer.encoding()); }, "buffer"_a,
        "The state a decoded buffer's kind starts from, as for its metadata.");

    m.def(
        "turned",
        [](const ImageMetadata& metadata, const DevelopState& state, bool clockwise) {
            return turned(metadata, state, clockwise);
        },
        "metadata"_a, "state"_a, "clockwise"_a,
        "Turn the photograph by a quarter as it appears on screen, carrying the crop.");
    m.def(
        "flipped",
        [](const ImageMetadata& metadata, const DevelopState& state, bool horizontal) {
            return flipped(metadata, state, horizontal);
        },
        "metadata"_a, "state"_a, "horizontal"_a,
        "Mirror the photograph as it appears on screen, carrying the crop.");
    m.def(
        "with_aspect",
        [](const ImageMetadata& metadata, const DevelopState& state, const CropAspect& aspect) {
            return withAspect(metadata, state, aspect);
        },
        "metadata"_a, "state"_a, "aspect"_a,
        "Set the crop aspect, fitting the crop to a ratio. ValueError without the photograph's "
        "size.");
    m.def(
        "with_locked_aspect",
        [](const ImageMetadata& metadata, const DevelopState& state) {
            return withLockedAspect(metadata, state);
        },
        "metadata"_a, "state"_a, "Lock the aspect at the crop's present ratio.");
    m.def(
        "with_swapped_orientation",
        [](const ImageMetadata& metadata, const DevelopState& state) {
            return withSwappedOrientation(metadata, state);
        },
        "metadata"_a, "state"_a, "Swap portrait and landscape.");
    m.def(
        "with_crop_reset",
        [](const ImageMetadata& metadata, const DevelopState& state) {
            return withCropReset(metadata, state);
        },
        "metadata"_a, "state"_a, "Return to automatic framing, keeping the aspect constraint.");
    m.def(
        "displayed_straighten",
        [](const DevelopState& state) { return displayedStraighten(state); }, "state"_a,
        "The straighten as it appears on screen: degrees, clockwise positive. Needs no metadata.");
    m.def(
        "with_displayed_straighten",
        [](const ImageMetadata& metadata, const DevelopState& state, double displayed) {
            return withDisplayedStraighten(metadata, state, displayed);
        },
        "metadata"_a, "state"_a, "displayed"_a,
        "Straighten to an angle as it appears on screen (clockwise positive), shrinking the "
        "crop as the `straighten` setting does.");

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
             "`label` change the marks instead (label=None clears it). Applies no rules: each "
             "keyword is assigned as it is, so a turn leaves the crop where it was. `edited` "
             "applies the rules of the editing frontends.")
        .def(
            "edited",
            [](const Photo& photo, const nb::kwargs& keywords) {
                return Photo(photo.path(), photo.metadata(),
                             editedState(photo.metadata(), photo.state(), keywords), photo.marks());
            },
            "kwargs"_a,
            "Return a photograph with flat snake_case keywords set, e.g. exposure=0.7, by the "
            "rules the app and the command line apply: a temperature makes the white balance "
            "Custom, grain turned on gets a seed, a turn carries the crop, a straighten shrinks "
            "it, a crop rectangle frees the aspect. Keywords are applied in the order given, "
            "so crop_aspect then crop_rectangle differs from the reverse. Raises TypeError for "
            "an unknown keyword or a wrong type, and ValueError for a value the setting refuses "
            "or a geometry that does not fit the photograph's size.")
        .def_prop_ro(
            "local_adjustments",
            [](const Photo& photo) { return hashable(photo.state().localAdjustments); },
            nb::sig("def local_adjustments(self) -> tuple[LocalAdjustment, ...]"),
            "The photograph's masked adjustments, ids included, in the order they sum in; the "
            "same as `state.local_adjustments`.")
        .def(
            "add_linear_mask",
            [](const Photo& photo, const CorrectedPoint& from, const CorrectedPoint& to,
               const std::string& name, const nb::object& opacity, bool invert, bool enabled,
               const nb::kwargs& deltas) {
                return withMaskAdded(photo, LinearMask{from, to}, name, opacity, invert, enabled,
                                     deltas);
            },
            "from_"_a, "to"_a, nb::kw_only(), "name"_a = "", "opacity"_a = nb::float_(1.0),
            "invert"_a = false, "enabled"_a = true, "kwargs"_a,
            nb::sig("def add_linear_mask(self, from_: tuple[float, float], to: tuple[float, "
                    "float], *, name: str = '', opacity: float = 1.0, invert: bool = False, "
                    "enabled: bool = True, **deltas: float) -> Photo"),
            "Return a photograph with a linear (graduated) mask added last, so that it is "
            "`local_adjustments[-1]`. `from_` and `to` are (u, v) points normalised to the "
            "corrected frame; the weight is 1 at `from_` and 0 at `to`. The remaining keywords "
            "are the mask's deltas by their snake_case names (relative_temperature, "
            "relative_tint, exposure, contrast, highlights, shadows, whites, blacks, texture, "
            "clarity, dehaze, saturation, vibrance), added to the global settings where the mask "
            "has weight. Numbers out of range are clamped; a non-finite or degenerate one, a "
            "full list (16) or an unknown keyword is refused (ValueError, TypeError).")
        .def(
            "add_radial_mask",
            [](const Photo& photo, const CorrectedPoint& centre, const nb::object& radiusX,
               const nb::object& radiusY, const nb::object& angle, const nb::object& feather,
               const std::string& name, const nb::object& opacity, bool invert, bool enabled,
               const nb::kwargs& deltas) {
                const RadialMask mask{.centre = centre,
                                      .radiusX = convertValue<float>(radiusX, "radius_x"),
                                      .radiusY = convertValue<float>(radiusY, "radius_y"),
                                      .angle = convertValue<float>(angle, "angle"),
                                      .feather = convertValue<float>(feather, "feather")};
                return withMaskAdded(photo, mask, name, opacity, invert, enabled, deltas);
            },
            "centre"_a, "radius_x"_a, "radius_y"_a, "angle"_a = nb::float_(0.0),
            "feather"_a = nb::float_(0.5), nb::kw_only(), "name"_a = "",
            "opacity"_a = nb::float_(1.0), "invert"_a = false, "enabled"_a = true, "kwargs"_a,
            nb::sig("def add_radial_mask(self, centre: tuple[float, float], radius_x: float, "
                    "radius_y: float, angle: float = 0.0, feather: float = 0.5, *, name: str = "
                    "'', opacity: float = 1.0, invert: bool = False, enabled: bool = True, "
                    "**deltas: float) -> Photo"),
            "Return a photograph with a radial (oval) mask added last. `centre` is a (u, v) "
            "point normalised to the corrected frame, the radii are in long-edge units, `angle` "
            "turns the x radius towards +y in degrees, and `feather` is the soft edge's width "
            "from 0 (hard) to 1. Keywords and errors are as for add_linear_mask.")
        .def("with_local_adjustment", &withLocalChanged, "id"_a, "kwargs"_a,
             nb::sig("def with_local_adjustment(self, id: int, **changes: typing.Any) -> Photo"),
             "Return a photograph with the adjustment of that id changed. The keywords, applied "
             "in the order given, are `name`, `enabled`, `invert`, `opacity`, `shape` (a "
             "LinearMask or RadialMask of the kind the adjustment already has) and the deltas "
             "by their snake_case names; numbers out of range are clamped. ValueError for an "
             "unknown id or a shape of another kind, TypeError for an unknown keyword.")
        .def(
            "without_local_adjustment",
            [](const Photo& photo, LocalAdjustmentId id) {
                return Photo(photo.path(), photo.metadata(),
                             withLocalAdjustmentRemoved(photo.state(), id), photo.marks());
            },
            "id"_a,
            "Return a photograph without the adjustment of that id; its id is not reused. "
            "ValueError for an unknown id.")
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
        "state and marks unless sidecar=False. Without one, or with one that records no "
        "settings, the state is default_state(metadata). A sidecar that cannot be read is "
        "logged as an error on the 'arraw' logger, not raised, and the defaults are used.");

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
            return withoutGil([&] {
                return develop(source, state.value_or(defaultStateFor(source.encoding())), request);
            });
        },
        "source"_a, "state"_a = nb::none(), nb::kw_only(), "size"_a = nb::none(),
        "filter"_a = requestDefaults.filter,
        "allow_upscale"_a = (requestDefaults.upscale == Upscale::Allowed),
        nb::sig("def develop(source: ImageBuffer, state: DevelopState | None = None, *, "
                "size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = "
                "arraw._arraw.ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer"),
        "Develop a decoded buffer on the CPU; with no state, the defaults of its kind "
        "(default_state: colour noise reduction for a RAW, nothing for anything else). "
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

    nb::enum_<Tap>(m, "Tap", "Named position inside the pointwise chain that sample() stops at.")
        .value("CURVE_INPUT", Tap::CurveInput,
               "What the tone curves take in: after white balance, exposure and Basic Tone; "
               "handed back in NamedEncoding.REC2020_GAMMA22.");

    m.def(
        "sample",
        [](const ImageBuffer& source, Tap tap, const std::optional<DevelopState>& state,
           const nb::object& size, const nb::object& filter, bool allowUpscale) {
            const RenderRequest request = requestFrom(size, filter, allowUpscale);
            return withoutGil([&] {
                return sample(source, state.value_or(defaultStateFor(source.encoding())), tap,
                              request);
            });
        },
        "source"_a, "tap"_a, "state"_a = nb::none(), nb::kw_only(), "size"_a = nb::none(),
        "filter"_a = requestDefaults.filter,
        "allow_upscale"_a = (requestDefaults.upscale == Upscale::Allowed),
        nb::sig("def sample(source: ImageBuffer, tap: Tap, state: DevelopState | None = None, *, "
                "size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = "
                "arraw._arraw.ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer"),
        "Develop a decoded buffer on the CPU with the chain stopped at `tap`, to measure it: the "
        "same frame and size as develop() with the same arguments, in the tap's encoding.");

    m.def(
        "sample",
        [](const Photo& photo, Tap tap, const std::optional<DevelopState>& state,
           const nb::object& size, const nb::object& filter, bool allowUpscale) {
            const RenderRequest request = requestFrom(size, filter, allowUpscale);
            return withoutGil([&] {
                const ImageBuffer source = loadImage(photo.path());
                return sample(source, state.value_or(photo.state()), tap, request);
            });
        },
        "source"_a, "tap"_a, "state"_a = nb::none(), nb::kw_only(), "size"_a = nb::none(),
        "filter"_a = requestDefaults.filter,
        "allow_upscale"_a = (requestDefaults.upscale == Upscale::Allowed),
        nb::sig("def sample(source: Photo, tap: Tap, state: DevelopState | None = None, *, "
                "size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = "
                "arraw._arraw.ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer"),
        "Decode a photograph and sample it at `tap` with its own state unless `state` is given.");

    m.attr("CURVE_HISTOGRAM_BINS") = curveHistogramBins;
    // The curve_histogram signatures and docstrings below spell the default out.
    static_assert(curveHistogramLongEdge == 1024);

    // A read-only view of the bins, not a copy; it keeps the histogram alive.
    const auto binsOf = [](CurveHistogram::Bins CurveHistogram::* channel) {
        return [channel](nb::handle self) {
            const auto& bins = nb::cast<const CurveHistogram&>(self).*channel;
            return nb::ndarray<nb::numpy, const std::uint64_t, nb::shape<curveHistogramBins>>(
                bins.data(), {curveHistogramBins}, self);
        };
    };
    nb::class_<CurveHistogram>(
        m, "CurveHistogram",
        "Pixel counts of the curve input over the perceptual coordinate, CURVE_HISTOGRAM_BINS "
        "bins from 0 to 1 per channel; not constructible from Python.")
        .def_prop_ro("luma", binsOf(&CurveHistogram::luma),
                     "Read-only uint64 array of the luminance counts, as the luma curve reads it.")
        .def_prop_ro("red", binsOf(&CurveHistogram::red),
                     "Read-only uint64 array of the red counts.")
        .def_prop_ro("green", binsOf(&CurveHistogram::green),
                     "Read-only uint64 array of the green counts.")
        .def_prop_ro("blue", binsOf(&CurveHistogram::blue),
                     "Read-only uint64 array of the blue counts.")
        .def_ro("pixels", &CurveHistogram::pixels,
                "Number of pixels counted; fully transparent ones are not.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const CurveHistogram& histogram) {
            return "CurveHistogram(pixels=" + std::to_string(histogram.pixels) + ")";
        });
    m.attr("CurveHistogram").attr("__hash__") = nb::none();

    m.def(
        "curve_histogram",
        [](const ImageBuffer& image) { return withoutGil([&] { return curveHistogram(image); }); },
        "image"_a,
        "Count a sample taken at Tap.CURVE_INPUT (NamedEncoding.REC2020_GAMMA22) into a "
        "CurveHistogram.");

    m.def(
        "curve_histogram",
        [](const ImageBuffer& source, const DevelopState& state, const nb::object& size,
           bool allowUpscale) {
            const RenderRequest request =
                requestFrom(size, nb::cast(curveHistogramRequest.filter), allowUpscale);
            return withoutGil([&] { return curveHistogram(source, state, request); });
        },
        "source"_a, "state"_a, nb::kw_only(), "size"_a.none() = curveHistogramLongEdge,
        "allow_upscale"_a = (curveHistogramRequest.upscale == Upscale::Allowed),
        nb::sig("def curve_histogram(source: ImageBuffer, state: DevelopState, *, size: int | "
                "tuple[int, int] | float | None = 1024, allow_upscale: bool = False) -> "
                "CurveHistogram"),
        "Sample a decoded buffer at Tap.CURVE_INPUT and count it. `size` is as for develop() "
        "and defaults to a 1024-pixel long edge; None counts the full cropped resolution. The "
        "resize is always bilinear, so no ringing reaches the end bins.");

    m.def(
        "curve_histogram",
        [](const Photo& photo, const std::optional<DevelopState>& state, const nb::object& size,
           bool allowUpscale) {
            const RenderRequest request =
                requestFrom(size, nb::cast(curveHistogramRequest.filter), allowUpscale);
            return withoutGil([&] {
                const ImageBuffer source = loadImage(photo.path());
                return curveHistogram(source, state.value_or(photo.state()), request);
            });
        },
        "source"_a, "state"_a = nb::none(), nb::kw_only(), "size"_a.none() = curveHistogramLongEdge,
        "allow_upscale"_a = (curveHistogramRequest.upscale == Upscale::Allowed),
        nb::sig("def curve_histogram(source: Photo, state: DevelopState | None = None, *, size: "
                "int | tuple[int, int] | float | None = 1024, allow_upscale: bool = False) -> "
                "CurveHistogram"),
        "Decode a photograph, sample it at Tap.CURVE_INPUT with its own state unless `state` is "
        "given, and count it; `size` and the resize as for a decoded buffer.");

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

    bindFrozen<MetadataSelection>(
        m, "MetadataSelection",
        "Groups of metadata an export carries from its source photograph: capture (camera, lens, "
        "exposure, time), location (GPS) and descriptive (rating, label, title, caption, "
        "keywords, creator, rights).",
        field("capture", &MetadataSelection::capture),
        field("location", &MetadataSelection::location),
        field("descriptive", &MetadataSelection::descriptive));

    const ExportOptions exportDefaults{};
    m.def(
        "save",
        [](const ImageBuffer& image, const std::filesystem::path& path,
           std::optional<ImageFileFormat> format, NamedEncoding encoding, int bitDepth, int quality,
           bool embedProfile, int sharpening, const std::optional<Photo>& metadataFrom,
           const MetadataSelection& metadata) {
            const ExportOptions options{format,  encoding,     bitDepth,
                                        quality, embedProfile, sharpening};
            std::optional<ExportMetadata> carried;
            if (metadataFrom) {
                carried = ExportMetadata{metadataFrom->path(), metadataFrom->marks(), metadata};
            }
            PythonLog log;
            withoutGil([&] { exportImage(image, path, options, carried, log); });
        },
        "image"_a, "path"_a, nb::kw_only(), "format"_a = exportDefaults.format,
        "encoding"_a = exportDefaults.encoding, "bit_depth"_a = exportDefaults.bitDepth,
        "quality"_a = exportDefaults.quality, "embed_profile"_a = exportDefaults.embedProfile,
        "sharpening"_a = exportDefaults.sharpening, "metadata_from"_a = std::optional<Photo>{},
        "metadata"_a = MetadataSelection{},
        "Write an image as JPEG, PNG or TIFF; the format comes from the extension unless given. "
        "`sharpening` (0-100, default 0 = off) applies an unsharp mask to the final pixels. "
        "With `metadata_from` (the photograph the pixels came from) the groups of `metadata` "
        "are copied from its file and sidecar, and its marks written as rating and label; "
        "without it nothing is written. A source or sidecar that cannot be read is logged as a "
        "warning on the 'arraw' logger and its metadata left out; the file is still written.");
}

} // namespace arraw::python
