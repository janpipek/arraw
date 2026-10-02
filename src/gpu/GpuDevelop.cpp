#include "GpuDevelop.h"

#include "CheckpointState.h"
#include "GpuPlan.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace arraw {

namespace {

/// @brief Views a uniform block as the bytes a pass takes.
template <typename Block> std::span<const std::byte> bytesOf(const Block& block) {
    return std::as_bytes(std::span(&block, 1));
}

/// @brief Resizes a device image, as ::arraw::resample does on the host.
///
/// Four renders in general: the horizontal shader runs once per plane over the
/// image, and the vertical one reads the three planes and writes the result.
/// An opaque image needs no planes beyond the sums, so it takes two.
DeviceImage resizeOnGpu(GpuContext& context, const DeviceImage& image, const ResizePlan& resize) {
    const ImageSize size = image.size();
    const ImageSize target = resize.outputSize;
    const DeviceImage acrossWeights =
        context.upload(packResizeWeights(size.width, target.width, resize.filter));
    const DeviceImage downWeights =
        context.upload(packResizeWeights(size.height, target.height, resize.filter));

    const ImageSize widened{target.width, size.height};
    if (resize.opaque) {
        const GpuResizeBlock across{.plane = 0, .inputLength = size.width};
        const GpuResizeBlock down{.plane = 0, .inputLength = size.height};
        const DeviceImage sums =
            context.render(GpuPass::ResizeAcrossOpaque, bytesOf(across),
                           std::array{image, acrossWeights}, widened, workingEncoding);
        return context.render(GpuPass::ResizeDownOpaque, bytesOf(down),
                              std::array{sums, downWeights}, target, workingEncoding);
    }

    std::array<DeviceImage, resizePlaneCount> planes;
    for (std::size_t plane = 0; plane < resizePlaneCount; ++plane) {
        const GpuResizeBlock block{.plane = static_cast<std::uint32_t>(plane),
                                   .inputLength = size.width};
        const std::array inputs{image, acrossWeights};
        planes[plane] =
            context.render(GpuPass::ResizeAcross, bytesOf(block), inputs, widened, workingEncoding);
    }
    const GpuResizeBlock block{.plane = 0, .inputLength = size.height};
    const std::array inputs{planes[static_cast<std::size_t>(ResizePlane::Sums)], downWeights,
                            planes[static_cast<std::size_t>(ResizePlane::Low)],
                            planes[static_cast<std::size_t>(ResizePlane::High)]};
    return context.render(GpuPass::ResizeDown, bytesOf(block), inputs, target, workingEncoding);
}

} // namespace

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DevelopSettings& settings, Stage stopAfter,
                              const RenderRequest& request) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    // Only a render that reaches the resize plans one: stopping earlier ignores
    // the request, whatever it says, and has no use for the opacity scan.
    ProcessingPlan plan =
        planFor(source, settings, stopAfter == Stage::Resize ? request : RenderRequest{});

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
    DeviceImage framed = std::move(developed);
    if (!geometry.isIdentity()) {
        const GpuGeometryBlock block = packGeometry(geometry);
        framed = context.render(GpuPass::Geometry, bytesOf(block), framed, geometry.outputSize,
                                workingEncoding);
    }
    if (stopAfter == Stage::Geometry) {
        return makeCheckpoint(Stage::Geometry, std::move(plan), std::move(framed));
    }

    // As resample: a size equal to the cropped one is not resized, and the
    // pixels are reused as they are.
    if (plan.resize->isIdentity(geometry.outputSize)) {
        return makeCheckpoint(Stage::Resize, std::move(plan), std::move(framed));
    }
    DeviceImage resized = resizeOnGpu(context, framed, *plan.resize);
    return makeCheckpoint(Stage::Resize, std::move(plan), std::move(resized));
}

} // namespace arraw
