#include "PyBindings.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <Photo.h>
#include <PhotoMarks.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

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
        .value("CROP", EditOrigin::Crop);

    nb::class_<HistoryStep>(module, "HistoryStep",
                            "One entry of an edit history: the state it left, and why.")
        .def_ro("state", &HistoryStep::state, "Whole develop state after the step.")
        .def_ro("origin", &HistoryStep::origin, "Kind of change the step was.")
        .def_ro("detail", &HistoryStep::detail,
                "What the origin applies to, such as a preset's name; empty otherwise.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const HistoryStep& step) {
            return "HistoryStep(state=" + reprValue(step.state) +
                   ", origin=" + reprValue(step.origin) + ", detail=" + reprValue(step.detail) +
                   ")";
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
        .def(nb::self == nb::self)
        .def("__repr__", [](const ChangeDescription& change) {
            return "ChangeDescription(keys=" + reprValue(change.keys) +
                   ", group=" + reprValue(change.group) + ")";
        });

    module.def("describe_change", &describeChange, "before"_a, "after"_a,
               "List the settings two develop states differ in.");

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
        .def(nb::init<Photo>(), "photo"_a)
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
        .def("set_state", &EditSession::setState, "state"_a, "origin"_a = EditOrigin::Edit,
             "detail"_a = "", "Replace the develop state as one history step.")
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
