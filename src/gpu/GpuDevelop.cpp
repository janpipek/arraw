#include "GpuDevelop.h"

#include "CheckpointState.h"
#include "GpuPlan.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>

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
///
/// The plan's region is cut by the horizontal pass, which reads the image at an
/// offset and clamps to the region's own edges, with weights made for the
/// region's size: the same taps the host resamples a cut-out copy with, and no
/// copy. A region that is only cut runs the same passes with identity weights.
DeviceImage resizeOnGpu(GpuContext& context, const DeviceImage& image, const ResizePlan& resize) {
    const ImageSize size = resize.region.size();
    const ImageSize target = resize.outputSize;
    const std::array<std::uint32_t, 2> offset{resize.region.x, resize.region.y};
    const DeviceImage acrossWeights =
        context.upload(packResizeWeights(size.width, target.width, resize.filter));
    const DeviceImage downWeights =
        context.upload(packResizeWeights(size.height, target.height, resize.filter));

    const ImageSize widened{target.width, size.height};
    if (resize.opaque) {
        const GpuResizeBlock across{.plane = 0, .inputLength = size.width, .offset = offset};
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
                                   .inputLength = size.width,
                                   .offset = offset};
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

DeviceImage uploadSource(GpuContext& context, const ImageBuffer& source) {
    return source.format() == PixelFormat::RgbaF32 ? context.upload(source)
                                                   : context.upload(toRgbaF32(source));
}

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DevelopState& state, Stage stopAfter,
                              const RenderRequest& request) {
    // One path: the transfer this call pays for is the only difference.
    // Validating first keeps a bad argument from costing an upload.
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    const DeviceImage uploaded = uploadSource(context, source);
    return developOnGpu(context, source, uploaded, state, stopAfter, request);
}

namespace {

/// @brief Runs the passes after a boundary, up to another, on an image taken at the first.
///
/// The one path for every GPU development: a fresh one starts at the
/// pointwise pass, a resumed one after the boundary it resumes from, and each
/// pass is skipped under exactly the condition the CPU skips it.
/// @param context Device the images live on.
/// @param done Boundary @p image was taken at, or empty for the uploaded source.
/// @param image Source pixels, or the pixels at @p done.
/// @param plan The plan that makes the rest.
/// @param stopAfter Last boundary to run.
RenderCheckpoint runPasses(GpuContext& context, std::optional<Stage> done, DeviceImage image,
                           ProcessingPlan plan, Stage stopAfter) {
    if (!done) {
        const GpuPointwiseBlock pointwise = packPointwise(plan);
        image = context.render(GpuPass::Pointwise, bytesOf(pointwise), image, image.size(),
                               workingEncoding);
        done = Stage::Pointwise;
    }
    if (*done == Stage::Pointwise && stopAfter != Stage::Pointwise) {
        // As applyGeometry: an identity geometry leaves the developed pixels as
        // they are, rather than resampling them onto themselves.
        const GeometryPlan& geometry = *plan.geometry;
        if (!geometry.isIdentity()) {
            const GpuGeometryBlock block = packGeometry(geometry);
            image = context.render(GpuPass::Geometry, bytesOf(block), image, geometry.outputSize,
                                   workingEncoding);
        }
        done = Stage::Geometry;
    }
    if (*done == Stage::Geometry && stopAfter == Stage::Resize) {
        // As resample: a size equal to the cropped one is not resized, and the
        // pixels are reused as they are.
        if (!plan.resize->isIdentity(plan.geometry->outputSize)) {
            image = resizeOnGpu(context, image, *plan.resize);
        }
        done = Stage::Resize;
    }
    return makeCheckpoint(*done, std::move(plan), std::move(image));
}

} // namespace

RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DeviceImage& uploaded, const DevelopState& state,
                              Stage stopAfter, const RenderRequest& request) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    if (!uploaded.valid() || uploaded.device() != context.id()) {
        throw std::invalid_argument("The uploaded source does not belong to this device");
    }
    if (uploaded.size() != source.size()) {
        throw std::invalid_argument("The uploaded source is not of the source's size");
    }
    // Only a render that reaches the resize plans one: stopping earlier ignores
    // the request, whatever it says, and has no use for the opacity scan.
    ProcessingPlan plan =
        planFor(source, state, stopAfter == Stage::Resize ? request : RenderRequest{});
    return runPasses(context, std::nullopt, uploaded, std::move(plan), stopAfter);
}

RenderCheckpoint developOnGpu(GpuContext& context, const RenderCheckpoint& from,
                              const ImageBuffer& source, const DevelopState& state, Stage stopAfter,
                              const RenderRequest& request) {
    if (static_cast<std::size_t>(stopAfter) >= stageCount) {
        throw std::invalid_argument("A GPU development needs a recognised pass boundary");
    }
    const CheckpointState& held = stateOf(from);
    const auto* image = std::get_if<DeviceImage>(&held.pixels);
    if (image == nullptr) {
        throw std::invalid_argument("A checkpoint in host memory cannot be resumed on the GPU");
    }
    if (image->device() != context.id()) {
        throw std::invalid_argument("The checkpoint belongs to another device");
    }
    ProcessingPlan plan =
        planFor(source, state, stopAfter == Stage::Resize ? request : RenderRequest{});
    requireResumable(held, plan, source.size(), stopAfter);
    if (stopAfter == held.boundary) {
        return from;
    }
    return runPasses(context, held.boundary, *image, std::move(plan), stopAfter);
}

} // namespace arraw
