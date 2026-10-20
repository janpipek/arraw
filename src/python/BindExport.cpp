#include "PyBindings.h"

#include <ConfigDirectory.h>
#include <ExportPresets.h>
#include <ExportSettings.h>

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace arraw::python {
namespace {

/// @brief Python type of ::arraw::ExportPresetError; the module holds the reference.
PyObject* exportPresetErrorType = nullptr;

/// @brief Turns an ::arraw::ExportPresetError into the Python exception, with `.problem` set.
void translateExportPresetError(const std::exception_ptr& pointer, void*) {
    try {
        std::rethrow_exception(pointer);
    } catch (const ExportPresetError& error) {
        nb::object value = nb::borrow(exportPresetErrorType)(error.what());
        value.attr("problem") = nb::cast(error.problem());
        PyErr_SetObject(exportPresetErrorType, value.ptr());
    }
}

} // namespace

void bindExport(nb::module_& m) {
    bindFrozen<ExportSettings>(
        m, "ExportSettings",
        "What an export is asked for: file format, output colour, 16 bits or 8, the box to fit "
        "(`resize` with `width` and `height`, which `allow_enlarging` lets exceed the "
        "photograph's own size), JPEG `quality`, output `sharpening`, and the metadata groups "
        "(`capture_info`, `location`, `descriptive`) the file carries. Presets are named "
        "ExportSettings, and `save(..., settings=...)` writes with them.",
        field("format", &ExportSettings::format), field("encoding", &ExportSettings::encoding),
        field("sixteen_bit", &ExportSettings::sixteenBit), field("resize", &ExportSettings::resize),
        field("width", &ExportSettings::width), field("height", &ExportSettings::height),
        field("allow_enlarging", &ExportSettings::allowEnlarging),
        field("quality", &ExportSettings::quality),
        field("sharpening", &ExportSettings::sharpening),
        field("capture_info", &ExportSettings::captureInfo),
        field("location", &ExportSettings::location),
        field("descriptive", &ExportSettings::descriptive))
        .def_prop_ro(
            "develop_size",
            [](const ExportSettings& self)
                -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
                if (!self.resize) {
                    return std::nullopt;
                }
                return std::pair{self.width, self.height};
            },
            nb::sig("def develop_size(self) -> tuple[int, int] | None"),
            "The `size` to give `develop`: the box to fit as (width, height), or None for the "
            "photograph's own size.")
        .def(
            "canonical", [](const ExportSettings& self) { return canonical(self); },
            "The settings without what the format and size do not use: width, height and "
            "enlarging are 0 and False when not resizing, and 16 bits is off for JPEG. Two "
            "settings that export alike have one canonical form.")
        .def(
            "validate", [](const ExportSettings& self) { validate(self); },
            "Raise ValueError unless the settings can be exported: quality and sharpening 0 to "
            "100, a box of 1 to 99999 on each side when resizing.");

    nb::enum_<ExportPresetProblem>(m, "ExportPresetProblem",
                                   "Why a change to the export presets did not happen.")
        .value("INVALID_NAME", ExportPresetProblem::InvalidName,
               "The name is empty, too long or holds control characters.")
        .value("NAME_TAKEN", ExportPresetProblem::NameTaken,
               "Another preset already has the new name.")
        .value("NOT_FOUND", ExportPresetProblem::NotFound, "No preset has the name.")
        .value("UNUSABLE", ExportPresetProblem::Unusable,
               "The preset is one this arraw cannot use.")
        .value("NEWER_FILE", ExportPresetProblem::NewerFile,
               "A newer arraw wrote the file, so it is never written.")
        .value("UNREADABLE_FILE", ExportPresetProblem::UnreadableFile,
               "The file cannot be read, so it is never written over.")
        .value("BUSY", ExportPresetProblem::Busy,
               "Another process is changing the file and did not finish in time.")
        .value("WRITE_FAILED", ExportPresetProblem::WriteFailed,
               "The file or its directory could not be written; the old file stays.")
        .value("BUILT_IN", ExportPresetProblem::BuiltIn,
               "The name is a built-in preset's, which cannot be changed.")
        .value("NOT_UNREADABLE", ExportPresetProblem::NotUnreadable,
               "The file can be read, so it is not set aside: only an unreadable one is.");

    nb::enum_<ExportPresetFileState>(m, "ExportPresetFileState",
                                     "What reading the export presets file found.")
        .value("MISSING", ExportPresetFileState::Missing, "There is no file.")
        .value("READ", ExportPresetFileState::Read, "The file was read, empty or not.")
        .value("NEWER", ExportPresetFileState::Newer,
               "A newer arraw wrote it: read as far as understood, never written.")
        .value("UNREADABLE", ExportPresetFileState::Unreadable,
               "It cannot be read: no presets from it, and it is never written over.");

    exportPresetErrorType = PyErr_NewExceptionWithDoc(
        "arraw._arraw.ExportPresetError",
        "A change to the export presets that could not be made. `problem` says why "
        "(ExportPresetProblem); the message names the file.",
        PyExc_RuntimeError, nullptr);
    if (exportPresetErrorType == nullptr) {
        throw nb::python_error();
    }
    m.attr("ExportPresetError") = nb::steal(exportPresetErrorType); // the module owns it now
    nb::register_exception_translator(&translateExportPresetError);

    nb::class_<ExportPreset>(
        m, "ExportPreset",
        "A named, whole set of export settings. `builtin` presets are defined by arraw and "
        "read-only; `shadowed` is a preset of the file whose name a built-in also has, which "
        "the built-in hides.")
        .def(
            "__init__",
            [](ExportPreset* self, const std::string& name,
               const std::optional<ExportSettings>& settings) {
                new (self)
                    ExportPreset{.name = name, .settings = settings.value_or(ExportSettings{})};
            },
            "name"_a, "settings"_a.none() = nb::none(),
            nb::sig("def __init__(self, name: str, settings: ExportSettings | None = None) -> "
                    "None"),
            "A preset for saving; `settings` default to those of a new ExportSettings.")
        .def_ro("name", &ExportPreset::name)
        .def_ro("settings", &ExportPreset::settings)
        .def_ro("builtin", &ExportPreset::builtIn)
        .def_ro("shadowed", &ExportPreset::shadowed)
        .def(nb::self == nb::self)
        .def("__hash__",
             [](const ExportPreset& self) {
                 return nb::hash(nb::make_tuple(self.name, nb::cast(self.settings), self.builtIn,
                                                self.shadowed));
             })
        .def("__repr__",
             [](const ExportPreset& self) {
                 return "ExportPreset(name=" + std::string(nb::repr(nb::cast(self.name)).c_str()) +
                        ", settings=" + std::string(nb::repr(nb::cast(self.settings)).c_str()) +
                        ", builtin=" + (self.builtIn ? "True" : "False") +
                        ", shadowed=" + (self.shadowed ? "True" : "False") + ")";
             })
        .def(
            "to_json", [](const ExportPreset& self) { return exportPresetToJson(self); },
            "The preset as the presets file holds it and `arraw-cli export-preset show` prints "
            "it: a JSON object, every key written. Raises ValueError for a name or settings "
            "that are not valid.");

    nb::class_<UnusableExportPreset>(
        m, "UnusableExportPreset",
        "A preset in the file that this arraw cannot use, kept in the file as it is.")
        .def_ro("name", &UnusableExportPreset::name, "Its name as written, empty when it has none.")
        .def_ro("reason", &UnusableExportPreset::reason, "Why it cannot be used, as a phrase.")
        .def("__repr__", [](const UnusableExportPreset& self) {
            return "UnusableExportPreset(name=" +
                   std::string(nb::repr(nb::cast(self.name)).c_str()) +
                   ", reason=" + std::string(nb::repr(nb::cast(self.reason)).c_str()) + ")";
        });

    nb::class_<ExportPresetStore>(
        m, "ExportPresetStore",
        "The export presets file of one directory, read and changed safely. Reading takes no "
        "lock and creates nothing. Each change takes a lock file, re-reads the file and "
        "replaces it atomically, so that the app, the command line and scripts lose none of "
        "each other's changes. A file written by a newer arraw, or one that cannot be read, is "
        "never written. The built-in presets come first in `presets()` and cannot be changed. "
        "Changes raise ExportPresetError.")
        .def(
            "__init__",
            [](ExportPresetStore* self, const std::optional<std::filesystem::path>& directory) {
                new (self) ExportPresetStore(directory ? *directory : configDirectory());
            },
            "directory"_a.none() = nb::none(),
            nb::sig("def __init__(self, directory: str | os.PathLike | None = None) -> None"),
            "A store for the presets file of `directory`, which need not exist; None is the "
            "configuration directory (config_directory(), which ARRAW_CONFIG_DIR overrides).")
        .def_prop_ro("file", &ExportPresetStore::file,
                     "Path of the presets file, which may not exist.")
        .def(
            "presets",
            [](const ExportPresetStore& self) {
                PythonLog log;
                return withoutGil([&] { return self.read(log).presets; });
            },
            "The usable presets: the built-in ones first, in their order, then the file's, "
            "sorted by name. Problems of the file are logged on the 'arraw' logger.")
        .def(
            "unusable",
            [](const ExportPresetStore& self) {
                PythonLog log;
                return withoutGil([&] { return self.read(log).unusable; });
            },
            "The entries of the file that this arraw cannot use.")
        .def(
            "state",
            [](const ExportPresetStore& self) {
                return withoutGil([&] { return self.read().state; });
            },
            "What the file is: ExportPresetFileState.MISSING, READ, NEWER or UNREADABLE.")
        .def(
            "load",
            [](const ExportPresetStore& self, std::string_view name) {
                PythonLog log;
                std::optional<ExportPreset> found =
                    withoutGil([&] { return self.load(name, log); });
                if (!found) {
                    throw ExportPresetError(ExportPresetProblem::NotFound,
                                            "no export preset is named '" + std::string(name) +
                                                "' in " + self.file().string());
                }
                return *found;
            },
            "name"_a,
            "Find a preset by name, case-insensitively; a built-in wins over a preset of the "
            "file that has its name. Raises ExportPresetError with NOT_FOUND when there is none, "
            "or UNUSABLE when the file's preset of that name cannot be used by this arraw.")
        .def(
            "save",
            [](ExportPresetStore& self, const ExportPreset& preset) {
                withoutGil([&] { self.save(preset); });
            },
            "preset"_a,
            "Add a preset, or replace the one whose name folds alike (the new spelling is kept). "
            "Raises ExportPresetError (INVALID_NAME, BUILT_IN, UNUSABLE, NEWER_FILE, "
            "UNREADABLE_FILE, BUSY or WRITE_FAILED), or ValueError for invalid settings.")
        .def(
            "rename",
            [](ExportPresetStore& self, std::string_view from, std::string_view to) {
                withoutGil([&] { self.rename(from, to); });
            },
            "old"_a, "new"_a,
            "Give a preset another name; a change of letter case only is allowed. Raises "
            "ExportPresetError (NOT_FOUND, NAME_TAKEN and the problems of save).")
        .def(
            "delete",
            [](ExportPresetStore& self, std::string_view name) {
                withoutGil([&] { self.remove(name); });
            },
            "name"_a,
            "Remove a preset, usable or not. Raises ExportPresetError with NOT_FOUND when there "
            "is none, or BUILT_IN for a built-in one.")
        .def(
            "set_aside",
            [](ExportPresetStore& self) { return withoutGil([&] { return self.setAside(); }); },
            "Move the presets file to `export-presets.unreadable-<date>.json` beside it, so that "
            "the next change starts a new file, and return the new path. Never replaces a file. "
            "Only "
            "an unreadable file is moved: raises ExportPresetError with NOT_FOUND when there is "
            "none, NEWER_FILE for a newer arraw's, NOT_UNREADABLE for one that reads fine.");

    m.def(
        "config_directory", [] { return configDirectory(); },
        "The directory where arraw keeps its shared configuration: ARRAW_CONFIG_DIR when set, "
        "else `arraw` under the user's generic configuration location. Nothing is created. "
        "Raises RuntimeError when there is none.");
}

} // namespace arraw::python
