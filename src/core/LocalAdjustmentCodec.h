#pragma once

#include <DevelopState.h>
#include <Diagnostics.h>
#include <LocalAdjustments.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// The format-neutral half of reading and writing local adjustments (ADR 044, section 9).
///
/// A document format (the state JSON, the XMP sidecar) turns each mask it finds into
/// ::arraw::MaskFields, and ::arraw::readLocalAdjustments applies every rule of reading to the lot:
/// what is dropped, what is clamped, how the counter is repaired, and the warning for each. Writing
/// goes the other way through ::arraw::writtenForm. The formats keep only their syntax.

namespace arraw {

/// @brief A scalar read from a document, in every reading it could stand for.
///
/// A JSON number sets only `number`, a JSON boolean only `flag`, a JSON string only `text`. XMP
/// holds text alone, so it sets `text` and whichever of the others the text spells.
struct FieldValue {
    /// @brief The value as a number, when it is one (possibly not finite).
    std::optional<double> number;

    /// @brief The value as a flag, when it is one.
    std::optional<bool> flag;

    /// @brief The value as text, when it is text.
    std::optional<std::string> text;

    /// @brief Whether the document held something that is not a scalar (an array, an object,
    /// null) where a scalar belongs.
    bool structured = false;
};

/// @brief Fields of a mask in a document, by key, in the order the document gave them.
using FieldList = std::vector<std::pair<std::string, FieldValue>>;

/// @brief One mask as a document holds it, before any rule is applied.
struct MaskFields {
    /// @brief `id`, `type`, `name`, `enabled`, `opacity` and `invert`.
    FieldList top;

    /// @brief The geometry, flat: `fromX`, `fromY`, `toX`, `toY`; or `centreX`, `centreY`,
    /// `radiusX`, `radiusY`, `angle`, `feather`.
    FieldList geometry;

    /// @brief The deltas, by key of ::arraw::localAdjustmentDescriptors.
    FieldList deltas;

    /// @brief What is wrong with the entry as a whole, such as "not an object"; empty if nothing.
    std::string problem;
};

/// @brief What reading a list of masks gave.
struct ReadLocalAdjustments {
    /// @brief The masks kept, in the order of the document.
    std::vector<LocalAdjustment> adjustments;

    /// @brief The counter: the stored one, repaired to be above every id kept; at least 1.
    LocalAdjustmentId next{1};
};

/// @brief Tells which part of a mask an XMP field key belongs to.
enum class FieldPart { Top, Geometry, Delta };

/// @brief Sorts an XMP field key into the part of a mask it belongs to.
/// @param key Local name of the field.
/// @return Top for the six general fields, Geometry for the geometry keys of either kind, and
/// Delta for anything else (a table key, or a key to be reported as unknown).
[[nodiscard]] FieldPart partOfKey(std::string_view key) noexcept;

/// @brief Applies the rules of reading to the masks a document holds.
///
/// Dropped with a ::arraw::Notice::LocalAdjustmentDropped warning, naming position, id, type and
/// reason: an entry that is not a structure, an unknown type (a brush included, for now), a
/// missing or malformed field, a number that is not finite, an id that is zero or too large,
/// degenerate geometry, a duplicate id (the later one), and every entry from the seventeenth on.
/// Ignored with ::arraw::Notice::LocalAdjustmentFieldIgnored: an unknown field or delta key.
/// Clamped with ::arraw::Notice::SettingClamped, under the key `localAdjustments[position].field`:
/// opacity, feather, radii, positions, deltas, and an angle outside [-180, 180) (wrapped). Control
/// characters are removed from a name, silently.
/// @param entries Masks as the document gave them, in order.
/// @param storedNext The counter the document gave, when it gave one.
/// @param log Where the warnings go.
/// @param subject Photograph the document belongs to, when known.
/// @return The masks kept and the repaired counter.
[[nodiscard]] ReadLocalAdjustments
readLocalAdjustments(std::span<const MaskFields> entries, std::optional<double> storedNext,
                     DiagnosticLog& log, const std::optional<std::filesystem::path>& subject);

/// @brief A value to write: a flag, a number or text.
using WrittenValue = std::variant<bool, double, std::string>;

/// @brief Fields to write, by key.
using WrittenFields = std::vector<std::pair<std::string, WrittenValue>>;

/// @brief One mask as a document is to hold it.
struct WrittenMask {
    /// @brief `id`, `type`, `name`, `enabled`, `opacity` and `invert`, in that order.
    WrittenFields top;

    /// @brief The geometry, flat, in the order of the keys of ::arraw::MaskFields::geometry.
    WrittenFields geometry;

    /// @brief The non-zero deltas, in table order.
    WrittenFields deltas;
};

/// @brief Turns a mask into the fields a document writes.
///
/// Numbers are the doubles whose shortest text reads back as the float, so every format that
/// spells them shortest round-trips exactly.
/// @param adjustment Mask to write.
[[nodiscard]] WrittenMask writtenForm(const LocalAdjustment& adjustment);

} // namespace arraw
