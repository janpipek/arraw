#pragma once

#include "ProcessingPlan.h"

#include <DeviceImage.h>
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
