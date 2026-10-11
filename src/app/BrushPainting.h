#pragma once

#include "MaskEditing.h"

#include <BrushStrokes.h>
#include <DevelopState.h>
#include <LocalAdjustments.h>

#include <QPointF>
#include <QSettings>
#include <QString>

#include <optional>

namespace arraw::app {

/// @brief Smallest brush flow.
inline constexpr float minimumBrushFlow = 0.01F;

/// @brief Factor a brush radius grows by for one step of ::arraw::app::resized.
inline constexpr float brushResizeFactor = 1.25F;

/// @brief Amount hardness changes by for one step of ::arraw::app::hardened.
inline constexpr float brushHardnessStep = 0.1F;

/// @brief Settings of the brush tool: view state, never in the document (ADR 044, section 1).
struct BrushSettings {
    float radius = 0.02F;  ///< Long-edge units, ::arraw::minimumBrushRadius to maximumBrushRadius.
    float hardness = 0.5F; ///< 0 to 1.
    float flow = 1.0F;     ///< ::arraw::app::minimumBrushFlow to 1.
    bool erase = false;    ///< The Erase toggle; Alt at a press inverts it for that stroke.

    friend bool operator==(const BrushSettings&, const BrushSettings&) = default;
};

/// @brief Reads the brush settings, using the defaults for missing or invalid values.
///
/// Radius, hardness and flow are kept under `brush/`, clamped to their ranges; Erase is never
/// stored.
[[nodiscard]] BrushSettings restoreBrushSettings(QSettings& store);

/// @brief Stores the brush settings' radius, hardness and flow.
void saveBrushSettings(const BrushSettings& settings, QSettings& store);

/// @brief Gives the settings with the radius multiplied by 1.25 per step, clamped.
/// @param steps Steps to grow by; negative to shrink.
[[nodiscard]] BrushSettings resized(BrushSettings settings, int steps);

/// @brief Gives the settings with the hardness moved by 0.1 per step, clamped.
/// @param steps Steps to harden by; negative to soften.
[[nodiscard]] BrushSettings hardened(BrushSettings settings, int steps);

/// @brief Why a stroke cannot be painted, in the order the checks run.
enum class BrushRefusal { None, MasksFull, NotABrush, Strokes, Points, SweptArea, Dabs };

/// @brief Tells which cap or budget refuses a stroke on a mask.
/// @param state State holding the mask.
/// @param id Mask the stroke would be appended to.
/// @param stroke Stroke to append.
/// @return ::arraw::app::BrushRefusal::NotABrush if @p id is not a brush of @p state or its
/// rasteriser is not the current one, the first cap or budget the mask would pass, or None when it
/// fits. A brush without a stroke list counts as empty, as in the core.
[[nodiscard]] BrushRefusal refusalOf(const DevelopState& state, LocalAdjustmentId id,
                                     const Stroke& stroke);

/// @brief Makes the stroke that stands for "one more stroke" at some settings: two points one
/// radius apart at a position, so it spends swept area and dabs as a real stroke does.
/// @param settings Style of the stroke.
/// @param at Where it starts.
[[nodiscard]] Stroke probeStroke(const BrushSettings& settings, SensorPoint at);

/// @brief Words a refusal for the status line.
/// @param refusal Reason.
/// @param maskName Name of the mask the stroke was meant for.
/// @param midStroke Whether the stroke was already under way.
/// @return The message; empty for None and NotABrush, which are never shown.
[[nodiscard]] QString describeRefusal(BrushRefusal refusal, const QString& maskName,
                                      bool midStroke = false);

/// @brief One stroke in progress: the states a gesture reports, from its press to its release.
///
/// Consecutive states share every earlier stroke by pointer and each live stroke's points are a
/// prefix of the next one's, which is what the coverage cache grows from.
class LiveStroke {
public:
    /// @brief Starts at a press: on a mask, or on a new brush when none is given.
    /// @param baseline State the stroke starts from.
    /// @param target Brush to paint on; empty to add a new one.
    /// @param settings Radius, hardness and flow, captured for the stroke.
    /// @param erase Whether this stroke erases (the toggle, inverted by Alt, decided by the
    /// caller).
    /// @param press Pointer position, in widget logical pixels.
    /// @param mapping Where the photograph lies in the widget.
    /// @param refused Set to the reason when no stroke can start.
    /// @return The stroke, with one point; or nothing when the probe stroke does not fit.
    [[nodiscard]] static std::optional<LiveStroke>
    begin(const DevelopState& baseline, std::optional<LocalAdjustmentId> target,
          const BrushSettings& settings, bool erase, QPointF press, const MaskViewMapping& mapping,
          BrushRefusal& refused);

    /// @brief Adds a pointer position, if it is far enough from the last one kept.
    ///
    /// A caller reports state() whenever pendingReport() before a call that finds atPointCap(),
    /// so that the stroke the split finishes is the one already reported; otherwise the next
    /// render redraws the whole brush.
    /// @param pointer Position, in widget logical pixels.
    /// @param mapping Where the photograph lies in the widget now.
    /// @param release Whether this is the release: kept however close, unless it repeats the
    /// last point.
    /// @return Whether state() changed.
    bool extend(QPointF pointer, const MaskViewMapping& mapping, bool release = false);

    /// @brief Records that state() was reported, so a split at the point cap builds on it.
    void reported() noexcept {
        reported_ = true;
    }

    /// @brief Tells whether state() changed since it was last reported.
    [[nodiscard]] bool pendingReport() const noexcept {
        return !reported_;
    }

    /// @brief Tells whether the next point kept ends this stroke and starts another.
    ///
    /// A caller with a pending report reports it first (::arraw::app::LiveStroke::pendingReport),
    /// so that the finished stroke is the one the cache has seen.
    [[nodiscard]] bool atPointCap() const noexcept {
        return live_.points.size() >= maximumStrokePoints;
    }

    /// @brief Gives the baseline with every finished part and the live part appended.
    ///
    /// Built on the first call after a change, not at every extend.
    [[nodiscard]] const DevelopState& state() const;

    /// @brief Gives the mask being painted.
    [[nodiscard]] LocalAdjustmentId mask() const noexcept {
        return id_;
    }

    /// @brief Gives the reason a budget stopped the stroke, or None; later positions are ignored.
    [[nodiscard]] BrushRefusal refusal() const noexcept {
        return refusal_;
    }

private:
    LiveStroke() = default;

    DevelopState base_;              ///< Baseline plus the finished parts.
    mutable DevelopState state_;     ///< Cache of base_ plus live_.
    mutable bool stateStale_ = true; ///< Whether state_ lacks the latest change.
    LocalAdjustmentId id_{};
    Stroke live_;                 ///< Live part, with the style and points the core stores.
    StrokeBudget baseBudget_;     ///< What the strokes of base_'s brush spend.
    std::size_t baseStrokes_ = 0; ///< Number of strokes of base_'s brush.
    double liveLength_ = 0.0;     ///< Path length of live_, summed as the core does.
    QPointF lastKept_;
    BrushRefusal refusal_ = BrushRefusal::None;
    bool reported_ = false;
};

} // namespace arraw::app
