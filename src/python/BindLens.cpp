#include "PyBindings.h"

#include <ColorEncoding.h>
#include <Diagnostics.h>
#include <LensCorrection.h>
#include <LensDatabase.h>
#include <LensFacts.h>
#include <LensMatch.h>
#include <LensProblem.h>
#include <LensResolution.h>
#include <NegativeSettings.h>
#include <Photo.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace arraw::python {

namespace {

/// @brief Tells whether a photograph can carry lens corrections: a RAW, whatever the file.
bool carriesLens(const Photo& photo) {
    return !std::holds_alternative<NamedEncoding>(photo.metadata().encoding);
}

/// @brief Turns a table into a tuple of floats, or an empty tuple for none.
nb::tuple tupleOf(const std::vector<float>& table) {
    nb::list items;
    for (const float value : table) {
        items.append(static_cast<double>(value));
    }
    return nb::tuple(items);
}

} // namespace

const LensDatabase& lensDatabaseOr(const LensDatabase* database) {
    return database != nullptr ? *database : LensDatabase::system();
}

LensCorrection lensCorrectionOf(const Photo& photo, const LensDatabase* database) {
    PythonLog log;
    return database != nullptr ? lensCorrectionFor(photo, *database, log)
                               : lensCorrectionFor(photo, log);
}

