#include "PyBindings.h"

#include <Photo.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace arraw::python {
namespace {

/// @brief Python types of the snapshot and sidecar errors; the module holds the references.
PyObject* revisionConflictType = nullptr;
PyObject* baseMissingType = nullptr;
PyObject* lockTimeoutType = nullptr;

/// @brief Turns the snapshot and lock errors of core into their Python exceptions.
void translateSnapshotError(const std::exception_ptr& pointer, void*) {
    try {
        std::rethrow_exception(pointer);
    } catch (const SnapshotRevisionConflict& error) {
        nb::object value = nb::borrow(revisionConflictType)(error.what());
        value.attr("expected") = nb::int_(error.expected);
        value.attr("actual") = nb::int_(error.actual);
        PyErr_SetObject(revisionConflictType, value.ptr());
    } catch (const SnapshotBaseMissing& error) {
        nb::object value = nb::borrow(baseMissingType)(error.what());
        value.attr("base") = nb::cast(error.base);
        PyErr_SetObject(baseMissingType, value.ptr());
    } catch (const SidecarLockTimeout& error) {
        PyErr_SetString(lockTimeoutType, error.what());
    }
}

/// @brief Makes a Python exception class and adds it to the module.
PyObject* defineError(nb::module_& m, const char* name, const char* doc, PyObject* base) {
    PyObject* type = PyErr_NewExceptionWithDoc((std::string("arraw._arraw.") + name).c_str(), doc,
                                               base, nullptr);
    if (type == nullptr) {
        throw nb::python_error();
    }
    Py_INCREF(type); // one reference for the module, one for the translator
    m.attr(name) = nb::steal(type);
    return type;
}

/// @brief Reads a snapshot ID from a Python int: an index, no bool, from 1 to 2^64 - 1.
/// @throws nb::type_error if @p value is no int, std::invalid_argument if it is out of range.
std::uint64_t idFrom(nb::handle value) {
    if (nb::isinstance<nb::bool_>(value) || !PyIndex_Check(value.ptr())) {
        throw nb::type_error("'value': a snapshot ID is an int");
    }
    nb::object index = nb::steal(PyNumber_Index(value.ptr()));
    if (!index.is_valid()) {
        throw nb::python_error();
    }
    const unsigned long long number = PyLong_AsUnsignedLongLong(index.ptr());
    if (number == static_cast<unsigned long long>(-1) && PyErr_Occurred() != nullptr) {
        PyErr_Clear();
        throw std::invalid_argument("a snapshot ID is a whole number from 1 to 2**64 - 1");
    }
    if (number == 0) {
        throw std::invalid_argument("0 is no snapshot; IDs start at 1");
    }
    return number;
}

/// @brief Spells a time as a timezone-aware datetime in UTC.
nb::object datetimeOf(std::chrono::sys_seconds time) {
    // Not fromtimestamp: Windows refuses the years past 3000 that the model allows.
    const nb::module_ datetime = nb::module_::import_("datetime");
    const nb::object epoch =
        datetime.attr("datetime")(1970, 1, 1, "tzinfo"_a = datetime.attr("timezone").attr("utc"));
    return epoch + datetime.attr("timedelta")("seconds"_a = time.time_since_epoch().count());
}

} // namespace

