#include "GpuCoverage.h"

#include "TimingTrace.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

namespace arraw {

void GpuCoverage::synchronise(GpuContext& context, detail::CoverageResidency& residency) {
    const detail::PackedCoverage& packed = residency.packed();
    const auto planes = static_cast<std::uint32_t>(packed.planes.size());
    if (planes > coverageTextureCount) {
        throw std::logic_error("A residency holds more coverage planes than the GPU reads");
    }
    for (std::uint32_t plane = planes; plane < coverageTextureCount; ++plane) {
        textures_[plane] = DeviceImage();
        residency.clearPending(plane);
    }
    for (std::uint32_t plane = 0; plane < planes; ++plane) {
        DeviceImage& texture = textures_[plane];
        if (!texture.valid() || !(texture.size() == packed.size)) {
            // Whole: the texture does not exist, or is for another size.
            texture = context.uploadCoverage(packed.size, packed.planes[plane]);
            countPlane();
            residency.clearPending(plane);
        } else if (residency.hasPending(plane)) {
            const std::vector<PixelRect> changed = residency.pending(plane);
            context.updateCoverage(texture, changed, packed.planes[plane]);
            countRectangles(changed.size());
            residency.clearPending(plane);
        }
    }
}

CoverageTextures coverageTexturesOf(GpuContext& context, detail::CoverageResidency& residency,
                                    const DeviceImage& standIn) {
    const detail::TimingSpan timing("gpu.coverage");
    auto* held = dynamic_cast<GpuCoverage*>(residency.device());
    if (held == nullptr || held->device() != context.id()) {
        residency.setDevice(std::make_unique<GpuCoverage>(context.id()));
        held = &static_cast<GpuCoverage&>(*residency.device());
        // Whatever the old textures held, the new ones start from the planes: every plane is
        // uploaded whole below, and the changed tiles it had pending are no longer owed.
    }
    held->synchronise(context, residency);
    CoverageTextures textures;
    for (std::uint32_t plane = 0; plane < coverageTextureCount; ++plane) {
        textures[plane] = held->texture(plane).valid() ? held->texture(plane) : standIn;
    }
    return textures;
}

CoverageTextures coverageTexturesOf(GpuContext& context, const detail::PackedCoverage& packed,
                                    const DeviceImage& standIn) {
    const detail::TimingSpan timing("gpu.coverage");
    if (packed.planes.size() > coverageTextureCount) {
        throw std::logic_error("A plan holds more coverage planes than the GPU reads");
    }
    CoverageTextures textures;
    for (std::uint32_t plane = 0; plane < coverageTextureCount; ++plane) {
        textures[plane] = plane < packed.planes.size()
                              ? context.uploadCoverage(packed.size, packed.planes[plane])
                              : standIn;
    }
    return textures;
}

} // namespace arraw
