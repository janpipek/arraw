#pragma once

#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <SettingDescriptors.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace arraw {

/// @brief Named numbers of a compound value, in the order every format writes them.
///
/// A crop rectangle is left, top, right, bottom; a crop ratio is ratio.
using Compound = std::vector<std::pair<std::string, double>>;

/// @brief Control points of a curve as (x, y) pairs, in the order the curve holds them.
using PointList = std::vector<std::pair<double, double>>;

/// @brief Reads control points written as `x,y;x,y;...`.
///
/// The one parse of that spelling, for the sidecar and the command line. Blanks
/// around a number are ignored and so is a leading `+`; nothing else is
/// repaired, and the values are not checked, so each caller applies its own
/// policy to them (a sidecar clamps, the command line refuses).
/// @param text The spelling.
/// @return The points in the order written, or nothing if the text is empty, a
/// pair is missing or does not have two parts, or a part is not a number.
[[nodiscard]] std::optional<PointList> parsePointList(std::string_view text);

/// @brief Format-neutral value of one setting, which each document format maps to its own syntax.
///
/// Null stands for an unset optional, a number for any float or double, a
/// string for an enumeration or a keyword, and a compound for a value of
/// several named numbers, a point list for a curve. Reading accepts the members of a compound in
/// any order; writing uses the order ::arraw::encode gives.
using Encoded = std::variant<std::monostate, bool, double, std::string, Compound, PointList>;

/// @brief Encodes the field a row describes.
///
/// Floats are encoded as the double with the same shortest decimal spelling,
/// so that 0.1F becomes 0.1 rather than 0.10000000149011612, and decode
/// reverses that exactly.
/// @param descriptor Row naming the field.
/// @param settings Settings holding the field.
/// @return The field's value; never null except for an unset optional.
[[nodiscard]] Encoded encode(const FieldDescriptor& descriptor, const DevelopSettings& settings);

/// @brief Lists the compound values a row can encode to, by the names of their members.
///
/// For a format whose text does not say what shape a value has, and that has to
/// build the compound a row would take: a crop rectangle gives
/// left, top, right, bottom, a crop aspect gives ratio.
/// @param descriptor Row naming the field.
/// @return One list of member names, in ::arraw::encode's order, for each
/// compound the row can encode to; none for a row that never encodes to one.
[[nodiscard]] std::vector<std::vector<std::string>>
compoundShapes(const FieldDescriptor& descriptor);

/// @brief Tells whether a row encodes to a point list.
///
/// For a format whose text does not say what shape a value has: a curve's
/// points are the one value that cannot be told from a number or a compound.
/// @param descriptor Row naming the field.
/// @return `true` for a tone curve row.
[[nodiscard]] bool takesPoints(const FieldDescriptor& descriptor);

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
/// rectangle or ratio that the geometry plan would refuse, a curve with fewer
/// than two or more than sixteen points, neighbouring x closer than
/// ::arraw::minimumCurvePointSpacing, an end x further than
/// ::arraw::curveCoordinateTolerance from 0 or 1, or a non-finite coordinate, or a null
/// for a field that cannot be unset is skipped with a ::arraw::Notice::SettingMalformed
/// warning. A number outside the row's range is clamped into it with a
/// ::arraw::Notice::SettingClamped warning (ADR 008), and so is a curve's y
/// outside zero to one; an end x within the tolerance is snapped onto 0 or 1, and a curve's points
/// may come in any order and are sorted by x. Either way the field ends up valid, and a skipped
/// field keeps its value.
/// @param descriptor Row naming the field.
/// @param encoded Value read from a document.
/// @param settings Settings to change.
/// @param log Where the warnings go.
/// @param subject Photograph the document belongs to, when known.
void decode(const FieldDescriptor& descriptor, const Encoded& encoded, DevelopSettings& settings,
            DiagnosticLog& log, const std::optional<std::filesystem::path>& subject = std::nullopt);

/// @brief Spells an encoded value as JSON, as the settings document does.
///
/// Null is `null`, a compound an object with its members in order, and strings
/// are quoted and escaped. Numbers take the shortest text that reads back the same.
/// @param encoded Value to spell.
/// @return The JSON text of the value.
[[nodiscard]] std::string encodedToJson(const Encoded& encoded);

/// @brief Resets the settings a render would not read.
///
/// Temperature and tint belong to Custom white balance, so a photograph in any
/// other mode has none of its own, and they need a sensor, so a photograph
/// that is not a RAW has none either. Whatever a sidecar left in them is dropped.
/// @param settings Settings to clean.
/// @param raw Whether the photograph is a RAW.
/// @return @p settings without the unused ones; what a render reads is unchanged.
[[nodiscard]] DevelopSettings withoutUnusedSettings(DevelopSettings settings, bool raw);

} // namespace arraw