void bindLens(nb::module_& m) {
    m.def("lensfun_available", &lensfunAvailable,
          "Whether this build can read a lens database at all (it was built with lensfun).");
    m.def(
        "lensfun_version", [] { return std::string(lensfunVersion()); },
        "The lensfun this build was compiled against, such as '0.3.4'; empty without lensfun.");

    nb::enum_<LensOrigin>(m, "LensOrigin",
                          "How the lens recorded in a photograph's lens settings was chosen; "
                          "str() is the token the sidecar uses.")
        .value("MATCHED", LensOrigin::Matched, "Found from the file's EXIF.")
        .value("CHOSEN", LensOrigin::Chosen, "Picked by the photographer.")
        .def("__str__", [](LensOrigin origin) { return std::string(tokenOf(origin)); });

    nb::enum_<LensProblem>(m, "LensProblem", "Why a lens could not be found or loaded.")
        .value("LENS_UNIDENTIFIED", LensProblem::LensUnidentified)
        .value("LENS_FOCAL_UNKNOWN", LensProblem::LensFocalUnknown)
        .value("CAMERA_NOT_IN_DATABASE", LensProblem::CameraNotInDatabase)
        .value("LENS_NOT_IN_DATABASE", LensProblem::LensNotInDatabase)
        .value("LENS_AMBIGUOUS", LensProblem::LensAmbiguous)
        .value("LENS_PROFILE_MISSING", LensProblem::LensProfileMissing)
        .value("LENS_DATABASE_CHANGED", LensProblem::LensDatabaseChanged)
        .value("LENS_DATABASE_MISSING", LensProblem::LensDatabaseMissing)
        .value("LENSFUN_UNAVAILABLE", LensProblem::LensfunUnavailable);

    nb::enum_<CropSource>(m, "CropSource", "Where the crop factor of a lens match came from.")
        .value("CAMERA", CropSource::Camera, "The camera listed in the database.")
        .value("FOCAL_RATIO", CropSource::FocalRatio,
               "The 35 mm focal length over the focal length.")
        .value("LENS", CropSource::Lens, "The lens's own, when nothing else says.");

    bindFrozen<LensIdentity, false>(
        m, "LensIdentity", "A lens by name, exactly as lensfun spells its maker and model.",
        field("maker", &LensIdentity::maker), field("model", &LensIdentity::model));

    bindFrozen<RecordedLens>(
        m, "RecordedLens",
        "The lens a photograph's corrections were found for, by name only: lensfun's maker and "
        "model, the crop factor of the image as the match used it, how the lens was chosen, and "
        "the stamp of the database at the time. No calibration is recorded.",
        field("maker", &RecordedLens::maker), field("model", &RecordedLens::model),
        field("crop_factor", &RecordedLens::cropFactor), field("origin", &RecordedLens::origin),
        field("database", &RecordedLens::database));

    bindFrozen<LensSettings>(
        m, "LensSettings",
        "Which lens corrections a photograph asks for (all off by default), and the lens recorded "
        "for them, None until one is first turned on. They apply to a RAW only.",
        field("distortion", &LensSettings::distortion),
        field("vignetting", &LensSettings::vignetting),
        field("lateral_ca", &LensSettings::lateralCa), field("lens", &LensSettings::lens))
        .def_prop_ro("any", &LensSettings::any, "Whether any correction is asked for.");

    bindFrozen<LensFacts>(
        m, "LensFacts",
        "What a RAW says about its camera and lens, for a profile to be found by; empty for "
        "any other file.",
        field("camera_maker", &LensFacts::cameraMaker),
        field("camera_model", &LensFacts::cameraModel), field("lens_names", &LensFacts::lensNames),
        field("min_focal", &LensFacts::minFocal), field("max_focal", &LensFacts::maxFocal),
        field("max_aperture", &LensFacts::maxAperture),
        field("focal_length", &LensFacts::focalLength), field("aperture", &LensFacts::aperture),
        field("focus_distance", &LensFacts::focusDistance),
        field("focal_length_35", &LensFacts::focalLength35));

    bindFrozen<CameraEntry>(
        m, "CameraEntry", "A camera as the lens database lists it.",
        field("maker", &CameraEntry::maker), field("model", &CameraEntry::model),
        field("mount", &CameraEntry::mount), field("crop_factor", &CameraEntry::cropFactor));

    bindFrozen<LensEntry>(
        m, "LensEntry", "A lens as the lens database lists it.", field("maker", &LensEntry::maker),
        field("model", &LensEntry::model), field("mounts", &LensEntry::mounts),
        field("crop_factor", &LensEntry::cropFactor), field("min_focal", &LensEntry::minFocal),
        field("max_focal", &LensEntry::maxFocal),
        field("widest_aperture", &LensEntry::widestAperture), field("score", &LensEntry::score));

    nb::class_<LensMatch>(m, "LensMatch",
                          "A lens found for a photograph, and what it was found with; not "
                          "constructible from Python.")
        .def_ro("lens", &LensMatch::lens, "The lens found.")
        .def_ro("name", &LensMatch::name, "The name from the photograph that found it.")
        .def_ro("score", &LensMatch::score, "lensfun's score for that name.")
        .def_ro("exact_name", &LensMatch::exactName,
                "Whether the name is the lens's whole name, not merely like it.")
        .def_ro("camera_listed", &LensMatch::cameraListed,
                "Whether the camera is in the database; informational when not.")
        .def_ro("crop_factor", &LensMatch::cropFactor,
                "Crop factor of the image, as the profile is applied with.")
        .def_ro("crop_source", &LensMatch::cropSource, "Where the crop factor came from.")
        .def_ro("focal_length", &LensMatch::focalLength,
                "Focal length to resolve the profile at, in millimetres.")
        .def_ro("focal_length_read", &LensMatch::focalLengthRead,
                "Whether the file recorded the focal length; a prime's own is assumed.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const LensMatch& match) {
            return "LensMatch(lens=" + reprValue(match.lens) + ", name=" + reprValue(match.name) +
                   ", crop_factor=" + reprValue(match.cropFactor) +
                   ", focal_length=" + reprValue(match.focalLength) + ")";
        });
    m.attr("LensMatch").attr("__hash__") = nb::none();

    bindFrozen<LensProvenance>(
        m, "LensProvenance",
        "Where one correction's numbers came from: the lens, the conditions it was resolved "
        "for, and whether the file recorded each or it was assumed.",
        field("lens", &LensProvenance::lens), field("crop_factor", &LensProvenance::cropFactor),
        field("focal_length", &LensProvenance::focalLength),
        field("focal_length_read", &LensProvenance::focalLengthRead),
        field("aperture", &LensProvenance::aperture),
        field("aperture_read", &LensProvenance::apertureRead),
        field("distance", &LensProvenance::distance),
        field("distance_read", &LensProvenance::distanceRead),
        field("database", &LensProvenance::database));

    nb::class_<LensCorrection>(
        m, "LensCorrection",
        "A lens's corrections resolved into radial tables, independent of resolution and of "
        "where the profile came from; not constructible from Python. The radius is 0 at the "
        "optical centre and 1 at the farthest corner, and each table holds the same number of "
        "values evenly spaced over it; an empty table means that correction is absent.")
        .def_prop_ro("active", &LensCorrection::active,
                     "Whether any correction is present, so whether the render has work.")
        .def_prop_ro(
            "distortion", [](const LensCorrection& c) { return tupleOf(c.distortion); },
            "Radial scale from a point of the corrected image to the sensor image, before the "
            "fill scale; empty for none.")
        .def_prop_ro(
            "red_scale", [](const LensCorrection& c) { return tupleOf(c.redScale); },
            "Scale of red's position against green's, by sensor radius; empty for none.")
        .def_prop_ro(
            "blue_scale", [](const LensCorrection& c) { return tupleOf(c.blueScale); },
            "Scale of blue's position against green's, by sensor radius; empty for none.")
        .def_prop_ro(
            "vignetting", [](const LensCorrection& c) { return tupleOf(c.vignetting); },
            "Gain that restores the light, by sensor radius; empty for none.")
        .def_ro("center_u", &LensCorrection::centerU,
                "Optical centre across, as a fraction of the width.")
        .def_ro("center_v", &LensCorrection::centerV,
                "Optical centre down, as a fraction of the height.")
        .def_ro("aspect", &LensCorrection::aspect,
                "Width over height of the frames the tables were sampled for.")
        .def_ro("fill", &LensCorrection::fill,
                "Zoom that keeps every sample of the corrected frame inside the sensor image.")
        .def_ro("distortion_from", &LensCorrection::distortionFrom,
                "Provenance of the distortion, or None.")
        .def_ro("lateral_ca_from", &LensCorrection::lateralCaFrom,
                "Provenance of the lateral chromatic aberration, or None.")
        .def_ro("vignetting_from", &LensCorrection::vignettingFrom,
                "Provenance of the vignetting, or None.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const LensCorrection& c) {
            return std::string("LensCorrection(active=") + (c.active() ? "True" : "False") +
                   ", distortion=" + (c.distortion.empty() ? "False" : "True") +
                   ", lateral_ca=" + (c.hasLateralCa() ? "True" : "False") +
                   ", vignetting=" + (c.vignetting.empty() ? "False" : "True") + ")";
        });
    m.attr("LensCorrection").attr("__hash__") = nb::none();

    nb::class_<LensDatabase>(
        m, "LensDatabase",
        "The lens and camera database, loaded once and read-only afterwards. One that could not "
        "be loaded is an object like any other, which finds nothing and says why in `problem`.")
        .def_static(
            "system",
            []() -> const LensDatabase* {
                return withoutGil([] { return &LensDatabase::system(); });
            },
            nb::rv_policy::reference,
            "The process's database, loaded on the first call (slow): beside the executable, "
            "else lensfun's own directories, the user's update replacing either when newer.")
        .def_static(
            "from_directory",
            [](const std::filesystem::path& directory) {
                return withoutGil([&] { return LensDatabase::fromDirectory(directory); });
            },
            "directory"_a,
            "Load the database of one directory of lensfun XML files, and optionally a "
            "timestamp.txt naming its age. Never raises: check `loaded` and `problem`.")
        .def_prop_ro("loaded", &LensDatabase::loaded, "Whether any database was loaded.")
        .def_prop_ro("problem", &LensDatabase::problem, "Why nothing was loaded, or None.")
        .def_prop_ro("directory", &LensDatabase::directory, "The directory it came from.")
        .def_prop_ro("stamp", &LensDatabase::stamp,
                     "Its age, as its newest timestamp: a UTC time; empty when nothing loaded.")
        .def_prop_ro("camera_count", &LensDatabase::cameraCount)
        .def_prop_ro("lens_count", &LensDatabase::lensCount)
        .def("find_camera", &LensDatabase::findCamera, "maker"_a, "model"_a,
             "Find a camera from its EXIF Make and Model, or None when it is not listed.")
        .def(
            "find_lenses",
            [](const LensDatabase& database, const std::string& name,
               const std::optional<std::string>& mount, const std::optional<double>& cropFactor) {
                return database.findLenses(name, {mount, cropFactor});
            },
            "name"_a, nb::kw_only(), "mount"_a = nb::none(), "crop_factor"_a = nb::none(),
            "The lenses a name as a photograph records it could mean, best first by lensfun's "
            "score, within the mount and crop factor given.")
        .def("lens", &LensDatabase::lens, "maker"_a, "model"_a, "crop_factor"_a = nb::none(),
             "Look a lens up by its exact lensfun name, or None. A name the database lists "
             "more than once is answered by the calibration for `crop_factor`.")
        .def("lenses", &LensDatabase::lenses, "search"_a = "", "mount"_a = nb::none(),
             "List lenses whose maker and model hold every word of `search` (any order and "
             "case), ordered by maker and model, within a mount if one is given.")
        .def("mount_of", &LensDatabase::mountOf, "maker"_a, "model"_a,
             "The mount of a camera, or None when it is not listed or has none.")
        .def("__repr__", [](const LensDatabase& database) {
            return std::string("LensDatabase(loaded=") + (database.loaded() ? "True" : "False") +
                   ", stamp=" + reprValue(database.stamp()) +
                   ", lenses=" + std::to_string(database.lensCount()) + ")";
        });
}

