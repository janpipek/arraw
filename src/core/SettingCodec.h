#pragma once

#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <SettingDescriptors.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace arraw {

/// @brief Named numbers of a compound value, in the order every format writes them.
///
/// A crop rectangle is left, top, right, bottom; a crop ratio is ratio.
using Compound = std::vector<std::pair<std::string, double>>;

/// @brief Format-neutral value of one setting, which each document format maps to its own syntax.
///
/// Null stands for an unset optional, a number for any float or double, a
/// string for an enumeration or a keyword, and a compound for a value of
/// several named numbers. Reading accepts the members of a compound in any
/// order; writing uses the order ::arraw::encode gives.
using Encoded = std::variant<std::monostate, bool, double, std::string, Compound>;

/// @brief Encodes the field a row describes.
///
/// Floats are encoded as the double with the same shortest decimal spelling,
/// so that 0.1F becomes 0.1 rather than 0.10000000149011612, and decode
/// reverses that exactly.
/// @param descriptor Row naming the field.
/// @param settings Settings holding the field.
/// @return The field's value; never null except for an unset optional.
[[nodiscard]] Encoded encode(const FieldDescriptor& descriptor, const DevelopSettings& settings);

/// @brief Describes what a row accepts, in words for a warning.
/// @param descriptor Row naming the field.
/// @return The expectation, for instance "a number, or unset".
[[nodiscard]] std::string expectation(const FieldDescriptor& descriptor);

/// @brief Reports a value that a row cannot take, as ::arraw::Notice::SettingMalformed.
///
/// For a format that finds the value unreadable before it reaches ::arraw::decode.
/// @param descriptor Row naming the field.
/// @param log Where the warning goes.
/// @param subject Photograph the document belongs to, when known.
void reportMalformed(const FieldDescriptor& descriptor, DiagnosticLog& log,
                     const std::optional<std::filesystem::path>& subject = std::nullopt);

/// @brief Applies an encoded value to the field a row describes.
///
/// A value of the wrong shape, a non-finite number, an unknown name, a crop
/// rectangle or ratio that the geometry plan would refuse, or a null
/// for a field that cannot be unset is skipped with a ::arraw::Notice::SettingMalformed
/// warning. A number outside the row's range is clamped into it with a
/// ::arraw::Notice::SettingClamped warning (ADR 008). Either way the field
/// ends up valid, and a skipped field keeps its value.
/// @param descriptor Row naming the field.
/// @param encoded Value read from a document.
/// @param settings Settings to change.
/// @param log Where the warnings go.
/// @param subject Photograph the document belongs to, when known.
void decode(const FieldDescriptor& descriptor, const Encoded& encoded, DevelopSettings& settings,
            DiagnosticLog& log, const std::optional<std::filesystem::path>& subject = std::nullopt);

} // namespace arraw
