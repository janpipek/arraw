#pragma once

#include <DevelopState.h>
#include <LocalAdjustments.h>
#include <SettingDescriptors.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace arraw {

/// @name Local adjustment edits
///
/// The edit rules of the masks (ADR 044), beside those of ::arraw::withValue: what the GUI, the
/// command line (never, by decision) and Python go through. Each takes a state and returns a new
/// one, so a throw leaves the caller's state as it was. Only the adjustment edited is checked and
/// normalised (::arraw::normalised): a number out of range is clamped, a non-finite one is
/// refused. The settings and the other adjustments are not looked at, nor the photograph.
///
/// Every function that takes an id throws `std::invalid_argument` for one that is not in the list.
/// @{

/// @brief Tells whether an adjustment can be added or duplicated.
/// @param state State to ask about.
/// @return `false` when the list holds ::arraw::maximumLocalAdjustments, or the counter has run
/// out, so that a front end greys the action out.
[[nodiscard]] bool canAddLocalAdjustment(const DevelopState& state) noexcept;

/// @brief Finds an adjustment by its id.
/// @param state State to search.
/// @param id Id to find.
/// @return The adjustment, or null when the list holds none with that id. Valid until @p state
/// changes.
[[nodiscard]] const LocalAdjustment* findLocalAdjustment(const DevelopState& state,
                                                         LocalAdjustmentId id) noexcept;

/// @brief Appends an adjustment, giving it the next id.
///
/// The id of @p adjustment is ignored: the counter's value is taken and the counter incremented,
/// so removing an adjustment later never frees its id for another.
/// @param state State to add to.
/// @param adjustment Adjustment to add, normalised.
/// @return @p state with the adjustment last in the list, so the new id is
/// `localAdjustments.back().id`.
/// @throws std::invalid_argument if the list is full (and @p state is unchanged), the counter has
/// run out, or @p adjustment cannot be normalised.
[[nodiscard]] DevelopState withLocalAdjustmentAdded(DevelopState state, LocalAdjustment adjustment);

/// @brief Appends an enabled adjustment of a shape, at full opacity and with no deltas.
/// @param state State to add to.
/// @param shape Where it applies.
/// @return @p state with the adjustment last in the list.
/// @throws std::invalid_argument as ::arraw::withLocalAdjustmentAdded.
[[nodiscard]] DevelopState withLocalAdjustmentAdded(DevelopState state, Mask shape);

/// @brief Copies an adjustment, right after the original, under a new id.
///
/// The copy keeps the name, the flags, the shape and the deltas.
/// @param state State to edit.
/// @param id Adjustment to copy.
/// @return @p state with the copy in the list; its id is the counter's value before the call.
/// @throws std::invalid_argument if @p id is not in the list, or the list is full.
[[nodiscard]] DevelopState withLocalAdjustmentDuplicated(DevelopState state, LocalAdjustmentId id);

/// @brief Removes an adjustment. The counter is not lowered.
/// @param state State to edit.
/// @param id Adjustment to remove.
/// @return @p state without it.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] DevelopState withLocalAdjustmentRemoved(DevelopState state, LocalAdjustmentId id);

/// @brief Moves an adjustment to a position in the list.
/// @param state State to edit.
/// @param id Adjustment to move.
/// @param index Position it takes, 0 for first; the others keep their order.
/// @return @p state reordered.
/// @throws std::invalid_argument if @p id is not in the list or @p index is not below the list's
/// size.
[[nodiscard]] DevelopState withLocalAdjustmentReordered(DevelopState state, LocalAdjustmentId id,
                                                        std::size_t index);

/// @brief Names an adjustment.
/// @param state State to edit.
/// @param id Adjustment to name.
/// @param name New name; empty to show the default (::arraw::defaultMaskName).
/// @return @p state with the name set.
/// @throws std::invalid_argument if @p id is not in the list or @p name is not storable (see
/// ::arraw::storableMaskName).
[[nodiscard]] DevelopState withLocalAdjustmentRenamed(DevelopState state, LocalAdjustmentId id,
                                                      std::string name);

/// @brief Turns an adjustment on or off.
/// @param state State to edit.
/// @param id Adjustment to edit.
/// @param enabled Whether it acts.
/// @return @p state with the flag set.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] DevelopState withLocalAdjustmentEnabled(DevelopState state, LocalAdjustmentId id,
                                                      bool enabled);

