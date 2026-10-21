#include "CropSpans.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace arraw::app {

std::vector<RowSpan> cropSpans(QSize image, const std::array<QPointF, 4>& quad) {
    const int width = std::max(image.width(), 0);
    const int height = std::max(image.height(), 0);
    std::vector<RowSpan> spans(static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        const double y = row + 0.5;
        double left = std::numeric_limits<double>::infinity();
        double right = -std::numeric_limits<double>::infinity();
        for (std::size_t edge = 0; edge < quad.size(); ++edge) {
            const QPointF a = quad[edge];
            const QPointF b = quad[(edge + 1) % quad.size()];
            // Half-open in y, so a vertex on the row's centre is counted by one edge only.
            if ((a.y() > y) == (b.y() > y)) {
                continue;
            }
            const double x = a.x() + (y - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
            left = std::min(left, x);
            right = std::max(right, x);
        }
        if (!(left < right)) {
            continue;
        }
        // Centres with left <= x + 0.5 < right.
        const double first = std::clamp(std::ceil(left - 0.5), 0.0, static_cast<double>(width));
        const double end = std::clamp(std::ceil(right - 0.5), 0.0, static_cast<double>(width));
        if (first < end) {
            spans[static_cast<std::size_t>(row)] = {static_cast<std::uint32_t>(first),
                                                    static_cast<std::uint32_t>(end)};
        }
    }
    return spans;
}

} // namespace arraw::app
