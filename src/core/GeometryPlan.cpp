#include "GeometryPlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>

using namespace arraw;

namespace {

using Matrix = std::array<double, 4>;

/// @brief Composes two linear transforms, applying the inner one first.
Matrix compose(const Matrix& outer, const Matrix& inner) {
    return {outer[0] * inner[0] + outer[1] * inner[2], outer[0] * inner[1] + outer[1] * inner[3],
            outer[2] * inner[0] + outer[3] * inner[2], outer[2] * inner[1] + outer[3] * inner[3]};
}

/// @brief Resolves camera orientation without trigonometric rounding.
Matrix orientationMatrix(ImageOrientation orientation) {
    switch (orientation) {
    case ImageOrientation::Normal:
        return {1, 0, 0, 1};
    case ImageOrientation::MirrorHorizontal:
        return {-1, 0, 0, 1};
    case ImageOrientation::Rotate180:
        return {-1, 0, 0, -1};
    case ImageOrientation::MirrorVertical:
        return {1, 0, 0, -1};
    case ImageOrientation::Transpose:
        return {0, 1, 1, 0};
    case ImageOrientation::Rotate90:
        return {0, -1, 1, 0};
    case ImageOrientation::Transverse:
        return {0, -1, -1, 0};
    case ImageOrientation::Rotate270:
        return {0, 1, -1, 0};
    }
    throw std::invalid_argument("Unknown image orientation");
}

/// @brief Resolves a user quarter-turn without trigonometric rounding.
Matrix rotationMatrix(QuarterTurn rotation) {
    switch (rotation) {
    case QuarterTurn::None:
        return {1, 0, 0, 1};
    case QuarterTurn::Clockwise90:
        return {0, -1, 1, 0};
    case QuarterTurn::Clockwise180:
        return {-1, 0, 0, -1};
    case QuarterTurn::Clockwise270:
        return {0, 1, -1, 0};
    }
    throw std::invalid_argument("Unknown quarter-turn rotation");
}

/// @brief Continuous rectangle dimensions.
struct Extent {
    double width;
    double height;
};

/// @brief Source-axis extents of an upright rectangle.
Extent sourceExtent(const GeometryPlan& plan, Extent extent) {
    const auto& m = plan.matrix;
    return {std::abs(m[0]) * extent.width + std::abs(m[2]) * extent.height,
            std::abs(m[1]) * extent.width + std::abs(m[3]) * extent.height};
}

/// @brief Finds the largest centred rectangle at a fixed aspect.
Extent atAspect(const GeometryPlan& plan, double ratio) {
    const Extent unit{std::min(1.0, ratio), std::min(1.0, 1.0 / ratio)};
    const auto extent = sourceExtent(plan, unit);
    const double scale =
        std::min(plan.sourceSize.width / extent.width, plan.sourceSize.height / extent.height);
    return {unit.width * scale, unit.height * scale};
}

/// @brief Finds the largest-area axis-aligned rectangle in a rotated rectangle.
Extent largestFree(const GeometryPlan& plan) {
    // Symmetry guarantees a centred optimum. The two source dimensions give
    // two linear bounds on width and height. Area peaks either at their
    // intersection or at the stationary point on one of the bounds.
    const double a = std::abs(plan.matrix[0]);
    const double b = std::abs(plan.matrix[2]);
    const double c = std::abs(plan.matrix[1]);
    const double d = std::abs(plan.matrix[3]);
    const double w = plan.sourceSize.width;
    const double h = plan.sourceSize.height;
    Extent best{0, 0};
    const auto consider = [&](double width, double height) {
        if (width <= 0 || height <= 0 || !std::isfinite(width) || !std::isfinite(height)) {
            return;
        }
        // Scale any floating-point excess back into the two half-plane bounds.
        const double scale =
            std::min({1.0, w / (a * width + b * height), h / (c * width + d * height)});
        width *= scale;
        height *= scale;
        if (width * height > best.width * best.height) {
            best = {width, height};
        }
    };
    const double determinant = a * d - b * c;
    if (std::abs(determinant) > 1e-12) {
        consider((w * d - b * h) / determinant, (a * h - w * c) / determinant);
    }
    if (a > 0 && b > 0) {
        consider(w / (2 * a), w / (2 * b));
    }
    if (c > 0 && d > 0) {
        consider(h / (2 * c), h / (2 * d));
    }
    return best;
}

/// @brief Fits an explicit rectangle while retaining its centre whenever possible.
void fitExplicit(GeometryPlan& plan) {
    auto centre = plan.toSource({plan.left + plan.width / 2, plan.top + plan.height / 2});
    const auto extent = sourceExtent(plan, {plan.width, plan.height});
    const double roomX = std::min(centre.x, plan.sourceSize.width - centre.x);
    const double roomY = std::min(centre.y, plan.sourceSize.height - centre.y);
    double scale = std::min({1.0, 2 * roomX / extent.width, 2 * roomY / extent.height});
    if (scale == 1.0) {
        return;
    }
    if (scale <= 0) {
        // No positive rectangle fits at this centre. Choose the largest
        // retained size, then the nearest feasible centre. The feasible
        // centres form a rectangle in source axes, so projection is a clamp.
        scale = std::min(
            {1.0, plan.sourceSize.width / extent.width, plan.sourceSize.height / extent.height});
        const double marginX = std::min(plan.sourceSize.width / 2.0, extent.width * scale / 2);
        const double marginY = std::min(plan.sourceSize.height / 2.0, extent.height * scale / 2);
        centre.x = std::clamp(centre.x, marginX, plan.sourceSize.width - marginX);
        centre.y = std::clamp(centre.y, marginY, plan.sourceSize.height - marginY);
    }
    plan.width *= scale;
    plan.height *= scale;
    const auto upright = plan.toUpright(centre);
    plan.left = upright.x - plan.width / 2;
    plan.top = upright.y - plan.height / 2;
}

/// @brief Converts a continuous extent to a non-empty raster dimension.
std::uint32_t rasterExtent(double extent) {
    if (!std::isfinite(extent) || extent <= 0 ||
        extent > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Crop dimensions cannot be represented");
    }
    // Only repair rounding at an integer boundary, not ordinary fractions.
    const double nearest = std::round(extent);
    if (std::abs(nearest - extent) <= 1e-10 * std::max(1.0, extent)) {
        extent = nearest;
    }
    return static_cast<std::uint32_t>(std::max(1.0, std::floor(extent)));
}

/// @brief Interpolates in premultiplied alpha, returning straight RGBA.
std::array<float, 4> sample(const ImageBuffer& source, SourcePoint position) {
    const auto centreCoordinate = [](double edge, std::uint32_t length) {
        double value = std::clamp(edge - 0.5, 0.0, length - 1.0);
        const double nearest = std::round(value);
        if (std::abs(value - nearest) <= 32 * std::numeric_limits<double>::epsilon() * length) {
            value = nearest;
        }
        return value;
    };
    const double x = centreCoordinate(position.x, source.size().width);
    const double y = centreCoordinate(position.y, source.size().height);
    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min(x0 + 1, source.size().width - 1);
    const auto y1 = std::min(y0 + 1, source.size().height - 1);
    const double dx = x - x0;
    const double dy = y - y0;
    const auto pixels = source.samples<float>();
    const auto at = [&](std::uint32_t column, std::uint32_t row) {
        return pixels.subspan((static_cast<std::size_t>(row) * source.size().width + column) * 4,
                              4);
    };
    if (dx == 0 && dy == 0) {
        const auto pixel = at(x0, y0);
        return {pixel[0], pixel[1], pixel[2], pixel[3]};
    }
    const std::array neighbours{at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)};
    const double weights[]{(1 - dx) * (1 - dy), dx * (1 - dy), (1 - dx) * dy, dx * dy};
    std::array<double, 4> sum{};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto pixel = neighbours[index];
        const double weight = weights[index] * pixel[3];
        sum[3] += weight;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            sum[channel] += weight * pixel[channel];
        }
    }
    if (sum[3] <= 0) {
        return {};
    }
    return {static_cast<float>(sum[0] / sum[3]), static_cast<float>(sum[1] / sum[3]),
            static_cast<float>(sum[2] / sum[3]), static_cast<float>(sum[3])};
}

} // namespace

