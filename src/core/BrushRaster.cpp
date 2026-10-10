#include "BrushRaster.h"

#include "RowBands.h"
#include "TonePlan.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace arraw {

std::vector<DabCentre> dabCentres(const Stroke& stroke, ImageSize raster, bool endDab) {
    const double width = raster.width;
    const double height = raster.height;
    const double longEdge = std::max(width, height);
    struct Point {
        double x;
        double y;
    };
    const auto at = [&](std::size_t i) {
        const SensorPoint& p = stroke.points[i];
        return Point{static_cast<double>(p.u) * width / longEdge,
                     static_cast<double>(p.v) * height / longEdge};
    };
    std::vector<DabCentre> dabs;
    const auto emit = [&](const Point& p) {
        dabs.push_back({static_cast<float>(p.x * longEdge), static_cast<float>(p.y * longEdge)});
    };
    const double spacing = 0.25 * static_cast<double>(stroke.radius);
    emit(at(0));
    std::uint64_t k = 1;
    double travelled = 0.0;
    for (std::size_t i = 0; i + 1 < stroke.points.size(); ++i) {
        const Point a = at(i);
        const Point b = at(i + 1);
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length == 0.0) {
            continue;
        }
        while (static_cast<double>(k) * spacing <= travelled + length) {
            const double t = (static_cast<double>(k) * spacing - travelled) / length;
            emit({a.x + dx * t, a.y + dy * t});
            ++k;
        }
        travelled += length;
    }
    if (endDab && static_cast<double>(k - 1) * spacing < travelled) {
        emit(at(stroke.points.size() - 1));
    }
    return dabs;
}

PixelSpan dabSpan(float centre, float radius) noexcept {
    const double c = centre;
    const double r = radius;
    return {static_cast<std::int64_t>(std::floor(c - r)) - 1,
            static_cast<std::int64_t>(std::ceil(c + r)) + 2};
}

PlacedStroke placedStroke(const Stroke& stroke, ImageSize raster) {
    const double longEdge = std::max(raster.width, raster.height);
    const double radius = static_cast<double>(stroke.radius) * longEdge;
    // A dab thinner than ::arraw::minimumDrawnRadius is drawn that wide with its flow scaled by
    // the area it gives up, so that a thin stroke neither vanishes between pixel centres nor
    // changes much with its sub-pixel position.
    const double drawn = std::max(radius, minimumDrawnRadius);
    const double areaRatio = (radius / drawn) * (radius / drawn);
    PlacedStroke placed{};
    placed.radius = static_cast<float>(drawn);
    placed.inner =
        static_cast<float>(std::min(static_cast<double>(stroke.hardness) * drawn, drawn - 1.0));
    placed.flow = static_cast<float>(static_cast<double>(stroke.flow) * areaRatio);
    placed.erase = stroke.erase;
    placed.dabs = dabCentres(stroke, raster);
    placed.left = placed.top = INT64_MAX;
    placed.right = placed.bottom = INT64_MIN;
    for (const DabCentre& dab : placed.dabs) {
        const PixelSpan columns = dabSpan(dab.x, placed.radius);
        const PixelSpan rows = dabSpan(dab.y, placed.radius);
        placed.left = std::min(placed.left, columns.first);
        placed.right = std::max(placed.right, columns.last);
        placed.top = std::min(placed.top, rows.first);
        placed.bottom = std::max(placed.bottom, rows.last);
    }
    return placed;
}

std::vector<PlacedStroke> placedStrokes(const StrokeList& strokes, ImageSize raster,
                                        std::size_t first) {
    std::vector<PlacedStroke> placed;
    for (std::size_t i = first; i < strokes.size(); ++i) {
        placed.push_back(placedStroke(strokes[i], raster));
    }
    return placed;
}

void paintDab(const PlacedStroke& stroke, const DabCentre& dab, PixelRect region,
              std::span<float> values) {
    const auto regionLeft = static_cast<std::int64_t>(region.x);
    const auto regionTop = static_cast<std::int64_t>(region.y);
    const auto regionRight = regionLeft + region.width;
    const auto regionBottom = regionTop + region.height;
    const PixelSpan columns = dabSpan(dab.x, stroke.radius);
    const PixelSpan rows = dabSpan(dab.y, stroke.radius);
    const std::int64_t x0 = std::max(columns.first, regionLeft);
    const std::int64_t x1 = std::min(columns.last, regionRight);
    const std::int64_t y0 = std::max(rows.first, regionTop);
    const std::int64_t y1 = std::min(rows.last, regionBottom);
    for (std::int64_t y = y0; y < y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5F) - dab.y;
        float* row = values.data() + static_cast<std::size_t>(y - regionTop) * region.width;
        for (std::int64_t x = x0; x < x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5F) - dab.x;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r >= stroke.radius) {
                continue;
            }
            const float d = 1.0F - smoothstep(stroke.inner, stroke.radius, r);
            float& m = row[x - regionLeft];
            m = stroke.erase ? m * (1.0F - stroke.flow * d) : m + (stroke.flow * d) * (1.0F - m);
        }
    }
}