/// @brief Turns an adjustment's weight inside out, or back.
/// @param state State to edit.
/// @param id Adjustment to edit.
/// @param invert Whether the weight is `1 - w`.
/// @return @p state with the flag set.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] DevelopState withLocalAdjustmentInverted(DevelopState state, LocalAdjustmentId id,
                                                       bool invert);

/// @brief Sets an adjustment's opacity.
/// @param state State to edit.
/// @param id Adjustment to edit.
/// @param opacity Scale of every delta; clamped to 0 to 1.
/// @return @p state with the opacity set.
/// @throws std::invalid_argument if @p id is not in the list or @p opacity is not finite.
[[nodiscard]] DevelopState withLocalOpacity(DevelopState state, LocalAdjustmentId id,
                                            float opacity);

/// @brief Sets one delta of an adjustment.
/// @param state State to edit.
/// @param id Adjustment to edit.
/// @param key camelCase name of the control, as in ::arraw::localAdjustmentDescriptors.
/// @param value New delta, in setting units; clamped to the control's local range.
/// @return @p state with the delta set.
/// @throws std::invalid_argument if @p id is not in the list, @p key names no local control, or
/// @p value is not finite.
[[nodiscard]] DevelopState withLocalDelta(DevelopState state, LocalAdjustmentId id,
                                          std::string_view key, double value);

/// @brief Moves an adjustment's handles: replaces its shape by another of the same kind.
///
/// What a handle drag calls, with the shape the drag gives. The numbers are clamped as
/// ::arraw::normalised(const Mask&) says, so a handle dragged far outside the frame stops at its
/// limit.
/// @param state State to edit.
/// @param id Adjustment to edit.
/// @param shape New geometry; of the kind the adjustment already is.
/// @return @p state with the shape set.
/// @throws std::invalid_argument if @p id is not in the list, @p shape is of another kind, or it
/// cannot be normalised.
[[nodiscard]] DevelopState withLocalShape(DevelopState state, LocalAdjustmentId id, Mask shape);

/// @brief Tells whether a stroke can be appended to a brush mask: it normalises, and the mask
/// stays within every cap and budget (what a front end shows as "mask full").
/// @param state State holding the mask.
/// @param id Brush mask to ask about.
/// @param stroke Stroke to append.
/// @return `false` if @p id is not in the list or not a brush mask, the stroke cannot be
/// normalised, or the mask would pass a cap or budget.
[[nodiscard]] bool canAppendStroke(const DevelopState& state, LocalAdjustmentId id,
                                   const Stroke& stroke) noexcept;

/// @brief Appends a stroke to a brush mask, normalised (::arraw::normalised(Stroke)).
/// @param state State to edit.
/// @param id Brush mask to extend.
/// @param stroke Stroke to append.
/// @return @p state with the stroke last in the mask; earlier strokes are shared.
/// @throws std::invalid_argument if @p id is not in the list or not a brush mask, the stroke
/// cannot be normalised, or the mask would pass a cap or budget; @p state is then unchanged.
[[nodiscard]] DevelopState withStrokeAppended(DevelopState state, LocalAdjustmentId id,
                                              Stroke stroke);

/// @}

/// @brief Counts the adjustments of the same kind up to and including one, in list order.
/// @param state State holding the adjustment.
/// @param id Adjustment to count.
/// @return Its place among the masks of its kind (linear, radial or brush): 1 for the first.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] std::size_t maskOrdinal(const DevelopState& state, LocalAdjustmentId id);

/// @brief Gives the name an unnamed adjustment is shown under, such as "Linear 2" or "Brush 1"
/// (English).
///
/// The kind and its ::arraw::maskOrdinal. A translated front end builds its own from the same two.
/// @param state State holding the adjustment.
/// @param id Adjustment to name.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] std::string defaultMaskName(const DevelopState& state, LocalAdjustmentId id);

/// @brief Gives the name an adjustment is shown under: its own, or the default when it has none.
/// @param state State holding the adjustment.
/// @param id Adjustment to name.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] std::string displayedMaskName(const DevelopState& state, LocalAdjustmentId id);

} // namespace arraw