UprightPoint GeometryPlan::toUpright(SourcePoint point) const {
    const double x = point.x - sourceSize.width / 2.0;
    const double y = point.y - sourceSize.height / 2.0;
    return {matrix[0] * x + matrix[1] * y + uprightWidth / 2,
            matrix[2] * x + matrix[3] * y + uprightHeight / 2};
}

SourcePoint GeometryPlan::toSource(UprightPoint point) const {
    const double x = point.x - uprightWidth / 2;
    const double y = point.y - uprightHeight / 2;
    // Orthogonal transforms invert by transposition, including reflections.
    return {matrix[0] * x + matrix[2] * y + sourceSize.width / 2.0,
            matrix[1] * x + matrix[3] * y + sourceSize.height / 2.0};
}

GeometryPlan arraw::geometryPlanFor(ImageSize size, ImageOrientation orientation,
                                    const GeometrySettings& settings) {
    if (size.empty() || !std::isfinite(settings.straighten) ||
        settings.straighten < minimumStraighten || settings.straighten > maximumStraighten) {
        throw std::invalid_argument(
            "Geometry requires non-empty dimensions and straighten from -45 to 45");
    }
    const Matrix oriented =
        compose(rotationMatrix(settings.rotation), orientationMatrix(orientation));
    const double originalWidth =
        std::abs(oriented[0]) * size.width + std::abs(oriented[1]) * size.height;
    const double originalHeight =
        std::abs(oriented[2]) * size.width + std::abs(oriented[3]) * size.height;
    const double radians = settings.straighten * std::numbers::pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    // Quarter-turns commute with straighten; camera reflections do not.
    const Matrix fine{c, -s, s, c};
    const Matrix flip{settings.flipHorizontal ? -1.0 : 1.0, 0, 0,
                      settings.flipVertical ? -1.0 : 1.0};
    GeometryPlan plan;
    plan.sourceSize = size;
    plan.matrix = compose(flip, compose(fine, oriented));
    plan.uprightWidth =
        std::abs(plan.matrix[0]) * size.width + std::abs(plan.matrix[1]) * size.height;
    plan.uprightHeight =
        std::abs(plan.matrix[2]) * size.width + std::abs(plan.matrix[3]) * size.height;
    std::optional<double> aspect;
    if (std::holds_alternative<OriginalCropAspect>(settings.crop.aspect)) {
        aspect = originalWidth / originalHeight;
    } else if (const auto* custom = std::get_if<CropRatio>(&settings.crop.aspect)) {
        if (!std::isfinite(custom->widthOverHeight) || custom->widthOverHeight <= 0) {
            throw std::invalid_argument("Crop aspect must be positive and finite");
        }
        aspect = custom->widthOverHeight;
    }
    if (settings.crop.rectangle) {
        const auto& crop = *settings.crop.rectangle;
        if (!std::isfinite(crop.left) || !std::isfinite(crop.top) || !std::isfinite(crop.right) ||
            !std::isfinite(crop.bottom) || crop.left < 0 || crop.top < 0 || crop.right > 1 ||
            crop.bottom > 1 || crop.left >= crop.right || crop.top >= crop.bottom) {
            throw std::invalid_argument("Crop edges must be finite, ordered and within 0 to 1");
        }
        plan.left = crop.left * plan.uprightWidth;
        plan.top = crop.top * plan.uprightHeight;
        plan.width = (crop.right - crop.left) * plan.uprightWidth;
        plan.height = (crop.bottom - crop.top) * plan.uprightHeight;
        const double ratio = plan.width / plan.height;
        if (aspect && std::abs(ratio / *aspect - 1.0) > 1e-6) {
            throw std::invalid_argument("Explicit crop does not match its aspect constraint");
        }
        fitExplicit(plan);
    } else {
        const auto extent = aspect ? atAspect(plan, *aspect) : largestFree(plan);
        plan.width = extent.width;
        plan.height = extent.height;
        plan.left = (plan.uprightWidth - plan.width) / 2;
        plan.top = (plan.uprightHeight - plan.height) / 2;
    }
    plan.outputSize = {rasterExtent(plan.width), rasterExtent(plan.height)};
    return plan;
}

ImageBuffer arraw::applyGeometry(ImageBuffer source, const GeometryPlan& plan) {
    if (source.size() != plan.sourceSize || source.format() != workingFormat) {
        throw std::invalid_argument("Geometry requires matching developed float pixels");
    }
    if (source.orientation() == ImageOrientation::Normal && plan.matrix == Matrix{1, 0, 0, 1} &&
        plan.left == 0 && plan.top == 0 && plan.width == plan.sourceSize.width &&
        plan.height == plan.sourceSize.height) {
        return source;
    }
    ImageBuffer result(plan.outputSize, workingFormat, source.encoding());
    auto output = result.samples<float>();
    for (std::uint32_t y = 0; y < plan.outputSize.height; ++y) {
        for (std::uint32_t x = 0; x < plan.outputSize.width; ++x) {
            const UprightPoint upright{plan.left + (x + 0.5) * plan.width / plan.outputSize.width,
                                       plan.top + (y + 0.5) * plan.height / plan.outputSize.height};
            const auto pixel = sample(source, plan.toSource(upright));
            const auto index = (static_cast<std::size_t>(y) * plan.outputSize.width + x) * 4;
            std::copy(pixel.begin(), pixel.end(), output.begin() + index);
        }
    }
    return result;
}
