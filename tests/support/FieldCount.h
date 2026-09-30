#pragma once

#include <cstddef>
#include <utility>

namespace arraw::test {

/// @brief Placeholder that converts to any type, to probe how many initialisers an aggregate takes.
///
/// Converting to `T&&` and `T&` rather than `T` avoids the ambiguity that
/// `std::optional` and `std::variant` members raise between their converting
/// constructors and a conversion to the whole type, and stops brace elision
/// from descending into nested aggregates.
struct AnyField {
    template <class T> operator T&() const;
    template <class T> operator T&&() const;
};

namespace detail {

template <class T, std::size_t... I> constexpr bool initialisableWith(std::index_sequence<I...>) {
    return requires { T{(static_cast<void>(I), AnyField{})...}; };
}

template <class T, std::size_t N = 0> constexpr std::size_t countFields() {
    if constexpr (initialisableWith<T>(std::make_index_sequence<N + 1>{})) {
        return countFields<T, N + 1>();
    } else {
        return N;
    }
}

} // namespace detail

/// @brief Number of direct members of aggregate @p T.
///
/// C++20 has no reflection, so this is the drift guard for the descriptor
/// table: adding a field to a settings struct changes the count, and the test
/// that pins it then fails until the table has a row (ADR 008).
template <class T> inline constexpr std::size_t fieldCount = detail::countFields<T>();

} // namespace arraw::test
