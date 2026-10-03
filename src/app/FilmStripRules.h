#pragma once

#include <MarksFilter.h>
#include <PhotoMarks.h>

#include <QColor>

#include <optional>
#include <set>
#include <vector>

namespace arraw::app {

/// @brief Padding around the content of a film strip cell, in pixels.
inline constexpr int cellPadding = 6;

/// @brief Smallest side of a film strip cell's content, in pixels.
inline constexpr int minimumCellContent = 32;

/// @brief Value of the filter's star choice that shows only rejects.
inline constexpr int rejectsOnlyChoice = 6;

/// @brief Tells the side of a square cell from the room the strip has.
///
/// Cells are squares that follow the strip's height, as in main's film strip: the
/// content takes the height less the padding on both sides, but never less than
/// ::arraw::app::minimumCellContent, and the cell is the content plus the padding.
/// @param availableHeight Height the cells may use, in pixels.
/// @return Side of a cell, in pixels.
[[nodiscard]] int cellSide(int availableHeight);

/// @brief Finds the shot a step away from the active one among those shown.
/// @param count Number of shots shown.
/// @param current Position of the active shot among them, or -1 when it is not shown.
/// @param delta Steps to take, usually -1 or 1.
/// @return The position reached; without an active shot the first one for a forward
/// step and the last for a backward one; nothing when the step leaves the list or there is
/// no shot.
[[nodiscard]] std::optional<int> adjacentRow(int count, int current, int delta);

/// @brief Decides what a label key does to a set of shots.
///
/// The key toggles: when every shot already has the label it is removed from all of
/// them, otherwise it is set on all of them (those that had another label lose it, and
/// those that had this one keep it). An empty set gives the label, so that a key never
/// does nothing.
/// @param marks Marks of the shots the key acts on.
/// @param label Label of the key.
/// @return The label every shot should have afterwards, or nothing for no label.
[[nodiscard]] std::optional<ColorLabel> toggledLabel(const std::vector<PhotoMarks>& marks,
                                                     ColorLabel label);

/// @brief Builds the filter the title bar's controls stand for.
/// @param starChoice 0 for any rating, 1 to 5 for at least that many stars, or
/// ::arraw::app::rejectsOnlyChoice.
/// @param labels Colours that are switched on; empty for any.
/// @return The filter; values of @p starChoice outside the range count as 0.
[[nodiscard]] MarksFilter filterOf(int starChoice, const std::set<ColorLabel>& labels);

/// @brief Names the swatch colour of a label, as in main's film strip.
/// @param label Label to colour.
/// @return The colour.
[[nodiscard]] QColor labelColour(ColorLabel label);

/// @brief Spells a label for a menu or a tooltip.
/// @param label Label to name.
/// @return `Red`, `Yellow` and so on.
[[nodiscard]] const char* labelName(ColorLabel label);

} // namespace arraw::app
