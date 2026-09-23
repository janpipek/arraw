#pragma once

#include <ImageBuffer.h>

#include <cstddef>
#include <memory>

namespace arraw {

/// @brief Pass boundary that a checkpoint's pixels were taken at.
///
/// A pass boundary is a place where a buffer exists anyway (ADR 011), so this
/// names the passes that exist rather than every stage that one day will. It
/// grows as spatial stages arrive: decode, lens, spots and noise join it when
/// each becomes a pass with its parameters in the plan (ADR 012).
enum class Stage {
    Pointwise, ///< After the fused pointwise chain, before any geometry.
    Geometry,  ///< After the resample into the upright, cropped frame.
};

/// @brief Number of pass boundaries, for the prefix fold to iterate over.
inline constexpr std::size_t stageCount = 2;

class CheckpointState;

/// @brief Pixels from a pass boundary, with the plan prefix that made them.
///
/// What `stopAfter` hands back and `resumeFrom` takes (ADR 011). Its payload is
/// an ::arraw::ImageBuffer on the CPU backend and a graphics-device image on the
/// GPU one, so that keeping a developed result on the device across a geometry
/// edit is the same idea, with the same validity rule, as keeping a buffer.
///
/// Provenance lives here and not on the pixels: ::arraw::ImageBuffer is a plain
/// raster by ADR 001, and one that knew which pipeline made it would be the
/// partially populated state that ADR forbids.
///
/// The plan prefix is deliberately not reachable from here. A checkpoint is
/// validated by the engine comparing plans, never by a caller reading one out
/// and deciding for itself; that is what keeps the rule in one place.
///
/// Immutable, and cheap to copy: copies share one payload.
class RenderCheckpoint {
public:
    /// @brief Wraps engine-side state that a caller cannot construct.
    /// @param state Boundary, plan prefix and pixels, as the engine resolved them.
    /// @throws std::invalid_argument if @p state is null, its boundary is unknown,
    /// or its pixels are empty or incomplete.
    explicit RenderCheckpoint(std::shared_ptr<const CheckpointState> state);

    /// @brief Reports where in the pipeline these pixels were taken.
    [[nodiscard]] Stage boundary() const noexcept;

    /// @brief Pixel dimensions of the checkpoint's pixels.
    [[nodiscard]] ImageSize size() const noexcept;

    /// @brief Meaning of the checkpoint's RGB sample values.
    ///
    /// A checkpoint declares its own encoding rather than leaving its numbers
    /// to mean whatever its caller assumed (ADR 011).
    [[nodiscard]] const ColorEncoding& encoding() const;

    /// @brief Whether the pixels live on a graphics device rather than in host memory.
    [[nodiscard]] bool isResident() const noexcept;

    /// @brief Copies the pixels into host memory.
    ///
    /// Always a copy, and never a cheap one: a resident checkpoint pays a
    /// device transfer, and a host-resident one pays a clone, because the
    /// payload is shared and immutable. Export, diagnostics and the comparison
    /// tests are what this is for; a preview should present the checkpoint
    /// rather than read it back (reimplementation plan, section 12).
    /// @return A new buffer holding the checkpoint's pixels.
    /// @throws std::runtime_error if a device transfer fails.
    [[nodiscard]] ImageBuffer readBack() const;

private:
    std::shared_ptr<const CheckpointState> state_;
};

} // namespace arraw