void bindSnapshots(nb::module_& m) {
    revisionConflictType = defineError(
        m, "SnapshotRevisionConflict",
        "A snapshot change that was refused because the sidecar's snapshots are no longer the "
        "ones the session saw (another process changed them). `expected` is the revision the "
        "session saw and `actual` the one the sidecar holds. The session has read the "
        "snapshots again; nothing was changed, and the call is not retried.",
        PyExc_RuntimeError);
    baseMissingType = defineError(
        m, "SnapshotBaseMissing",
        "A save whose base names no snapshot of the sidecar (another process deleted it). "
        "`base` is the SnapshotId; nothing was written.",
        PyExc_ValueError);
    lockTimeoutType = defineError(
        m, "SidecarLockTimeout",
        "Another writer held the sidecar's lock for too long (about two seconds); nothing was "
        "written. Try again.",
        PyExc_RuntimeError);
    nb::register_exception_translator(&translateSnapshotError);

    nb::class_<SnapshotId>(m, "SnapshotId",
                           "Identity of a snapshot within one photograph: a whole number from 1, "
                           "never reused after the snapshot is deleted.")
        .def(
            "__init__",
            [](SnapshotId* self, const nb::object& value) { new (self) SnapshotId{idFrom(value)}; },
            "value"_a, nb::sig("def __init__(self, value: int) -> None"),
            "ValueError for 0 or a number out of 64 bits; TypeError for anything but an int.")
        .def_ro("value", &SnapshotId::value, "The number.")
        .def(nb::self == nb::self)
        .def(nb::self < nb::self)
        .def(nb::self <= nb::self)
        .def(nb::self >= nb::self)
        .def("__hash__", [](const SnapshotId& self) { return nb::hash(nb::int_(self.value)); })
        .def("__int__", [](const SnapshotId& self) { return self.value; })
        .def("__index__", [](const SnapshotId& self) { return self.value; })
        .def("__str__", [](const SnapshotId& self) { return std::to_string(self.value); })
        .def("__repr__", [](const SnapshotId& self) {
            return "SnapshotId(" + std::to_string(self.value) + ")";
        });

    nb::class_<Snapshot>(m, "Snapshot",
                         "A named, complete develop recipe of one photograph, as its sidecar "
                         "lists it. Read-only; `SnapshotCollection` and `EditSession` make and "
                         "change them.")
        .def_ro("id", &Snapshot::id, "Identity.")
        .def_ro("parent", &Snapshot::parent,
                "Snapshot the working look was based on when this one was captured, or None.")
        .def_ro("name", &Snapshot::name, "Name; not unique.")
        .def_prop_ro(
            "created", [](const Snapshot& self) { return datetimeOf(self.created); },
            nb::sig("def created(self) -> datetime.datetime"),
            "When it was captured, a timezone-aware datetime in UTC, to the second.")
        .def_ro("state", &Snapshot::state, "The develop state it restores.")
        .def_ro("negative", &Snapshot::negative, "The negative settings it restores.")
        .def_prop_ro(
            "has_thumbnail", [](const Snapshot& self) { return self.thumbnail.has_value(); },
            "Whether the sidecar holds a thumbnail of it (the JPEG itself is not exposed).")
        .def(nb::self == nb::self)
        .def("__repr__", [](const Snapshot& self) {
            return "Snapshot(id=" + std::to_string(self.id.value) +
                   ", parent=" + (self.parent ? std::to_string(self.parent->value) : "None") +
                   ", name=" + reprValue(self.name) + ")";
        });
    m.attr("Snapshot").attr("__hash__") = nb::none();

    nb::class_<SnapshotCollection>(m, "SnapshotCollection",
                                   "All snapshots of one photograph, in the order the sidecar "
                                   "lists them.")
        .def_prop_ro(
            "entries", [](const SnapshotCollection& self) { return self.entries; },
            nb::rv_policy::copy, "The snapshots, at most 32; a new list on every access.")
        .def_ro("next_id", &SnapshotCollection::nextId,
                "Number the next snapshot takes; above every ID, never lowered.")
        .def_ro("revision", &SnapshotCollection::revision,
                "Count of structural changes (add, rename, delete) written to the sidecar.")
        .def(
            "find",
            [](const SnapshotCollection& self, const SnapshotId& id) -> std::optional<Snapshot> {
                const Snapshot* found = self.find(id);
                return found ? std::optional<Snapshot>(*found) : std::nullopt;
            },
            "id"_a, "The snapshot with that ID, or None.")
        .def("__len__", [](const SnapshotCollection& self) { return self.entries.size(); })
        .def(
            "__contains__",
            [](const SnapshotCollection& self, nb::handle item) {
                SnapshotId id;
                return nb::try_cast(item, id, false) && self.find(id) != nullptr;
            },
            "item"_a.none(), nb::sig("def __contains__(self, item: object) -> bool"),
            "Whether `item` is the SnapshotId of a snapshot; False for anything else.")
        .def(
            "__iter__",
            [](const SnapshotCollection& self) { return nb::iter(nb::cast(self.entries)); },
            nb::sig("def __iter__(self) -> collections.abc.Iterator[Snapshot]"),
            "Iterate over the snapshots in sidecar order.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const SnapshotCollection& self) {
            return "SnapshotCollection(" + std::to_string(self.entries.size()) +
                   " snapshots, revision=" + std::to_string(self.revision) + ")";
        });
    m.attr("SnapshotCollection").attr("__hash__") = nb::none();

    nb::class_<OpenDocument>(m, "OpenDocument",
                             "A photograph with everything its sidecar adds to it: what "
                             "`open_document` returns.")
        .def_ro("photo", &OpenDocument::photo, "The photograph, as `open` gives it.")
        .def_ro("snapshots", &OpenDocument::snapshots,
                "The snapshots; empty when the sidecar has none.")
        .def_ro("saved_base", &OpenDocument::savedBase,
                "Snapshot the saved recipe was based on, if the sidecar says and it still "
                "exists.")
        .def_ro("unreadable_snapshots", &OpenDocument::unreadableSnapshots,
                "Count of raw snapshot entries in the sidecar that this arraw cannot use; they "
                "are kept until EditSession.discard_unreadable_snapshots removes them.")
        .def("__repr__", [](const OpenDocument& self) {
            return "OpenDocument(photo=" + reprValue(self.photo) +
                   ", snapshots=" + reprValue(self.snapshots) +
                   ", saved_base=" + reprValue(self.savedBase) + ")";
        });
    m.attr("OpenDocument").attr("__hash__") = nb::none();

    m.def(
        "open_document",
        [](const std::filesystem::path& path) {
            PythonLog log;
            return withoutGil([&] { return openDocument(path, log); });
        },
        "path"_a,
        "Open a photograph with its snapshots, reading its metadata and sidecar once. As `open`, "
        "which is this without the snapshots: an unreadable sidecar is logged on the 'arraw' "
        "logger, not raised, and the defaults are used; damaged snapshot entries are logged and "
        "left out (counted in `unreadable_snapshots`).");

    auto photo = nb::borrow<nb::class_<Photo>>(m.attr("Photo"));
    photo.def(
        "with_snapshot",
        [](const Photo& self, const Snapshot& snapshot) { return withSnapshot(self, snapshot); },
        "snapshot"_a,
        "Return a photograph developed as the snapshot says: its state and negative settings "
        "exactly (no lens is matched again), with this photograph's file and marks. This is "
        "what `arraw-cli export --snapshot` does before its flags; render or `with_` it as any "
        "photograph. Nothing is read from the disk or written to it.");
}

} // namespace arraw::python
