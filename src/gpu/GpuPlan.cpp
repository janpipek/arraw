#include "GpuPlan.h"

#include "GeometryPlan.h"
#include "ProcessingPlan.h"
#include "ResampleWeights.h"
#include "ToneCurve.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace arraw {

PointwiseProbe probeFor(Tap tap) {
    switch (tap) {
    case Tap::CurveInput:
        return PointwiseProbe::AfterTone;
    }
    throw std::invalid_argument("A sample needs a recognised tap");
}

bool probeReadsCurves(PointwiseProbe probe) {
    switch (probe) {
    case PointwiseProbe::AfterMatrix:
    case PointwiseProbe::AfterExposure:
    case PointwiseProbe::AfterTone:
        return false;
    case PointwiseProbe::Developed:
    case PointwiseProbe::AfterShoulder:
    case PointwiseProbe::AfterCurves:
        return true;
    }
    // An unknown probe is assumed to read them: the table costs an upload, never a wrong result.
    return true;
}

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
    const ColorAdjustmentPlan& colour = plan.colorAdjustments;
    block.convertsToGrayscale = colour.convertsToGrayscale ? 1U : 0U;
    block.saturation = colour.saturation;
    block.vibrance = colour.vibrance;
    block.adjustsSaturation = colour.adjustsSaturation ? 1U : 0U;
    block.adjustsVibrance = colour.adjustsVibrance ? 1U : 0U;
    block.adjustsHsl = colour.adjustsHsl ? 1U : 0U;
    block.hueShift = colour.hueShift;
    block.bandSaturation = colour.bandSaturation;
    block.bandLuminance = colour.bandLuminance;
    block.grayMix = colour.grayMix;
    const ColorGradingPlan& grading = colour.grading;
    block.grades = grading.active ? 1U : 0U;
    block.gradeBalanceShift = grading.balanceShift;
    block.gradeZoneWidth = grading.zoneWidth;
    block.gradeShadowMidtoneTint = {grading.shadowTint.a, grading.shadowTint.b,
                                    grading.midtoneTint.a, grading.midtoneTint.b};
    block.gradeHighlightTint = {grading.highlightTint.a, grading.highlightTint.b, 0.0F, 0.0F};
    const ToneCurvePlan& curves = plan.toneCurves;
    block.curvesLuma = curves.luma.active ? 1U : 0U;
    block.curvesRed = curves.red.active ? 1U : 0U;
    block.curvesGreen = curves.green.active ? 1U : 0U;
    block.curvesBlue = curves.blue.active ? 1U : 0U;
    block.probe = static_cast<std::uint32_t>(probe);
    return block;
}

namespace {

/// @brief A step split into a part with few mantissa bits and the remainder.
struct SplitStep {
    float high;
    float low;
};

/// @brief Rounds a value to 9 significant bits, and keeps what that dropped.
SplitStep splitStep(double value) {
    constexpr int bits = 9;
    if (value == 0.0) {
        return {0.0F, 0.0F};
    }
    int exponent = 0;
    const double mantissa = std::frexp(value, &exponent); // [0.5, 1) in magnitude
    const double high = std::ldexp(std::round(std::ldexp(mantissa, bits)), exponent - bits);
    return {static_cast<float>(high), static_cast<float>(value - high)};
}

} // namespace

GpuGeometryBlock packGeometry(const GeometryPlan& plan) {
    if (plan.outputSize.width > maxGeometryOutputExtent ||
        plan.outputSize.height > maxGeometryOutputExtent) {
        throw std::invalid_argument("The geometry pass is exact only up to 16384 pixels per side");
    }
    // Composed in double, as applyGeometry evaluates it: upright position
    // left + (x + 0.5) * width / outputWidth, and likewise down, taken back
    // through toSource, which is linear.
    const double columnScale = plan.width / plan.outputSize.width;
    const double rowScale = plan.height / plan.outputSize.height;
    const SourcePoint origin = plan.toSource({plan.left, plan.top});
    const auto& m = plan.matrix;

    GpuGeometryBlock block;
    const double originValues[2] = {origin.x, origin.y};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double whole = std::floor(originValues[axis]);
        auto fraction = static_cast<float>(originValues[axis] - whole);
        auto wholeInt = static_cast<std::int32_t>(whole);
        if (fraction >= 1.0F) { // Rounded up to one: carry it.
            fraction = 0.0F;
            ++wholeInt;
        }
        block.originWhole[axis] = wholeInt;
        block.originFraction[axis] = fraction;
    }
    // toSource applies the transpose, so a column of output moves the source
    // along the matrix's first row and a row of output along its second.
    const double columnStep[2] = {m[0] * columnScale, m[1] * columnScale};
    const double rowStep[2] = {m[2] * rowScale, m[3] * rowScale};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const SplitStep column = splitStep(columnStep[axis]);
        const SplitStep row = splitStep(rowStep[axis]);
        block.columnStepHigh[axis] = column.high;
        block.columnStepLow[axis] = column.low;
        block.rowStepHigh[axis] = row.high;
        block.rowStepLow[axis] = row.low;
    }
    block.sourceSize = {plan.sourceSize.width, plan.sourceSize.height};
    block.outputSize = {plan.outputSize.width, plan.outputSize.height};
    return block;
}

ImageBuffer packToneCurves(const ToneCurvePlan& curves) {
    ImageBuffer image({static_cast<std::uint32_t>(toneCurveSamples), 1}, workingFormat,
                      workingEncoding);
    const auto samples = image.samples<float>();
    for (std::size_t index = 0; index < toneCurveSamples; ++index) {
        samples[index * 4] = curves.luma.table[index];
        samples[index * 4 + 1] = curves.red.table[index];
        samples[index * 4 + 2] = curves.green.table[index];
        samples[index * 4 + 3] = curves.blue.table[index];
    }
    return image;
}

ImageBuffer packResizeWeights(std::uint32_t in, std::uint32_t out, ResizeFilter filter) {
    const AxisWeights axis = axisWeights(in, out, filter);
    std::size_t widest = 0;
    for (const Taps& taps : axis.taps) {
        widest = std::max(widest, taps.count);
    }
    const auto width = static_cast<std::uint32_t>(1 + (widest + 3) / 4);
    ImageBuffer image({width, out}, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::size_t row = 0; row < axis.taps.size(); ++row) {
        const Taps& taps = axis.taps[row];
        float* texels = &samples[row * width * 4];
        texels[0] = static_cast<float>(taps.first);
        texels[1] = static_cast<float>(taps.count);
        for (std::size_t k = 0; k < taps.count; ++k) {
            texels[4 + k] = static_cast<float>(axis.weights[taps.offset + k]);
        }
    }
    return image;
}

} // namespace arraw
