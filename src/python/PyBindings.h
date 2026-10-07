#pragma once

// Python.h (pulled in by nanobind) must come before any standard header, so the
// nanobind block deliberately breaks the CLAUDE.md include grouping. Every
// binding file includes this header first and so sees the same set of type
// casters, which nanobind needs for a consistent conversion of one type.
// clang-format off
#include <nanobind/nanobind.h>
#include <nanobind/operators.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/set.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/variant.h>
#include <nanobind/stl/vector.h>
// clang-format on

#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <ShortestDecimal.h>
#include <ToneCurveSettings.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/// @brief Caster of a curve point to and from a Python `(x, y)` tuple.
///
/// A float goes out as the double of its shortest decimal spelling, so that a
/// point read as 0.3 shows as 0.3 and not as 0.30000001192092896. Reading
/// takes any two-item sequence of numbers except text and bytes, and goes
/// through the shortest spelling too, so that the two directions are inverse.
template <> struct nanobind::detail::type_caster<arraw::CurvePoint> {
    NB_TYPE_CASTER(arraw::CurvePoint, const_name("tuple[float, float]"))

    /// @brief Reads a two-item sequence of numbers.
    bool from_python(nanobind::handle source, uint8_t, cleanup_list*) noexcept {
        PyObject* object = source.ptr();
        if (PyUnicode_Check(object) || PyBytes_Check(object) || PyByteArray_Check(object) ||
            PySequence_Check(object) == 0 || PySequence_Size(object) != 2) {
            PyErr_Clear();
            return false;
        }
        double coordinates[2]{};
        for (Py_ssize_t index = 0; index < 2; ++index) {
            PyObject* item = PySequence_GetItem(object, index);
            const bool number = item != nullptr && PyBool_Check(item) == 0;
            coordinates[index] = number ? PyFloat_AsDouble(item) : 0.0;
            const bool failed = !number || PyErr_Occurred() != nullptr;
            Py_XDECREF(item);
            if (failed) {
                PyErr_Clear();
                return false;
            }
        }
        value = arraw::CurvePoint{arraw::shortestFloat(coordinates[0]),
                                  arraw::shortestFloat(coordinates[1])};
        return true;
    }

    /// @brief Makes an `(x, y)` tuple.
    static handle from_cpp(const arraw::CurvePoint& point, rv_policy, cleanup_list*) noexcept {
        PyObject* tuple = PyTuple_New(2);
        PyTuple_SET_ITEM(tuple, 0, PyFloat_FromDouble(arraw::shortestDouble(point.x)));
        PyTuple_SET_ITEM(tuple, 1, PyFloat_FromDouble(arraw::shortestDouble(point.y)));
        return tuple;
    }
};

/// @brief Private helpers of the Python bindings.
namespace arraw::python {

namespace nb = nanobind;
using namespace nb::literals;

/// @brief Binds images, pixel formats, colour encodings and metadata.
void bindImage(nb::module_& module);

/// @brief Binds the EXIF value classes and `read_exif`.
void bindExif(nb::module_& module);

/// @brief Binds the settings classes, their enumerations and the descriptor table.
void bindSettings(nb::module_& module);

/// @brief Binds shots, the marks filter and the supported extensions.
void bindShots(nb::module_& module);

/// @brief Binds photographs, diagnostics and the develop and save functions.
void bindPhoto(nb::module_& module);

/// @brief Applies flat snake_case keywords to settings, driven by the descriptor table.
/// @param settings Settings to change.
/// @param keywords Keyword arguments, each naming one leaf of the table.
/// @throws nb::type_error for a key not in the table or a value of the wrong type.
void applyFlatSettings(DevelopSettings& settings, const nb::kwargs& keywords);

/// @brief Log that forwards each diagnostic to `logging.getLogger("arraw")`.
///
/// Records may come from a thread that has released the GIL, so each one
/// acquires it before calling into Python.
class PythonLog final : public DiagnosticLog {
public:
    /// @brief Forwards one diagnostic at the level matching its severity.
    /// @param diagnostic What happened.
    void record(const Diagnostic& diagnostic) override;
};

/// @brief Runs a callable with the GIL released.
/// @param function Callable that touches no Python object.
/// @return Whatever the callable returns.
template <class Function> decltype(auto) withoutGil(Function&& function) {
    nb::gil_scoped_release release;
    return function();
}

/// @brief Trait for an optional holding an enumeration.
template <class V> struct IsOptionalEnum : std::false_type {};
template <class V> struct IsOptionalEnum<std::optional<V>> : std::is_enum<V> {};

/// @brief Casts a Python value to a C++ type, naming the setting on failure.
///
/// Stricter than nanobind's own casts, which read a bool as a number and an
/// int as an enumeration member: `exposure=True` or `rotation=1` is more
/// likely a mistake than a meaning, so both are refused.
/// @tparam V Target type.
/// @param value Python object.
/// @param name Name of the setting or argument, for the message.
/// @throws nb::type_error if the value is of the wrong type.
template <class V> V convertValue(nb::handle value, std::string_view name) {
    const auto refuse = [&] {
        return nb::type_error(("'" + std::string(name) + "': cannot use a value of type '" +
                               std::string(nb::type_name(value.type()).c_str()) + "'")
                                  .c_str());
    };
    constexpr bool numeric = std::is_floating_point_v<V> || std::is_integral_v<V> ||
                             std::is_same_v<V, std::optional<float>>;
    if constexpr (numeric && !std::is_same_v<V, bool>) {
        if (nb::isinstance<nb::bool_>(value)) {
            throw refuse();
        }
    }
    if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
        // Anything usable as an index (NumPy integers included), but no bool (refused above).
        if (!PyIndex_Check(value.ptr())) {
            throw refuse();
        }
    }
    if constexpr (std::is_enum_v<V>) {
        if (!nb::isinstance<V>(value)) {
            throw refuse();
        }
    }
    if constexpr (IsOptionalEnum<V>::value) {
        if (!value.is_none() && !nb::isinstance<typename V::value_type>(value)) {
            throw refuse();
        }
    }
    try {
        return nb::cast<V>(value);
    } catch (const nb::cast_error&) {
        throw refuse();
    }
}