void bindLensMethods(nb::module_& m) {
    auto photo = nb::borrow<nb::class_<Photo>>(m.attr("Photo"));
    photo.def_prop_ro(
        "lens", [](const Photo& p) { return p.negative().lens; },
        "The lens settings of the negative; the same as `negative.lens`.");
    photo.def(
        "lens_correction",
        [](const Photo& p, const LensDatabase* database) {
            return withoutGil([&] { return lensCorrectionOf(p, database); });
        },
        nb::arg("database").none() = nb::none(),
        "Resolve the photograph's lens corrections as a render applies them: empty when none is "
        "on or the photograph is not a RAW, else the tables of the recorded lens (looked up by "
        "its exact name) or of the match of the file's EXIF. What cannot be resolved is logged "
        "on the 'arraw' logger and the correction skipped. `database` defaults to the "
        "process's, loaded only when a correction is on.");
    photo.def(
        "lens_match",
        [](const Photo& p, const LensDatabase* database) -> std::optional<LensMatch> {
            if (!carriesLens(p)) {
                return std::nullopt;
            }
            PythonLog log;
            return withoutGil([&] {
                return resolveLensMatch(p.negative().lens, p.metadata().lens,
                                        lensDatabaseOr(database), log, p.path());
            });
        },
        nb::arg("database").none() = nb::none(),
        "Find the lens the photograph's corrections are for, whether or not any is on: the "
        "recorded lens, else the match of the file's EXIF. None for a file that is not a RAW or "
        "when no lens is found (the reason is logged on the 'arraw' logger).");
    photo.def(
        "with_lens_corrections",
        [](const Photo& p, std::optional<bool> distortion, std::optional<bool> vignetting,
           std::optional<bool> lateralCa) {
            if (!carriesLens(p)) {
                return p;
            }
            NegativeSettings negative = p.negative();
            negative.lens.distortion = distortion.value_or(negative.lens.distortion);
            negative.lens.vignetting = vignetting.value_or(negative.lens.vignetting);
            negative.lens.lateralCa = lateralCa.value_or(negative.lens.lateralCa);
            return p.with(std::move(negative));
        },
        nb::kw_only(), "distortion"_a = nb::none(), "vignetting"_a = nb::none(),
        "lateral_ca"_a = nb::none(),
        "Return a photograph with the lens switches given set, as `--lens-corrections` sets "
        "them; None keeps one as it is, and the recorded lens is untouched (see "
        "record_lens_match). A photograph that is not a RAW is returned as it was.");
    photo.def(
        "with_chosen_lens",
        [](const Photo& p, const std::string& maker, const std::string& model,
           const LensDatabase* database) {
            const auto chosen = withoutGil([&] {
                return withChosenLens(p, LensIdentity{maker, model}, lensDatabaseOr(database));
            });
            if (!chosen) {
                throw std::invalid_argument("'" + maker + "|" + model +
                                            "' is not in the lens database, or the photograph "
                                            "is not a RAW");
            }
            return *chosen;
        },
        "maker"_a, "model"_a, nb::arg("database").none() = nb::none(),
        "Return a photograph with the lens recorded as chosen by the photographer, as "
        "`--lens 'Maker|Model'` does: looked up by its exact lensfun name, with the camera's "
        "crop factor. ValueError when the database does not list it or the photograph is not "
        "a RAW.");
    m.def(
        "record_lens_match",
        [](const Photo& p, const LensDatabase* database) {
            if (!p.negative().lens.any() || p.negative().lens.lens) {
                return p; // nothing to record, so the database is not even loaded
            }
            return withoutGil([&] { return recordLensMatch(p, lensDatabaseOr(database)); });
        },
        "photo"_a, nb::arg("database").none() = nb::none(),
        "Record the lens matched from the file's EXIF in a photograph that has a correction on "
        "and no lens recorded, with its crop factor and the database's stamp, so that a "
        "database update cannot move the edit to another lens. The photograph is returned as "
        "it was when nothing is on, a lens is recorded already, it is not a RAW, or no lens "
        "matches.");
}

} // namespace arraw::python
