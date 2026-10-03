#pragma once

#include "DeviceImage.h"
#include "ProcessingPlan.h"

#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

#include <variant>

namespace arraw {

/// @brief A checkpoint's pixels, wherever they happen to live.
///
/// The one place the two backends differ, and deliberately the only one: a
/// developed result is the same thing whether it is a host raster or a texture,
/// and ADR 011's validity rule applies to it either way.
using CheckpointPixels = std::variant<ImageBuffer, DeviceImage>;

/// @brief Everything a ::arraw::RenderCheckpoint is, on the engine's side of it.
///
/// Held behind a `shared_ptr<const>` so that a checkpoint is immutable and
/// cheap to copy, and so that the public header carries neither a
/// ::arraw::ProcessingPlan, which is engine-private, nor anything that names a
/// graphics device.
struct CheckpointState {
    /// @brief Pass boundary these pixels were taken at.
    Stage boundary = Stage::Pointwise;

    /// @brief The plan they were made by, kept whole and compared by prefix.
    ///
    /// A stored copy and `==` is exact, needs no hash function, and cannot
    /// drift from what actually executed; next to the pixels, a few hundred
    /// bytes of plan is free (ADR 011).
    ProcessingPlan plan;

    /// @brief The pixels themselves.
    CheckpointPixels pixels;

    /// @brief Pixel dimensions of the pixels, whichever alternative holds them.
    [[nodiscard]] ImageSize size() const noexcept;

    /// @brief Meaning of the pixels' RGB sample values.
    [[nodiscard]] const ColorEncoding& encoding() const;

    /// @brief Whether the pixels live on a graphics device.
    [[nodiscard]] bool isResident() const noexcept;

    /// @brief Copies the pixels into host memory, transferring or cloning.
    [[nodiscard]] ImageBuffer readBack() const;
};

/// @brief Gives the engine-side state of a checkpoint.
///
/// For the backends that resume from one. The plan prefix stays out of a
/// caller's reach by this being an `src/` header, not by anything stronger.
/// @param checkpoint Checkpoint to look into; the result lives as long as it does.
[[nodiscard]] const CheckpointState& stateOf(const RenderCheckpoint& checkpoint) noexcept;

/// @brief Refuses a resume that a checkpoint cannot serve.
///
/// The one place the validity rule is applied, for both backends (ADR 011): the
/// checkpoint's plan must equal the new one up to its own boundary, and the run
/// must not stop before it. The plan carries the source's size and orientation
/// (in the geometry group) and its encoding (in the pointwise one), but not yet
/// a file or a stamp (ADR 012's decode block), so for a pointwise checkpoint,
/// whose boundary precedes the geometry, the pixels' size is also checked
/// against the source's. Two sources of one size and encoding are not told
/// apart here; a caller that swaps one for the other must drop its checkpoints.
/// @param from Checkpoint to resume from.
/// @param plan Plan of the render being resumed, resolved for @p stopAfter.
/// @param sourceSize Size of the source the render develops.
/// @param stopAfter Last boundary the render runs.
/// @throws std::invalid_argument if @p stopAfter is not a boundary or is before
/// the checkpoint's, the plan prefixes differ, or the pixels are not what the
/// plan would have made at that boundary.
void requireResumable(const CheckpointState& from, const ProcessingPlan& plan, ImageSize sourceSize,
                      Stage stopAfter);

/// @brief Builds a checkpoint, refusing states that could not have been rendered.
/// @param boundary Pass boundary the pixels were taken at.
/// @param plan Plan that made them.
/// @param pixels The pixels, on the host or on a device.
/// @return A checkpoint sharing one immutable payload.
/// @throws std::invalid_argument if @p boundary is unknown, or @p pixels is
/// empty or has incomplete storage.
[[nodiscard]] RenderCheckpoint makeCheckpoint(Stage boundary, ProcessingPlan plan,
                                              CheckpointPixels pixels);

} // namespace arraw
