#include "PyBindings.h"

#include <ImageImport.h>
#include <MarksFilter.h>
#include <PhotoMarks.h>
#include <Shot.h>

#include <filesystem>

namespace arraw::python {

void bindShots(nb::module_& m) {
    bindFrozen<Shot>(
        m, "Shot",
        "One capture: the file that is developed (the RAW when there is one) and the standard "
        "images of the same capture, which a RAW+JPEG camera writes beside it.",
        field("primary", &Shot::primary), field("companions", &Shot::companions))
        .def_prop_ro("format_label", &formatLabel,
                     "The formats of the shot, each once, joined by '+': 'ARW', 'JPEG', "
                     "'ARW+JPEG'.");

    bindFrozen<MarksFilter>(
        m, "MarksFilter",
        "Which culling marks a photograph must have: at least min_rating stars (which excludes "
        "rejects and unrated ones), or rejects_only (not both), and any one of labels (empty for "
        "any). The two dimensions combine by AND.",
        field("min_rating", &MarksFilter::minRating),
        field("rejects_only", &MarksFilter::rejectsOnly), field("labels", &MarksFilter::labels))
        .def_prop_ro("is_active", &MarksFilter::isActive, "Whether the filter narrows anything.")
        .def(
            "matches",
            [](const MarksFilter& self, const PhotoMarks& marks) { return self.matches(marks); },
            "marks"_a,
            "Whether a photograph's marks pass the filter. Raises ValueError for a filter that "
            "wants rejects_only and a min_rating, or a min_rating outside 0 to 5.");

    m.def(
        "group_shots",
        [](std::vector<std::filesystem::path> paths) {
            return withoutGil([&] { return groupShots(std::move(paths)); });
        },
        "paths"_a,
        "Group files into shots, without touching the filesystem: same folder and same stem "
        "(case-insensitive), the RAW as primary and the JPEG, PNG and TIFF files as its "
        "companions. Files without a RAW partner, and every file of a stem two RAWs share, stand "
        "alone; unsupported files and sidecars are dropped. In natural order of the primary's "
        "name (IMG_2 before IMG_10).");

    m.def(
        "list_shots",
        [](const std::filesystem::path& folder) {
            return withoutGil([&] { return listShots(folder); });
        },
        "folder"_a,
        "List the shots of a folder (not its subfolders, not hidden files), in natural order. "
        "Raises if the folder cannot be read.");

    m.def(
        "is_supported_image",
        [](const std::filesystem::path& path) { return isSupportedImage(path); }, "path"_a,
        "Whether a path names a photograph arraw opens, by its extension alone.");

    const auto extensions = supportedImageExtensions();
    const nb::object list =
        nb::cast(std::vector<std::string>(extensions.begin(), extensions.end()));
    m.attr("SUPPORTED_EXTENSIONS") = nb::steal(PySequence_Tuple(list.ptr()));
}

} // namespace arraw::python
