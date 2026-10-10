#pragma once

#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

#include <array>
#include <memory>
#include <optional>

namespace arraw {

namespace detail {
class CoverageResidency;
} // namespace detail

/// @brief The checkpoints one caller keeps of one source, one per pass boundary.
///
/// A render through ::arraw::resumeOrDevelop (or ::arraw::resumeOrDevelopOnGpu)
/// resumes from the deepest rung its plan still matches, drops every rung that
/// does not, and stores a rung at each boundary it passes before the last. The
/// engine decides what is kept and what is stale (ADR 011); the caller only
/// holds the ladder and clears it when something the plan cannot see changes,
/// such as the device. Bound to one source buffer, which it keeps alive so that
/// its address identifies it (until the plan has ADR 012's decode block).
///
/// Not thread-safe. A ladder holding resident rungs belongs to its device's
/// thread: render through it, clear it and destroy it there.
class CheckpointLadder {
public:
    /// @brief Makes an empty ladder.
    CheckpointLadder() = default;

    /// @brief Copies the rungs and the source, but not the brush coverage: a copy starts with
    /// none, so two ladders never write the same packed planes.
    CheckpointLadder(const CheckpointLadder& other);

    /// @brief Copies as the copy constructor does; this ladder's own coverage is dropped.
    CheckpointLadder& operator=(const CheckpointLadder& other);

    /// @brief Moves a ladder, its coverage included.
    CheckpointLadder(CheckpointLadder&&) noexcept = default;

    /// @brief Moves a ladder, its coverage included.
    CheckpointLadder& operator=(CheckpointLadder&&) noexcept = default;

    /// @brief Frees the ladder.
    ~CheckpointLadder() = default;

    /// @brief Drops every rung and the source.
    void clear() noexcept;

    /// @brief Tells whether it holds no rung.
    [[nodiscard]] bool empty() const noexcept;

    /// @brief Tells whether it holds a rung at a boundary.
    /// @param boundary Boundary to look at.
    [[nodiscard]] bool holds(Stage boundary) const noexcept;

private:
    friend struct LadderAccess;

    /// Source the rungs were made from.
    std::shared_ptr<const ImageBuffer> source_;
    /// Rung at each boundary, by stage.
    std::array<std::optional<RenderCheckpoint>, stageCount> rungs_;
    /// The packed brush coverage of the source, made on first use (ADR 044, section 8).
    std::shared_ptr<detail::CoverageResidency> coverage_;
};

/// @brief A render through a ladder: its result, and where it resumed from.
struct LadderRender {
    /// Result, at ::arraw::Stage::Effects.
    RenderCheckpoint checkpoint;
    /// Boundary of the rung resumed from; empty when it rendered from the source.
    std::optional<Stage> resumedFrom;
};

} // namespace arraw
