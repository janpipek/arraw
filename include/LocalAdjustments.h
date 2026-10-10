#pragma once

#include <BrushStrokes.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace arraw {

/// @brief Largest number of local adjustments one photograph holds (ADR 044).
inline constexpr std::size_t maximumLocalAdjustments = 16;

/// @brief Smallest extent of a mask: the distance between a linear mask's ends and a radial
/// mask's radii (ADR 044).
///
/// A state knows no frame size, so the ends' distance is measured in normalised coordinates,
/// `sqrt(du^2 + dv^2)`; a radius is already in long-edge units.
inline constexpr float minimumMaskExtent = 0.001F;

/// @brief Smallest and largest position of a mask handle on either axis, in normalised
/// coordinates. Handles may lie outside the frame (0 to 1), within this limit.
inline constexpr float minimumMaskPosition = -2.0F;

/// @copydoc minimumMaskPosition
inline constexpr float maximumMaskPosition = 3.0F;

/// @brief Largest radius of a radial mask, in long-edge units.
inline constexpr float maximumMaskRadius = 4.0F;

/// @brief Newest version of the local adjustment list this arraw reads and writes, in the state
/// JSON and in the sidecar.
inline constexpr int localAdjustmentsVersion = 1;

/// @brief Identity of one local adjustment, unique within a photograph's state.
///
/// Never zero for an adjustment in a state; the counter
/// ::arraw::DevelopState::nextLocalAdjustmentId starts at 1.
struct LocalAdjustmentId {
    /// @brief The number.
    std::uint32_t value = 0;

    friend bool operator==(const LocalAdjustmentId&, const LocalAdjustmentId&) = default;
    friend auto operator<=>(const LocalAdjustmentId&, const LocalAdjustmentId&) = default;
};

/// @brief A position in the corrected frame, normalised per axis to the frame (ADR 009).
///
/// Distinct from the position in the sensor frame, which lens corrections will add. Without
/// lens corrections both equal the source normalised by its own size.
struct CorrectedPoint {
    float u = 0.0F; ///< Position across, 0 at the left edge and 1 at the right.
    float v = 0.0F; ///< Position down, 0 at the top edge and 1 at the bottom.

    friend bool operator==(const CorrectedPoint&, const CorrectedPoint&) = default;
};

/// @brief A graduated fade between two points.
///
/// The weight is 1 at `from`, 0 at `to`, and smooth between (ADR 044, section 4).
struct LinearMask {
    CorrectedPoint from{0.5F, 0.25F}; ///< Where the weight is 1.
    CorrectedPoint to{0.5F, 0.75F};   ///< Where the weight is 0.

    friend bool operator==(const LinearMask&, const LinearMask&) = default;
};

/// @brief An oval with a feathered edge.
struct RadialMask {
    CorrectedPoint centre{0.5F, 0.5F}; ///< Centre of the oval.
    float radiusX = 0.25F;             ///< Radius along the turned x axis, in long-edge units.
    float radiusY = 0.25F;             ///< Radius along the turned y axis, in long-edge units.
    float angle = 0.0F;                ///< Turn of the x axis towards +y, in degrees.
    float feather = 0.5F;              ///< Width of the soft edge, 0 (hard) to 1 (from the centre).

    friend bool operator==(const RadialMask&, const RadialMask&) = default;
};

/// @brief A stencil painted on the photograph: strokes in the sensor frame (ADR 044, section 6).
struct BrushMask {
    /// @brief The strokes, shared and immutable; never null in a valid state.
    std::shared_ptr<const StrokeList> strokes = emptyStrokeList();

    /// @brief Compares by meaning: equal when the pointers are, or failing that the lists'
    /// contents (ADR 044, section 1).
    friend bool operator==(const BrushMask& a, const BrushMask& b) {
        return a.strokes == b.strokes ||
               (a.strokes != nullptr && b.strokes != nullptr && *a.strokes == *b.strokes);
    }
};

/// @brief The shape of a local adjustment.
///
/// Everything that reads a mask reads its weight, never its shape, so a kind added later is a new
/// alternative, and the shape may become a list of components without changing anything around
/// it.
using Mask = std::variant<LinearMask, RadialMask, BrushMask>;