/// @brief Formats a float as its shortest text that reads back the same.
/// @param value Number to format.
/// @return Text such as "0.7" or "25.0".
inline std::string formatFloat(float value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string text(buffer, result.ptr);
    if (text.find_first_of(".en") == std::string::npos) {
        text += ".0";
    }
    return text;
}

/// @brief Formats any bound value as Python would repr it.
template <class V> std::string reprValue(const V& value) {
    return std::string(nb::repr(nb::cast(value)).c_str());
}

/// @copydoc reprValue
inline std::string reprValue(float value) {
    return formatFloat(value);
}

/// @copydoc reprValue
inline std::string reprValue(const std::optional<float>& value) {
    return value ? formatFloat(*value) : "None";
}

/// @brief Name and member pointer of one attribute of a settings class.
template <class T, class M> struct Field {
    /// @brief snake_case attribute and keyword name.
    const char* name;

    /// @brief Member the attribute reads.
    M T::* member;
};

/// @brief Describes one attribute of a settings class.
template <class T, class M> Field<T, M> field(const char* name, M T::* member) {
    return {name, member};
}

template <class M> struct IsOptional : std::false_type {};
template <class V> struct IsOptional<std::optional<V>> : std::true_type {};

/// @brief Type a constructor takes for a field of type M.
///
/// A default that is an instance of a bound class would be held by the
/// constructor's function object for the life of the interpreter, and nanobind
/// reports that at exit as a leak. Such fields therefore default to None,
/// which the constructor reads as the C++ default.
template <class M>
using ConstructorParam =
    std::conditional_t<std::is_class_v<M> && !IsOptional<M>::value, std::optional<M>, M>;

/// @brief Default of a constructor parameter: the C++ default, or None for class types.
template <class M> nb::object constructorDefault(const M& value) {
    if constexpr (std::is_same_v<ConstructorParam<M>, M>) {
        return nb::cast(value);
    } else {
        return nb::none();
    }
}

/// @brief Whether a field is a number, which nanobind would fill from a bool (or refuse a NumPy
/// integer for).
template <class M>
inline constexpr bool isNumberField =
    std::is_floating_point_v<M> || (std::is_integral_v<M> && !std::is_same_v<M, bool>) ||
    std::is_same_v<M, std::optional<float>>;

/// @brief Type a constructor takes for a field of type M, as a Python annotation and a C++ value.
///
/// A number field takes any object, so that convertValue can refuse a bool as replace() does,
/// while nanobind's own cast would let it through. Every other field is cast by nanobind,
/// with noconvert (see argument()) refusing what convertValue would.
template <class M>
using ConstructorArgument =
    std::conditional_t<isNumberField<M>, nb::typed<nb::object, ConstructorParam<M>>,
                       ConstructorParam<M>>;

