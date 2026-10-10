#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace arraw {

/// @brief Version of the brush rasteriser: the rules that turn strokes into coverage (ADR 044,
/// section 6).
inline constexpr std::uint32_t brushRasteriserVersion = 1;

/// @brief Cap on points per stroke (ADR 044 section 6).
inline constexpr std::size_t maximumStrokePoints = 10'000;

/// @brief Cap on strokes per mask.
inline constexpr std::size_t maximumStrokesPerMask = 2'000;

/// @brief Cap on points in all the strokes of a mask, which bounds its sidecar.
inline constexpr std::size_t maximumPointsPerMask = 100'000;

/// @brief Cap on a mask's swept area, `sum of length * radius`, lengths in normalised (u, v) units
/// and radii in long-edge units. It bounds the time to rasterise without knowing a raster size.
inline constexpr double maximumSweptAreaPerMask = 4.0;

/// @brief Cap on a mask's dabs, `sum of length / (0.25 * radius)`, lengths as for the swept area.
inline constexpr double maximumDabsPerMask = 2'000'000.0;

/// @brief Smallest brush radius, in long-edge units.
inline constexpr float minimumBrushRadius = 0.0005F;

/// @brief Largest brush radius, in long-edge units.
inline constexpr float maximumBrushRadius = 1.0F;

/// @brief Position of a pointer event in normalised sensor coordinates.
struct SensorPoint {
    float u = 0.0F; ///< Fraction of the width.
    float v = 0.0F; ///< Fraction of the height.

    friend bool operator==(const SensorPoint&, const SensorPoint&) = default;
};

/// @brief One brush stroke: a style and the path it followed.
struct Stroke {
    float radius = 0.02F;            ///< Dab radius, in long-edge units of the frame.
    float hardness = 0.5F;           ///< Fraction of the radius that is fully opaque, 0 to 1.
    float flow = 1.0F;               ///< Opacity of one dab, 0 to 1.
    bool erase = false;              ///< Whether the stroke removes coverage rather than adds it.
    std::vector<SensorPoint> points; ///< At least one, at most ::arraw::maximumStrokePoints.

    friend bool operator==(const Stroke&, const Stroke&) = default;
};

/// @brief What a stroke or a list of strokes spends of a mask's budgets.
struct StrokeBudget {
    std::size_t points = 0; ///< Points, against ::arraw::maximumPointsPerMask.
    double sweptArea = 0.0; ///< Against ::arraw::maximumSweptAreaPerMask.
    double dabs = 0.0;      ///< Against ::arraw::maximumDabsPerMask.

    /// @brief Adds another stroke's cost.
    StrokeBudget& operator+=(const StrokeBudget& other) noexcept;

    /// @brief Whether every budget of a mask holds.
    [[nodiscard]] bool withinMaskLimits() const noexcept;
};

/// @brief Measures what one stroke costs: its points, and its length times its radius, and divided
/// by a quarter radius, with the length along the path in normalised (u, v) units. Those bound the
/// long-edge lengths for every aspect.
[[nodiscard]] StrokeBudget budgetOf(const Stroke& stroke) noexcept;

/// @brief Refuses a stroke outside the contract.
///
/// Checks that every number is finite, the radius is within the brush radius range, hardness
/// and flow are within 0 to 1, positions are within the mask position range per axis, the
/// stroke has one to ::arraw::maximumStrokePoints points, and it alone fits the swept-area and
/// dab budgets of a mask.
/// @throws std::invalid_argument naming the field.
void validate(const Stroke& stroke);

/// @brief Clamps a stroke as the edit rules and readers will: radius, hardness, flow and positions.
/// @throws std::invalid_argument for a non-finite number, no points, more than the cap, or a
/// stroke that alone passes a mask's swept-area or dab budget.
[[nodiscard]] Stroke normalised(Stroke stroke);

/// @brief An immutable, persistent list of strokes: appending shares every earlier stroke.
class StrokeList {
public:
    /// @brief Makes an empty list of the current rasteriser.
    StrokeList();

    /// @brief Makes a list of validated strokes.
    ///
    /// Any rasteriser number is accepted, so that a reader can build a list and then refuse it;
    /// validating a state refuses one other than ::arraw::brushRasteriserVersion.
    /// @throws std::invalid_argument past a cap or budget of a mask, or for a bad stroke.
    explicit StrokeList(std::vector<Stroke> strokes,
                        std::uint32_t rasteriser = brushRasteriserVersion);

    /// @brief Version of the rasteriser the list is to be drawn with.
    [[nodiscard]] std::uint32_t rasteriser() const noexcept {
        return rasteriser_;
    }

    /// @brief Number of strokes.
    [[nodiscard]] std::size_t size() const noexcept {
        return strokes_.size();
    }

    /// @brief Whether the list has no strokes.
    [[nodiscard]] bool empty() const noexcept {
        return strokes_.empty();
    }

    /// @brief Reads one stroke.
    [[nodiscard]] const Stroke& operator[](std::size_t index) const {
        return *strokes_.at(index);
    }

    /// @brief The shared strokes, in painting order.
    [[nodiscard]] std::span<const std::shared_ptr<const Stroke>> strokes() const noexcept {
        return strokes_;
    }

    /// @brief Total number of points of all strokes.
    [[nodiscard]] std::size_t pointCount() const noexcept {
        return budget_.points;
    }

    /// @brief What all the strokes spend of the mask's budgets.
    [[nodiscard]] const StrokeBudget& budget() const noexcept {
        return budget_;
    }

    /// @brief Whether appending @p stroke would stay within every cap and budget of a mask. The
    /// stroke is assumed valid.
    [[nodiscard]] bool accepts(const Stroke& stroke) const noexcept;

    /// @brief Hash of the rasteriser and every stroke's bits, extended on append.
    ///
    /// Equal contents give equal hashes. Stable across processes and builds (FNV-1a over the
    /// bytes defined in `BrushStrokes.cpp`): the thumbnail cache's key stores it. Changing the
    /// definition invalidates thumbnail keys only.
    [[nodiscard]] std::uint64_t contentHash() const noexcept {
        return hash_;
    }

    /// @brief Makes a new list of these strokes (shared, not copied) then @p stroke.
    /// @throws std::invalid_argument past a cap or budget of a mask or for a bad stroke; this
    /// list is unchanged.
    [[nodiscard]] std::shared_ptr<const StrokeList> appended(Stroke stroke) const;

    /// @brief Compares by meaning: same rasteriser, and for each index the same pointer or equal
    /// strokes.
    friend bool operator==(const StrokeList& a, const StrokeList& b);

private:
    std::uint32_t rasteriser_ = brushRasteriserVersion;
    std::vector<std::shared_ptr<const Stroke>> strokes_;
    StrokeBudget budget_;
    std::uint64_t hash_ = 0;

    /// @brief Seeds the hash from the rasteriser.
    [[nodiscard]] static std::uint64_t seedFor(std::uint32_t rasteriser) noexcept;

    /// @brief Adds one stroke without checking it.
    void push(std::shared_ptr<const Stroke> stroke);
};

/// @brief Gives a shared, immutable, empty list of the current rasteriser.
///
/// The default of ::arraw::BrushMask::strokes.
[[nodiscard]] const std::shared_ptr<const StrokeList>& emptyStrokeList();

} // namespace arraw
