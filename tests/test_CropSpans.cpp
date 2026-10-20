#include "CropSpans.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The rows of a crop quad, which the histogram counts while a crop is dragged (ADR 055, section
/// 3).

namespace {

/// The even-odd crossing test with the half-open rule, written the plain way.
bool insideQuad(const std::array<QPointF, 4>& quad, double px, double py) {
    bool inside = false;
    for (std::size_t i = 0; i < quad.size(); ++i) {
        const QPointF a = quad[i];
        const QPointF b = quad[(i + 1) % quad.size()];
        if ((a.y() > py) != (b.y() > py)) {
            const double x = a.x() + (py - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
            if (px < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

/// A rectangle turned by an angle about its centre.
std::array<QPointF, 4> turned(QPointF centre, double halfWidth, double halfHeight, double degrees) {
    const double angle = degrees * std::numbers::pi / 180.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const std::array<QPointF, 4> corners{
        QPointF{-halfWidth, -halfHeight}, QPointF{halfWidth, -halfHeight},
        QPointF{halfWidth, halfHeight}, QPointF{-halfWidth, halfHeight}};
    std::array<QPointF, 4> quad;
    for (std::size_t i = 0; i < 4; ++i) {
        quad[i] = {centre.x() + corners[i].x() * c - corners[i].y() * s,
                   centre.y() + corners[i].x() * s + corners[i].y() * c};
    }
    return quad;
}

/// Compares the spans with the point test on every pixel; returns the first mismatch count.
std::size_t mismatches(QSize image, const std::array<QPointF, 4>& quad) {
    const std::vector<RowSpan> spans = cropSpans(image, quad);
    REQUIRE(spans.size() == static_cast<std::size_t>(image.height()));
    std::size_t wrong = 0;
    for (int y = 0; y < image.height(); ++y) {
        const RowSpan span = spans[static_cast<std::size_t>(y)];
        REQUIRE(span.begin <= span.end);
        REQUIRE(span.end <= static_cast<std::uint32_t>(image.width()));
        for (int x = 0; x < image.width(); ++x) {
            const bool in = static_cast<std::uint32_t>(x) >= span.begin &&
                            static_cast<std::uint32_t>(x) < span.end;
            if (in != insideQuad(quad, x + 0.5, y + 0.5)) {
                ++wrong;
            }
        }
    }
    return wrong;
}

} // namespace

TEST_CASE("spans equal a brute-force point-in-quad test", "[app][cropspans]") {
    std::mt19937 random(7);
    std::uniform_real_distribution<double> position(-10.0, 90.0);
    std::uniform_real_distribution<double> half(1.0, 45.0);
    std::uniform_real_distribution<double> degrees(-90.0, 90.0);
    const QSize image(80, 60);
    for (int round = 0; round < 200; ++round) {
        const auto quad = turned({position(random), position(random)}, half(random), half(random),
                                 degrees(random));
        CHECK(mismatches(image, quad) == 0);
    }
    for (const double angle : {-45.0, 45.0, 0.0, 90.0, 135.0}) {
        CHECK(mismatches(image, turned({40.0, 30.0}, 30.0, 17.0, angle)) == 0);
    }
    // Either winding gives the same spans.
    auto quad = turned({40.5, 30.25}, 22.0, 15.0, 30.0);
    const std::vector<RowSpan> forward = cropSpans(image, quad);
    std::swap(quad[1], quad[3]);
    CHECK(cropSpans(image, quad) == forward);
}

TEST_CASE("an axis-aligned quad gives whole rows", "[app][cropspans]") {
    const QSize image(10, 8);
    // Whole pixels: rows 2 to 5, columns 3 to 7.
    const std::array<QPointF, 4> quad{QPointF{3, 2}, QPointF{8, 2}, QPointF{8, 6}, QPointF{3, 6}};
    const std::vector<RowSpan> spans = cropSpans(image, quad);
    REQUIRE(spans.size() == 8);
    for (std::size_t y = 0; y < 8; ++y) {
        if (y >= 2 && y < 6) {
            CHECK(spans[y] == RowSpan{3, 8});
        } else {
            CHECK(spans[y] == RowSpan{});
        }
    }

    // A quad over the whole image is whole rows.
    const std::array<QPointF, 4> all{QPointF{0, 0}, QPointF{10, 0}, QPointF{10, 8}, QPointF{0, 8}};
    for (const RowSpan span : cropSpans(image, all)) {
        CHECK(span == RowSpan{0, 10});
    }
}

TEST_CASE("a quad outside the image gives no spans", "[app][cropspans]") {
    const QSize image(10, 8);
    const std::array<QPointF, 4> beside{QPointF{20, 1}, QPointF{30, 1}, QPointF{30, 5},
                                        QPointF{20, 5}};
    const std::array<QPointF, 4> above{QPointF{1, -9}, QPointF{9, -9}, QPointF{9, -2},
                                       QPointF{1, -2}};
    for (const auto& quad : {beside, above}) {
        const std::vector<RowSpan> spans = cropSpans(image, quad);
        REQUIRE(spans.size() == 8);
        for (const RowSpan span : spans) {
            CHECK(span == RowSpan{});
        }
    }
    CHECK(cropSpans(QSize(0, 0), beside).empty());
}

TEST_CASE("a centre exactly on an edge is inside on the left and top, outside on the right and "
          "bottom",
          "[app][cropspans]") {
    // Centres at 0.5, 1.5, ...: the edges pass through the centres of column 2 and row 1.
    const QSize image(6, 6);
    const std::array<QPointF, 4> quad{QPointF{2.5, 1.5}, QPointF{4.5, 1.5}, QPointF{4.5, 4.5},
                                      QPointF{2.5, 4.5}};
    const std::vector<RowSpan> spans = cropSpans(image, quad);
    CHECK(spans[0] == RowSpan{});
    CHECK(spans[1] == RowSpan{2, 4});
    CHECK(spans[3] == RowSpan{2, 4});
    CHECK(spans[4] == RowSpan{});
    CHECK(mismatches(image, quad) == 0);
}