/// @brief The amounts a local adjustment adds to the global controls, one per row of
/// ::arraw::localAdjustmentDescriptors, in the table's order.
///
/// Each is in the setting units of the global control it acts on (relative temperature and
/// tint have none), and 0 changes nothing.
struct LocalDeltas {
    float relativeTemperature = 0.0F; ///< Warmer (positive) or cooler, relative to the global.
    float relativeTint = 0.0F;        ///< Towards magenta (positive) or green.
    float exposure = 0.0F;            ///< Stops.
    float contrast = 0.0F;            ///< As the global Contrast.
    float highlights = 0.0F;          ///< As the global Highlights.
    float shadows = 0.0F;             ///< As the global Shadows.
    float whites = 0.0F;              ///< As the global Whites.
    float blacks = 0.0F;              ///< As the global Blacks.
    float texture = 0.0F;             ///< As the global Texture.
    float clarity = 0.0F;             ///< As the global Clarity.
    float dehaze = 0.0F;              ///< As the global Dehaze.
    float saturation = 0.0F;          ///< As the global Saturation.
    float vibrance = 0.0F;            ///< As the global Vibrance.

    /// @brief Tells whether every delta is exactly zero.
    [[nodiscard]] bool isZero() const noexcept {
        return *this == LocalDeltas{};
    }

    friend bool operator==(const LocalDeltas&, const LocalDeltas&) = default;
};

/// @brief One masked adjustment: a shape, and the amounts added where it applies.
struct LocalAdjustment {
    /// @brief Identity, unique in the state.
    LocalAdjustmentId id{};

    /// @brief Name the photographer gave it; empty to show the default, such as "Linear 2".
    std::string name;

    /// @brief Whether it acts at all.
    bool enabled = true;

    /// @brief Scale of every delta, 0 to 1.
    float opacity = 1.0F;

    /// @brief Whether the weight is turned inside out (`1 - w`).
    bool invert = false;

    /// @brief Where it applies.
    Mask shape{};

    /// @brief What it adds there.
    LocalDeltas deltas{};

    friend bool operator==(const LocalAdjustment&, const LocalAdjustment&) = default;
};

/// @brief Gives the name a mask's kind has in documents: `linear`, `radial` or `brush`.
/// @param shape Mask to name.
[[nodiscard]] std::string_view maskTypeName(const Mask& shape) noexcept;

/// @brief Removes from a mask's name what it may not hold.
///
/// A name is valid UTF-8 made of characters XML 1.0 can carry (so neither U+FFFE nor U+FFFF),
/// without control characters (tab and line breaks included). What a document reader keeps of a
/// name it is given.
/// @param name Name to clean.
/// @return The name without invalid UTF-8 bytes and without the characters it may not hold.
[[nodiscard]] std::string storableMaskName(std::string_view name);

/// @brief Wraps an angle into [-180, 180).
/// @param degrees Angle in degrees; finite.
[[nodiscard]] float wrappedAngle(float degrees) noexcept;

/// @brief Brings a mask's numbers into their ranges, as every edit rule does (ADR 044).
///
/// Positions are clamped to ::arraw::minimumMaskPosition to ::arraw::maximumMaskPosition, radii
/// to at most ::arraw::maximumMaskRadius, the feather to 0 to 1, and the angle wrapped.
/// @param shape Mask to normalise.
/// @return The mask, which passes ::arraw::validate(const LocalAdjustment&) as a shape.
/// @throws std::invalid_argument if a number is not finite, a radius is below
/// ::arraw::minimumMaskExtent, or a linear mask's ends, once clamped, are closer than
/// ::arraw::minimumMaskExtent.
[[nodiscard]] Mask normalised(const Mask& shape);

/// @brief Brings every number of an adjustment into its range: the shape as
/// ::arraw::normalised(const Mask&) does, the opacity to 0 to 1, each delta to its row's range.
/// @param adjustment Adjustment to normalise; its id is kept.
/// @return The adjustment, which passes ::arraw::validate(const LocalAdjustment&).
/// @throws std::invalid_argument as ::arraw::normalised(const Mask&), or if the opacity or a
/// delta is not finite, or the name is not storable (see ::arraw::storableMaskName).
[[nodiscard]] LocalAdjustment normalised(LocalAdjustment adjustment);

/// @brief Checks one adjustment on its own, not against the rest of a state.
///
/// Every number finite and within its range (so a state that has not been through
/// ::arraw::normalised is refused, as a setting out of range is), a radial mask's radii at least
/// ::arraw::minimumMaskExtent, a linear mask's ends at least that far apart, the name valid
/// UTF-8 without control characters or characters XML cannot hold (see
/// ::arraw::storableMaskName). The id is not checked: see ::arraw::validate(const DevelopState&).
/// @param adjustment Adjustment to check.
/// @throws std::invalid_argument naming what is wrong.
void validate(const LocalAdjustment& adjustment);

} // namespace arraw
