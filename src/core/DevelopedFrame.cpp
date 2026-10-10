#include "GeometryPlan.h"
#include "LocalPlan.h"

#include <DevelopedFrame.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <variant>

using namespace arraw;

DevelopedFrameMap::DevelopedFrameMap(SourceShape source, const GeometrySettings& geometry)
    : source_(source) {
    // Throws for a geometry that is not valid, with the same words as the crop rules.
    static_cast<void>(cropFrameFor(source, geometry));
    const GeometryPlan plan = geometryPlanFor(source.size, source.orientation, geometry);
    const double sourceWidth = source.size.width;
    const double sourceHeight = source.size.height;
    width_ = plan.width;
    height_ = plan.height;
    longEdge_ = std::max(sourceWidth, sourceHeight);

    // corrected (u, v) -> source pixels (u W, v H) -> upright -> crop-relative fractions.
    const auto& m = plan.matrix;
    forward_ = {m[0] * sourceWidth / width_, m[1] * sourceHeight / width_,
                m[2] * sourceWidth / height_, m[3] * sourceHeight / height_};
    const UprightPoint origin = plan.toUpright({0.0, 0.0});
    shift_ = {(origin.x - plan.left) / width_, (origin.y - plan.top) / height_};
    const double determinant = forward_[0] * forward_[3] - forward_[1] * forward_[2];
    backward_ = {forward_[3] / determinant, -forward_[1] / determinant, -forward_[2] / determinant,
                 forward_[0] / determinant};
}

DevelopedPoint DevelopedFrameMap::developedFrom(CorrectedPosition position) const noexcept {
    return {forward_[0] * position.u + forward_[1] * position.v + shift_[0],
            forward_[2] * position.u + forward_[3] * position.v + shift_[1]};
}

CorrectedPosition DevelopedFrameMap::correctedFrom(DevelopedPoint point) const noexcept {
    const double x = point.x - shift_[0];
    const double y = point.y - shift_[1];
    return {backward_[0] * x + backward_[1] * y, backward_[2] * x + backward_[3] * y};
}

LongEdgePoint DevelopedFrameMap::longEdgeFrom(CorrectedPosition position) const noexcept {
    return {position.u * source_.size.width / longEdge_,
            position.v * source_.size.height / longEdge_};
}

CorrectedPosition DevelopedFrameMap::correctedFromLongEdge(LongEdgePoint point) const noexcept {
    return {point.x * longEdge_ / source_.size.width, point.y * longEdge_ / source_.size.height};
}

MaskCoverage arraw::maskCoverage(const LocalAdjustment& adjustment, const DevelopedFrameMap& frame,
                                 DevelopedRegion region, ImageSize size) {
    if (size.empty() || size.width > maximumCoverageSide || size.height > maximumCoverageSide) {
        throw std::invalid_argument("A coverage grid must be 1 to 4096 cells on each side");
    }
    if (!std::isfinite(region.left) || !std::isfinite(region.top) || !std::isfinite(region.width) ||
        !std::isfinite(region.height) || !(region.width > 0.0) || !(region.height > 0.0)) {
        throw std::invalid_argument("A coverage region must be finite and not empty");
    }
    if (std::holds_alternative<BrushMask>(adjustment.shape)) {
        throw std::invalid_argument("The coverage of a brush mask is not drawn yet");
    }
    const LocalMaskPlan mask =
        resolvedMask(normalised(adjustment.shape), adjustment.invert, frame.source().size);

    MaskCoverage coverage;
    coverage.size = size;
    coverage.weights.resize(size.pixelCount());
    const double sourceWidth = frame.source().size.width;
    const double sourceHeight = frame.source().size.height;
    std::uint8_t* out = coverage.weights.data();
    // The map is affine: a row start and a step per cell, instead of a map per cell.
    const double stepX = region.width / size.width;
    const double stepY = region.height / size.height;
    const CorrectedPosition origin =
        frame.correctedFrom({region.left + stepX / 2.0, region.top + stepY / 2.0});
    const CorrectedPosition across =
        frame.correctedFrom({region.left + stepX / 2.0 + stepX, region.top + stepY / 2.0});
    const CorrectedPosition down =
        frame.correctedFrom({region.left + stepX / 2.0, region.top + stepY / 2.0 + stepY});
    const double acrossU = (across.u - origin.u) * sourceWidth;
    const double acrossV = (across.v - origin.v) * sourceHeight;
    const double downU = (down.u - origin.u) * sourceWidth;
    const double downV = (down.v - origin.v) * sourceHeight;
    for (std::uint32_t row = 0; row < size.height; ++row) {
        const double rowU = origin.u * sourceWidth + row * downU;
        const double rowV = origin.v * sourceHeight + row * downV;
        for (std::uint32_t column = 0; column < size.width; ++column) {
            const float weight =
                maskWeight(mask, static_cast<float>(rowU + column * acrossU),
                           static_cast<float>(rowV + column * acrossV), PixelCoverage{});
            // Round half up: the weight is never negative.
            *out++ = static_cast<std::uint8_t>(std::clamp(weight, 0.0F, 1.0F) * 255.0F + 0.5F);
        }
    }
    return coverage;
}
