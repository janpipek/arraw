#pragma once

#include "BrushCoverage.h"
#include "DeviceCoverage.h"
#include "GpuContext.h"

#include <array>
#include <cstdint>

namespace arraw {

/// @brief Number of coverage textures the pointwise pass reads: four masks to a texture, sixteen
/// masks.
inline constexpr std::size_t coverageTextureCount = 4;

/// @brief The textures the pointwise pass reads its brush coverage from.
using CoverageTextures = std::array<DeviceImage, coverageTextureCount>;

/// @brief The packed coverage of a GPU ladder's source, on the ladder's device (ADR 044, section
/// 8).
///
/// Implements ::arraw::detail::DeviceCoverage for a ::arraw::detail::CoverageResidency. Holds one
/// RGBA8 texture for each plane the residency has, kept equal to the residency's host planes: a
/// texture is made whole when it does not exist, and otherwise only the tiles the residency
/// marked changed are copied into it. Belongs to the thread that owns its device.
class GpuCoverage final : public detail::DeviceCoverage {
public:
    /// @brief Constructs a copy with no textures.
    explicit GpuCoverage(DeviceId device) noexcept : DeviceCoverage(device) {}

    /// @brief Brings the textures up to the residency's planes.
    ///
    /// A plane with no texture, or whose texture is of another size, is uploaded whole; one with
    /// changed tiles has those copied in, in one batch; a texture past the residency's last
    /// plane is dropped. An upload that completed clears the plane's changed tiles in the
    /// residency, so that one which did not (a cancellation, a failure) is made again.
    /// @param context The device, the one this copy belongs to.
    /// @param residency Residency this copy is the device side of.
    /// @throws std::runtime_error if a transfer fails.
    void synchronise(GpuContext& context, detail::CoverageResidency& residency);

    /// @brief Gives the texture of a plane, empty if it has none.
    [[nodiscard]] const DeviceImage& texture(std::uint32_t plane) const noexcept {
        return textures_[plane];
    }

private:
    CoverageTextures textures_;
};

/// @brief Gives a ladder's residency its GPU textures, brought up to its planes.
///
/// Makes the residency's device side on the first call, or again when it belongs to another
/// device.
/// @param context The device the ladder lives on.
/// @param residency The ladder's residency, whose planes ::arraw::detail::CoverageResidency::update
/// has just brought up to the plan.
/// @param standIn Texture bound for each plane the plan does not use.
/// @return The four textures the pass reads.
[[nodiscard]] CoverageTextures coverageTexturesOf(GpuContext& context,
                                                  detail::CoverageResidency& residency,
                                                  const DeviceImage& standIn);

/// @brief Uploads whole the packed coverage of a render that goes through no ladder.
/// @param context The device to render on.
/// @param packed The planes, made for the call.
/// @param standIn Texture bound for each plane the plan does not use.
/// @return The four textures the pass reads.
[[nodiscard]] CoverageTextures coverageTexturesOf(GpuContext& context,
                                                  const detail::PackedCoverage& packed,
                                                  const DeviceImage& standIn);

} // namespace arraw
