#include "GpuDevelop.h"

#include "CheckpointState.h"
#include "GpuPlan.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"

#include <cstddef>
#include <span>
#include <stdexcept>

namespace arraw {

namespace {

/// @brief Views a uniform block as the bytes a pass takes.
template <typename Block> std::span<const std::byte> bytesOf(const Block& block) {
    return std::as_bytes(std::span(&block, 1));
}

} // namespace

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DevelopSettings& settings, Stage stopAfter) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    ProcessingPlan plan = planFor(source, settings);

    const DeviceImage uploaded = source.format() == PixelFormat::RgbaF32
                                     ? context.upload(source)
                                     : context.upload(toRgbaF32(source));
    const GpuPointwiseBlock pointwise = packPointwise(plan);
    DeviceImage developed = context.render(GpuPass::Pointwise, bytesOf(pointwise), uploaded,
                                           source.size(), workingEncoding);
    if (stopAfter == Stage::Pointwise) {
        return makeCheckpoint(Stage::Pointwise, std::move(plan), std::move(developed));
    }

    // As applyGeometry: an identity geometry leaves the developed pixels as
    // they are, rather than resampling them onto themselves.
    const GeometryPlan& geometry = *plan.geometry;
    if (geometry.isIdentity()) {
        return makeCheckpoint(Stage::Geometry, std::move(plan), std::move(developed));
    }
    const GpuGeometryBlock block = packGeometry(geometry);
    DeviceImage framed = context.render(GpuPass::Geometry, bytesOf(block), developed,
                                        geometry.outputSize, workingEncoding);
    return makeCheckpoint(Stage::Geometry, std::move(plan), std::move(framed));
}

} // namespace arraw