void paintRegion(std::span<const PlacedStroke> strokes, PixelRect region, std::span<float> values) {
    assert(values.size() == static_cast<std::size_t>(region.width) * region.height);
    const auto regionLeft = static_cast<std::int64_t>(region.x);
    const auto regionTop = static_cast<std::int64_t>(region.y);
    const auto regionRight = regionLeft + region.width;
    const auto regionBottom = regionTop + region.height;
    for (const PlacedStroke& stroke : strokes) {
        if (stroke.right <= regionLeft || stroke.left >= regionRight ||
            stroke.bottom <= regionTop || stroke.top >= regionBottom) {
            continue;
        }
        for (const DabCentre& dab : stroke.dabs) {
            paintDab(stroke, dab, region, values);
        }
    }
}

DabBuckets::DabBuckets(std::span<const PlacedStroke> placed, std::uint32_t rows,
                       std::uint32_t bucketRows)
    : bucketRows_(bucketRows) {
    if (rows == 0 || bucketRows == 0) {
        throw std::invalid_argument("a dab index needs rows and a bucket height");
    }
    count_ = static_cast<std::uint32_t>((static_cast<std::uint64_t>(rows) + bucketRows - 1) /
                                        bucketRows);
    // The buckets a dab's row span meets, clipped to the raster; empty when it meets none.
    const auto bucketsOf = [&](const PlacedStroke& stroke, const DabCentre& dab) {
        const PixelSpan span = dabSpan(dab.y, stroke.radius);
        const std::int64_t first = std::max<std::int64_t>(span.first, 0);
        const std::int64_t last = std::min<std::int64_t>(span.last, rows);
        struct Range {
            std::int64_t first;
            std::int64_t last; ///< Inclusive; below first when empty.
        };
        if (first >= last) {
            return Range{0, -1};
        }
        return Range{first / bucketRows, (last - 1) / bucketRows};
    };
    start_.assign(static_cast<std::size_t>(count_) + 1, 0);
    for (const PlacedStroke& stroke : placed) {
        for (const DabCentre& dab : stroke.dabs) {
            const auto range = bucketsOf(stroke, dab);
            for (std::int64_t b = range.first; b <= range.last; ++b) {
                ++start_[static_cast<std::size_t>(b) + 1];
            }
        }
    }
    for (std::size_t b = 0; b < count_; ++b) {
        start_[b + 1] += start_[b];
    }
    refs_.resize(start_.back());
    std::vector<std::size_t> next(start_.begin(), start_.end() - 1);
    for (std::size_t s = 0; s < placed.size(); ++s) {
        const PlacedStroke& stroke = placed[s];
        for (std::size_t d = 0; d < stroke.dabs.size(); ++d) {
            const auto range = bucketsOf(stroke, stroke.dabs[d]);
            for (std::int64_t b = range.first; b <= range.last; ++b) {
                refs_[next[static_cast<std::size_t>(b)]++] = {static_cast<std::uint32_t>(s),
                                                              static_cast<std::uint32_t>(d)};
            }
        }
    }
}

void paintBucket(std::span<const PlacedStroke> placed, const DabBuckets& buckets,
                 std::uint32_t bucket, PixelRect region, std::span<float> values) {
    assert(values.size() == static_cast<std::size_t>(region.width) * region.height);
    for (const DabBuckets::Ref& ref : buckets.bucket(bucket)) {
        const PlacedStroke& stroke = placed[ref.stroke];
        paintDab(stroke, stroke.dabs[ref.dab], region, values);
    }
}

CoveragePlane rasteriseBrush(const StrokeList& strokes, ImageSize raster) {
    if (raster.empty()) {
        throw std::invalid_argument("a brush raster needs a size");
    }
    if (strokes.rasteriser() != brushRasteriserVersion) {
        throw std::invalid_argument("unknown brush rasteriser version");
    }
    CoveragePlane plane{raster,
                        std::vector<float>(static_cast<std::size_t>(raster.width) * raster.height)};
    const std::vector<PlacedStroke> placed = placedStrokes(strokes, raster);
    detail::forEachRowBand(
        raster.height, raster.width, [&](std::uint32_t first, std::uint32_t last) {
            paintRegion(placed, {0, first, raster.width, last - first},
                        std::span<float>(plane.values.data() +
                                             static_cast<std::size_t>(first) * raster.width,
                                         static_cast<std::size_t>(last - first) * raster.width));
        });
    return plane;
}

} // namespace arraw