/// @brief Builds the keyword argument of a constructor parameter.
///
/// An enumeration refuses anything but its own kind (no int for an enumeration member), as
/// convertValue does.
template <class M> auto argument(const char* name, const nb::object& value) {
    if constexpr (std::is_enum_v<M> || IsOptionalEnum<M>::value) {
        return nb::arg(name).noconvert() = value;
    } else if constexpr (isNumberField<M>) {
        return nb::arg(name).none() = value; // an object argument takes None only when told to
    } else {
        return nb::arg(name) = value;
    }
}

/// @brief Moves one constructor argument onto a field; None leaves a class at its default.
template <class M, class Argument>
void assignArgument(M& target, Argument& source, const char* name) {
    if constexpr (isNumberField<M>) {
        target = convertValue<M>(source, name);
    } else if constexpr (std::is_same_v<Argument, std::optional<M>>) {
        if (source) {
            target = std::move(*source);
        }
    } else {
        target = std::move(source);
    }
}

/// @brief Tells whether a type is a std::vector.
template <class M> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};

/// @brief Tells whether a type is a std::set.
template <class M> struct IsSet : std::false_type {};
template <class T, class C, class A> struct IsSet<std::set<T, C, A>> : std::true_type {};

/// @brief Casts a field to a Python value that can be hashed: a vector becomes a tuple, a set a
/// frozenset.
template <class M> nb::object hashable(const M& value) {
    nb::object cast = nb::cast(value);
    if constexpr (IsVector<M>::value) {
        return nb::steal(PySequence_Tuple(cast.ptr()));
    } else if constexpr (IsSet<M>::value) {
        return nb::steal(PyFrozenSet_New(cast.ptr()));
    } else {
        return cast;
    }
}

/// @brief Puts a freshly built value in its canonical form; most values have one already.
template <class T> void canonicalise(T&) {}

/// @brief Sorts a curve's points by x and snaps its ends, so that Python takes them in any
/// order as every other frontend does (ADR 033).
inline void canonicalise(arraw::ToneCurve& curve) {
    arraw::normaliseCurvePoints(curve.points);
}

/// @brief Binds a plain settings struct as a frozen Python value class.
///
/// Gives it a keyword constructor with the C++ defaults, read-only attributes,
/// `replace(**kw)`, `==`, a hash and a repr, all from the one field list. Constructor and
/// `replace` convert values by the one rule of convertValue.
/// @param module Module to add the class to.
/// @param name Python class name.
/// @param doc Class docstring.
/// @param fields Attributes in constructor order.
/// @tparam KeywordOnly Whether the constructor takes keyword arguments only, so that user code
/// does not depend on the C++ member order; small value types turn it off.
/// @return The class, for further methods.
template <class T, bool KeywordOnly = true, class... Ms>
nb::class_<T> bindFrozen(nb::module_& module, const char* name, const char* doc,
                         Field<T, Ms>... fields) {
    nb::class_<T> cls(module, name, doc);
    [[maybe_unused]] const T defaults{};
    const auto init = [=](T* self, ConstructorArgument<Ms>... values) {
        T value;
        (assignArgument(value.*fields.member, values, fields.name), ...);
        canonicalise(value);
        new (self) T(std::move(value));
    };
    const auto defineInit = [&](auto... leading) {
        cls.def("__init__", init, leading...,
                (argument<Ms>(fields.name, constructorDefault(defaults.*fields.member)))...);
    };
    if constexpr (KeywordOnly && sizeof...(Ms) > 0) {
        defineInit(nb::kw_only());
    } else {
        defineInit();
    }
    (cls.def_ro(fields.name, fields.member), ...);
    cls.def(nb::self == nb::self);
    cls.def("__hash__", [=](const T& self) {
        return nb::hash(nb::make_tuple(hashable(self.*fields.member)...));
    });
    cls.def("__repr__", [=](const T& self) {
        std::string text = std::string(name) + "(";
        bool first = true;
        [[maybe_unused]] const auto append = [&](const char* attribute, const std::string& value) {
            text += (first ? "" : ", ") + std::string(attribute) + "=" + value;
            first = false;
        };
        (append(fields.name, reprValue(self.*fields.member)), ...);
        return text + ")";
    });
    cls.def(
        "replace",
        [=](const T& self, const nb::kwargs& keywords) {
            T copy = self;
            for (auto [key, value] : keywords) {
                const std::string keyName = nb::cast<std::string>(key);
                const bool known =
                    ((keyName == fields.name
                          ? (copy.*fields.member = convertValue<Ms>(value, keyName), true)
                          : false) ||
                     ...);
                if (!known) {
                    throw nb::type_error(
                        ("replace() got an unexpected keyword argument '" + keyName + "'").c_str());
                }
            }
            canonicalise(copy);
            return copy;
        },
        "Return a copy with the given attributes replaced.");
    return cls;
}

} // namespace arraw::python
