#include "GpuPlan.h"

#include "GeometryPlan.h"
#include "ProcessingPlan.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace arraw {

GpuPointwiseBlock packPointwise(const ProcessingPlan& plan, PointwiseProbe probe) {
    GpuPointwiseBlock block;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            block.toWorking[row * 4 + column] = plan.toWorking.at(row, column);
        }
    }
    block.exposureGain = plan.exposureGain;
    block.contrastSlope = plan.contrastSlope;
    block.contrastScale = plan.contrastScale;
    block.shadowShift = plan.shadowShift;
    block.highlightShift = plan.highlightShift;
    block.blackShift = plan.blackShift;
    block.whiteShift = plan.whiteShift;
    block.shapesTone = plan.shapesTone ? 1U : 0U;
    // A knee no luminance reaches is how the plan says "no shoulder"; the
    // shader is told so outright rather than trusted with an infinity. The
    // shader's comparison is the CPU's `!(luminance > knee)`, so a NaN
    // luminance passes through unbent on both.
    const bool rolls = std::isfinite(plan.shoulderKnee);
    block.rollsHighlights = rolls ? 1U : 0U;
    block.shoulderKnee = rolls ? plan.shoulderKnee : 0.0F;
    block.probe = static_cast<std::uint32_t>(probe);
    return block;
}

GpuGeometryBlock packGeometry(const GeometryPlan& plan) {
    // Composed in double, as applyGeometry evaluates it: upright position
    // left + (x + 0.5) * width / outputWidth, and likewise down, taken back
    // through toSource, which is linear.
    const double columnScale = plan.width / plan.outputSize.width;
    const double rowScale = plan.height / plan.outputSize.height;
    const SourcePoint origin = plan.toSource({plan.left, plan.top});
    const auto& m = plan.matrix;

    GpuGeometryBlock block;
    block.origin = {static_cast<float>(origin.x), static_cast<float>(origin.y)};
    // toSource applies the transpose, so a column of output moves the source
    // along the matrix's first row and a row of output along its second.
    block.columnStep = {static_cast<float>(m[0] * columnScale),
                        static_cast<float>(m[1] * columnScale)};
    block.rowStep = {static_cast<float>(m[2] * rowScale), static_cast<float>(m[3] * rowScale)};
    block.sourceSize = {plan.sourceSize.width, plan.sourceSize.height};
    block.outputSize = {plan.outputSize.width, plan.outputSize.height};
    return block;
}

} // namespace arraw
