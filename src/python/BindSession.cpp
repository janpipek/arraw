#include "PyBindings.h"

#include <ColorEncoding.h>
#include <DevelopState.h>
#include <EditSession.h>
#include <LensDatabase.h>
#include <LensResolution.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <PhotoMarks.h>

#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace arraw::python {

namespace {

/// @brief Context manager of one edit: begins on entry, commits on a normal exit, cancels on an
/// exception.
///
/// Holds the Python session object, so the session lives as long as the scope does.
struct EditScope {
    /// @brief Session the edit belongs to.
    nb::object session;

    /// @brief Kind of change the step is.
    EditOrigin origin = EditOrigin::Edit;

    /// @brief What the origin applies to.
    std::string detail;
};

} // namespace

void bindSession(nb::module_& module) {
    nb::enum_<EditOrigin>(module, "EditOrigin",
                          "Kind of change that made a history step, beyond what its states show.")
        .value("OPENED", EditOrigin::Opened)
        .value("EDIT", EditOrigin::Edit)
        .value("PASTE", EditOrigin::Paste)
        .value("PRESET", EditOrigin::Preset)
        .value("RESET", EditOrigin::Reset)
        .value("CROP", EditOrigin::Crop)
        .value("RESTORE", EditOrigin::Restore);

    nb::class_<HistoryStep>(module, "HistoryStep",
                            "One entry of an edit history: the state it left, and why.")
        .def_ro("state", &HistoryStep::state, "Whole develop state after the step.")
        .def_ro("origin", &HistoryStep::origin, "Kind of change the step was.")
        .def_ro("detail", &HistoryStep::detail,
                "What the origin applies to, such as a preset's name; empty otherwise.")
        .def_ro("negative", &HistoryStep::negative, "Negative settings after the step.")
        .def_prop_ro(
            "lens", [](const HistoryStep& step) { return step.negative.lens; },
            "Lens settings after the step; the same as `negative.lens`.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const HistoryStep& step) {
            return "HistoryStep(state=" + reprValue(step.state) +
                   ", origin=" + reprValue(step.origin) + ", detail=" + reprValue(step.detail) +
                   ", negative=" + reprValue(step.negative) + ")";
        });

    nb::class_<ChangeDescription>(module, "ChangeDescription",
                                  "Settings two develop states differ in, for naming a step.")
        .def_prop_ro(
            "keys", [](const ChangeDescription& change) { return change.keys; },
            "Descriptor keys (camelCase, as JSON and the sidecar spell them) whose values differ, "
            "in "
            "table order; setting_descriptors() maps them to the Python names.")
        .def_ro("group", &ChangeDescription::group,
                "Group every key belongs to; None when there are no keys or they span groups.")
        .def_prop_ro(
            "negative", [](const ChangeDescription& change) { return change.negative; },
            "Keys of the negative settings that differ, in the order 'demosaic', "
            "'lensDistortion', 'lensVignetting', 'lensLateralCa' and 'lens' (the recorded lens); "
            "empty unless made by describe_change over two history steps.")
        .def_prop_ro(
            "lens",
            [](const ChangeDescription& change) {
                std::vector<std::string> keys;
                for (const std::string_view key : change.negative) {
                    if (key.starts_with("lens")) {
                        keys.emplace_back(key);
                    }
                }
                return keys;
            },
            "The lens keys of `negative`: which corrections were switched and whether the "
            "recorded lens changed.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const ChangeDescription& change) {
            return "ChangeDescription(keys=" + reprValue(change.keys) +
                   ", group=" + reprValue(change.group) +
                   ", negative=" + reprValue(change.negative) + ")";
        });

    module.def("describe_change",
               static_cast<ChangeDescription (*)(const DevelopState&, const DevelopState&)>(
                   &describeChange),
               "before"_a, "after"_a, "List the settings two develop states differ in.");
    module.def(
        "describe_change",
        static_cast<ChangeDescription (*)(const HistoryStep&, const HistoryStep&)>(&describeChange),
        "before"_a, "after"_a,
        "List what two history steps differ in: the develop state, and the negative "
        "settings in `negative`.");

    nb::class_<EditScope>(module, "EditScope",
                          "Context manager of one edit; made by EditSession.edit().")
        .def("__enter__",
             [](nb::handle self) {
                 auto& scope = nb::cast<EditScope&>(self);
                 auto& session = nb::cast<EditSession&>(scope.session);
                 if (session.editing()) {
                     throw std::logic_error("An edit is already open");
                 }
                 session.begin();
                 return nb::borrow(self);
             })
        .def(
            "__exit__",
            [](EditScope& scope, const nb::object& type, const nb::object&, const nb::object&) {
                auto& session = nb::cast<EditSession&>(scope.session);
                if (!session.editing()) {
                    return false; // the block closed the edit itself
                }
                if (type.is_none()) {
                    session.commit(scope.origin, scope.detail);
                } else {
                    session.cancel();
                }
                return false; // never swallows the exception
            },
            "type"_a.none(), "value"_a.none(), "traceback"_a.none());

    nb::class_<EditSession>(module, "EditSession",
                            "One photograph being edited: the document as it stands, and how it "
                            "got there.")
        .def(
            "__init__",
            [](EditSession* self, Photo photo, const LensDatabase* database) {
                // None is the process's database, but only a RAW has lens corrections to record.
                if (database == nullptr &&
                    !std::holds_alternative<NamedEncoding>(photo.metadata().encoding)) {
                    database = withoutGil([] { return &LensDatabase::system(); });
                }
                new (self) EditSession(std::move(photo), database);
            },
            "photo"_a, "lens_database"_a.none() = nb::none(), nb::keep_alive<1, 3>(),
            "Start editing `photo`. `lens_database` is the database the session records lens "
            "matches in (None: the process's, loaded on first use, for a RAW only; other files "
            "have none). It is kept alive as long as the session.")
        .def_prop_ro("lens_database", &EditSession::lensDatabase, nb::rv_policy::reference,
                     nb::sig("def lens_database(self) -> LensDatabase | None"),
                     "The lens database the session records matches in, or None (not a RAW).")
        .def_prop_ro("photo", &EditSession::photo, nb::rv_policy::copy,
                     "Current photograph, including an edit in progress.")
        .def("begin", &EditSession::begin, "Open an edit; one already open is committed first.")
        .def("update", &EditSession::update, "state"_a,
             "Change the state provisionally, inside the open edit.")
        .def("commit", &EditSession::commit, "origin"_a = EditOrigin::Edit, "detail"_a = "",
             "Close the open edit as one history step; an edit that ends where it began leaves "
             "none.")
        .def("cancel", &EditSession::cancel, "Close the open edit, restoring its starting state.")
        .def_prop_ro("editing", &EditSession::editing, "Whether an edit is open.")
        .def(
            "set_state",
            [](EditSession& session, DevelopState state, EditOrigin origin, std::string detail,
               const std::optional<NegativeSettings>& negative) {
                if (negative) {
                    session.setState(*negative, std::move(state), origin, std::move(detail));
                } else {
                    session.setState(std::move(state), origin, std::move(detail));
                }
            },
            "state"_a, "origin"_a = EditOrigin::Edit, "detail"_a = "", nb::kw_only(),
            "negative"_a = nb::none(),
            "Replace the develop state as one history step. With `negative`, the negative "
            "settings change in the same step (a paste or a preset); None leaves them.")
        .def("set_negative", &EditSession::setNegative, "negative"_a, "origin"_a = EditOrigin::Edit,
             "detail"_a = "",
             "Replace the negative settings as one history step; equal ones leave none. An open "
             "edit is committed first.")
        .def("update_negative", &EditSession::updateNegative, "negative"_a,
             "Change the negative settings provisionally, inside the open edit, so that a drag "
             "of a lens amount is one history step on commit. RuntimeError when no edit is open "
             "and ValueError when the settings are invalid, with nothing changed.")
        .def(
            "set_lens_corrections",
            [](EditSession& session, std::optional<bool> distortion,
               const nb::typed<nb::object, float>& vignettingAmount, std::optional<bool> lateralCa,
               EditOrigin origin, std::string detail) {
                NegativeSettings negative = session.photo().negative();
                negative.lens.distortion = distortion.value_or(negative.lens.distortion);
                if (!vignettingAmount.is_none()) {
                    negative.lens.vignettingAmount =
                        convertValue<float>(vignettingAmount, "vignetting_amount");
                }
                negative.lens.lateralCa = lateralCa.value_or(negative.lens.lateralCa);
                session.setNegative(std::move(negative), origin, std::move(detail));
            },
            nb::kw_only(), "distortion"_a = nb::none(), "vignetting_amount"_a = nb::none(),
            "lateral_ca"_a = nb::none(), "origin"_a = EditOrigin::Edit, "detail"_a = "",
            "Set the lens corrections given (None keeps one) as one history step; "
            "`vignetting_amount` is a number from 0 to 150 (100 is the profile; a bool is "
            "refused). A correction turned on for a photograph with no recorded lens records "
            "the lens matched from its EXIF in the same step, in the session's `lens_database`, "
            "so undo takes both away; switching off keeps the record. Equal settings leave no "
            "step.")
        .def(
            "choose_lens",
            [](EditSession& session, const std::string& maker, const std::string& model,
               const LensDatabase* database, EditOrigin origin, std::string detail) {
                if (database == nullptr) {
                    database = session.lensDatabase();
                }
                const auto chosen = withoutGil([&] {
                    return withChosenLens(session.photo(), LensIdentity{maker, model},
                                          lensDatabaseOr(database));
                });
                if (!chosen) {
                    throw std::invalid_argument("'" + maker + "|" + model +
                                                "' is not in the lens database, or the "
                                                "photograph is not a RAW");
                }
                session.setNegative(chosen->negative(), origin, std::move(detail));
            },
            "maker"_a, "model"_a, nb::arg("database").none() = nb::none(),
            "origin"_a = EditOrigin::Edit, "detail"_a = "",
            "Record the lens the photographer chose, as one history step; ValueError when the "
            "database (default: the session's, else the process's) does not list it. The "
            "switches stay as they are.")
        .def(
            "reset_lens",
            [](EditSession& session, EditOrigin origin, std::string detail) {
                NegativeSettings negative = session.photo().negative();
                negative.lens.lens.reset();
                session.setNegative(std::move(negative), origin, std::move(detail));
            },
            "origin"_a = EditOrigin::Edit, "detail"_a = "",
            "Forget the recorded lens, as one history step; the session matches the file's EXIF "
            "again when a correction is on (\"reset to automatic\").")
        .def_prop_ro("history", &EditSession::history, nb::rv_policy::copy,
                     "Steps taken so far, the opening state first; a new list on every access.")
        .def_prop_ro("position", &EditSession::position,
                     "Index in history of the step the document is at.")
        .def("go_to", &EditSession::goTo, "index"_a,
             "Move to a step, keeping every step. An open edit is committed first, and index "
             "is taken after that.")
        .def_prop_ro("can_undo", &EditSession::canUndo)
        .def_prop_ro("can_redo", &EditSession::canRedo)
        .def("undo", &EditSession::undo, "Take back the latest history step.")
        .def("redo", &EditSession::redo, "Bring back the step undo took back.")
        .def_prop_ro("saved", &EditSession::saved, nb::rv_policy::copy,
                     "Photograph as its sidecar holds it.")
        .def_prop_ro("has_unsaved_changes", &EditSession::hasUnsavedChanges)
        .def("save", &EditSession::save,
             "Write the photograph to its sidecar and make that the saved state.")
        .def("set_marks", &EditSession::setMarks, "marks"_a,
             "Set the marks, writing them to the sidecar at once.")
        .def("discard_changes", &EditSession::discardChanges,
             "Return to the saved state, dropping history and any open edit.")
        .def(
            "edit",
            [](nb::handle self, EditOrigin origin, std::string detail) {
                return EditScope{nb::borrow(self), origin, std::move(detail)};
            },
            "origin"_a = EditOrigin::Edit, "detail"_a = "",
            "Context manager for one edit: commits on a normal exit, cancels if the block "
            "raises. Entering raises RuntimeError while an edit is open; leaving does nothing "
            "if the block closed the edit itself.");
}

} // namespace arraw::python
