#include "GeometryPlan.h"
#include "LocalPlan.h"

#include <CropGeometry.h>
#include <Develop.h>
#include <DevelopState.h>
#include <DevelopedFrame.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace arraw;
using Catch::Approx;

/// The placement of the corrected frame in the developed one, and the coverage a mask has there
/// (ADR 044, section 4).

namespace {

constexpr std::array<ImageOrientation, 8> orientations{
    ImageOrientation::Normal,     ImageOrientation::MirrorHorizontal,
    ImageOrientation::Rotate180,  ImageOrientation::MirrorVertical,
    ImageOrientation::Transpose,  ImageOrientation::Rotate90,
    ImageOrientation::Transverse, ImageOrientation::Rotate270};

constexpr std::array<QuarterTurn, 4> turns{QuarterTurn::None, QuarterTurn::Clockwise90,
                                           QuarterTurn::Clockwise180, QuarterTurn::Clockwise270};

constexpr std::array<double, 3> straightens{0.0, 7.5, -30.0};

/// @brief Calls a function with every geometry of the sweep, automatic and explicit crops.
template <class Visit> void forEachGeometry(Visit&& visit) {
    for (const ImageOrientation orientation : orientations) {
        for (const QuarterTurn turn : turns) {
            for (const bool horizontal : {false, true}) {
                for (const bool vertical : {false, true}) {
                    for (const double straighten : straightens) {
                        for (const bool explicitCrop : {false, true}) {
                            GeometrySettings geometry;
                            geometry.rotation = turn;
                            geometry.flipHorizontal = horizontal;
                            geometry.flipVertical = vertical;
                            geometry.straighten = straighten;
                            if (explicitCrop) {
                                geometry.crop.rectangle = UprightCropRect{0.4, 0.45, 0.6, 0.55};
                            }
                            visit(SourceShape{{300, 200}, orientation}, geometry);
                        }
                    }
                }
            }
        }
    }
}

constexpr std::array<CorrectedPosition, 6> samples{
    {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.3, 0.8}, {-0.5, 1.7}, {0.5, 0.5}}};

} // namespace

TEST_CASE("The map round-trips both ways in every geometry", "[developed-frame]") {
    std::size_t count = 0;
    forEachGeometry([&count](SourceShape source, const GeometrySettings& geometry) {
        const DevelopedFrameMap map(source, geometry);
        for (const CorrectedPosition at : samples) {
            const CorrectedPosition back = map.correctedFrom(map.developedFrom(at));
            REQUIRE(back.u == Approx(at.u).margin(1e-9));
            REQUIRE(back.v == Approx(at.v).margin(1e-9));
            const DevelopedPoint developed = map.developedFrom(at);
            const DevelopedPoint again = map.developedFrom(map.correctedFrom(developed));
            REQUIRE(again.x == Approx(developed.x).margin(1e-9));
            REQUIRE(again.y == Approx(developed.y).margin(1e-9));
            const CorrectedPosition viaLongEdge = map.correctedFromLongEdge(map.longEdgeFrom(at));
            REQUIRE(viaLongEdge.u == Approx(at.u).margin(1e-12));
            REQUIRE(viaLongEdge.v == Approx(at.v).margin(1e-12));
        }
        ++count;
    });
    REQUIRE(count == 8 * 4 * 4 * 3 * 2);
}

TEST_CASE("The map puts known points where the picture shows them", "[developed-frame]") {
    const SourceShape landscape{{300, 200}, ImageOrientation::Normal};

    SECTION("identity") {
        const DevelopedFrameMap map(landscape, {});
        const DevelopedPoint p = map.developedFrom({0.25, 0.75});
        REQUIRE(p.x == Approx(0.25).margin(1e-12));
        REQUIRE(p.y == Approx(0.75).margin(1e-12));
        REQUIRE(map.width() == Approx(300.0));
        REQUIRE(map.height() == Approx(200.0));
        REQUIRE(map.longEdge() == Approx(300.0));
        REQUIRE(map.source() == landscape);
    }

    SECTION("a camera orientation of 90 degrees puts the top-left corner top-right") {
        const DevelopedFrameMap map({{300, 200}, ImageOrientation::Rotate90}, {});
        const DevelopedPoint topLeft = map.developedFrom({0.0, 0.0});
        REQUIRE(topLeft.x == Approx(1.0).margin(1e-12));
        REQUIRE(topLeft.y == Approx(0.0).margin(1e-12));
        const DevelopedPoint topRight = map.developedFrom({1.0, 0.0});
        REQUIRE(topRight.x == Approx(1.0).margin(1e-12));
        REQUIRE(topRight.y == Approx(1.0).margin(1e-12));
        REQUIRE(map.width() == Approx(200.0));
        REQUIRE(map.height() == Approx(300.0));
    }

    SECTION("a horizontal flip mirrors across") {
        GeometrySettings geometry;
        geometry.flipHorizontal = true;
        const DevelopedFrameMap map(landscape, geometry);
        const DevelopedPoint p = map.developedFrom({0.2, 0.6});
        REQUIRE(p.x == Approx(0.8).margin(1e-12));
        REQUIRE(p.y == Approx(0.6).margin(1e-12));
    }

    SECTION("a crop corner is the origin") {
        GeometrySettings geometry;
        geometry.crop.rectangle = UprightCropRect{0.25, 0.25, 0.75, 0.75};
        const DevelopedFrameMap map(landscape, geometry);
        const DevelopedPoint corner = map.developedFrom({0.25, 0.25});
        REQUIRE(corner.x == Approx(0.0).margin(1e-12));
        REQUIRE(corner.y == Approx(0.0).margin(1e-12));
        const DevelopedPoint far = map.developedFrom({0.75, 0.75});
        REQUIRE(far.x == Approx(1.0).margin(1e-12));
        REQUIRE(far.y == Approx(1.0).margin(1e-12));
        REQUIRE(map.width() == Approx(150.0));
    }

    SECTION("an invalid geometry is refused as the crop rules refuse it") {
        GeometrySettings geometry;
        geometry.straighten = 60.0;
        REQUIRE_THROWS_AS(DevelopedFrameMap(landscape, geometry), std::invalid_argument);
        REQUIRE_THROWS_AS(DevelopedFrameMap({{0, 0}, ImageOrientation::Normal}, {}),
                          std::invalid_argument);
    }
}

TEST_CASE("The map agrees with the geometry plan the render and the picker use",
          "[developed-frame]") {
    forEachGeometry([](SourceShape source, const GeometrySettings& geometry) {
        const DevelopedFrameMap map(source, geometry);
        const GeometryPlan plan = geometryPlanFor(source.size, source.orientation, geometry);
        REQUIRE(map.width() == Approx(plan.width));
        REQUIRE(map.height() == Approx(plan.height));
        for (const DevelopedPoint at : {DevelopedPoint{0.0, 0.0}, {1.0, 1.0}, {0.3, 0.7}}) {
            // The picker's chain (WhiteBalance.cpp): developed -> upright -> source edge units.
            const SourcePoint expected =
                plan.toSource({plan.left + at.x * plan.width, plan.top + at.y * plan.height});
            const CorrectedPosition got = map.correctedFrom(at);
            REQUIRE(got.u * source.size.width == Approx(expected.x).margin(1e-9));
            REQUIRE(got.v * source.size.height == Approx(expected.y).margin(1e-9));
        }
    });
}

TEST_CASE("The long-edge metric is preserved up to one scale", "[developed-frame]") {
    forEachGeometry([](SourceShape source, const GeometrySettings& geometry) {
        const DevelopedFrameMap map(source, geometry);
        const std::array<CorrectedPosition, 3> points{{{0.1, 0.2}, {0.7, 0.35}, {0.4, 0.9}}};
        const auto inPixels = [&map](CorrectedPosition at) {
            const DevelopedPoint d = map.developedFrom(at);
            return std::array<double, 2>{d.x * map.width(), d.y * map.height()};
        };
        const auto inLongEdge = [&map](CorrectedPosition at) {
            const LongEdgePoint p = map.longEdgeFrom(at);
            return std::array<double, 2>{p.x, p.y};
        };
        const auto difference = [](std::array<double, 2> a, std::array<double, 2> b) {
            return std::array<double, 2>{b[0] - a[0], b[1] - a[1]};
        };
        const auto length = [](std::array<double, 2> v) { return std::hypot(v[0], v[1]); };
        const auto dot = [](std::array<double, 2> a, std::array<double, 2> b) {
            return a[0] * b[0] + a[1] * b[1];
        };
        // One scale: the source's long edge, since the long-edge metric has it as its unit.
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t j = (i + 1) % 3;
            REQUIRE(length(difference(inPixels(points[i]), inPixels(points[j]))) ==
                    Approx(map.longEdge() *
                           length(difference(inLongEdge(points[i]), inLongEdge(points[j]))))
                        .epsilon(1e-9));
        }
        // Angles, up to a reflection: the cosine at the first point.
        const auto cosine = [&](const auto& at) {
            const auto a = difference(at(points[0]), at(points[1]));
            const auto b = difference(at(points[0]), at(points[2]));
            return dot(a, b) / (length(a) * length(b));
        };
        REQUIRE(cosine(inPixels) == Approx(cosine(inLongEdge)).margin(1e-9));
    });
}

namespace {

/// @brief A state with one adjustment of a shape and exposure, in a geometry.
DevelopState stateWithMask(Mask shape, bool invert = false, const GeometrySettings& geometry = {},
                           float exposure = 1.0F) {
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0.0F;
    settings.geometry = geometry;
    LocalAdjustment adjustment;
    adjustment.shape = std::move(shape);
    adjustment.invert = invert;
    adjustment.deltas.exposure = exposure;
    return withLocalAdjustmentAdded(DevelopState{settings}, std::move(adjustment));
}

/// @brief The coverage of a state's only adjustment over the whole developed frame.
MaskCoverage wholeCoverage(const DevelopState& state, SourceShape source, ImageSize size) {
    const DevelopedFrameMap map(source, state.settings.geometry);
    return maskCoverage(state.localAdjustments.front(), map, {}, size);
}

constexpr RadialMask smallRadial{
    .centre = {0.4F, 0.6F}, .radiusX = 0.3F, .radiusY = 0.15F, .angle = 30.0F, .feather = 0.4F};

} // namespace

TEST_CASE("Coverage is the engine's weight, rounded to eight bits, on an identity geometry",
          "[developed-frame][coverage]") {
    constexpr SourceShape source{{37, 23}, ImageOrientation::Normal};
    const std::array<Mask, 2> shapes{LinearMask{{0.3F, 0.2F}, {0.7F, 0.9F}}, smallRadial};
    for (const bool invert : {false, true}) {
        for (const Mask& shape : shapes) {
            const DevelopState state = stateWithMask(shape, invert);
            const MaskCoverage coverage = wholeCoverage(state, source, source.size);
            REQUIRE(coverage.size == source.size);
            REQUIRE(coverage.weights.size() == 37U * 23U);
            const LocalPlan plan = localPlanFor(state, source.size);
            REQUIRE(plan.masks.size() == 1);
            for (std::uint32_t y = 0; y < source.size.height; ++y) {
                for (std::uint32_t x = 0; x < source.size.width; ++x) {
                    const float weight = maskWeight(plan.masks[0], static_cast<float>(x) + 0.5F,
                                                    static_cast<float>(y) + 0.5F, PixelCoverage{});
                    REQUIRE(coverage.weights[y * source.size.width + x] ==
                            static_cast<std::uint8_t>(std::lround(255.0F * weight)));
                }
            }
        }
    }
}

TEST_CASE("Invert turns the coverage inside out", "[developed-frame][coverage]") {
    constexpr SourceShape source{{40, 30}, ImageOrientation::Normal};
    const MaskCoverage plain = wholeCoverage(stateWithMask(smallRadial), source, source.size);
    const MaskCoverage inverted =
        wholeCoverage(stateWithMask(smallRadial, true), source, source.size);
    bool partial = false;
    for (std::size_t index = 0; index < plain.weights.size(); ++index) {
        const int sum = plain.weights[index] + inverted.weights[index];
        REQUIRE(std::abs(sum - 255) <= 1);
        partial = partial || (plain.weights[index] > 0 && plain.weights[index] < 255);
    }
    REQUIRE(partial);
}

TEST_CASE("A mask that does nothing is still covered", "[developed-frame][coverage]") {
    constexpr SourceShape source{{40, 30}, ImageOrientation::Normal};
    const DevelopedFrameMap map(source, {});
    LocalAdjustment adjustment;
    adjustment.shape = LinearMask{{0.5F, 0.2F}, {0.5F, 0.8F}};
    adjustment.enabled = false;
    adjustment.opacity = 0.0F;
    const MaskCoverage coverage = maskCoverage(adjustment, map, {}, {20, 20});
    REQUIRE(coverage.weights.front() == 255);
    REQUIRE(coverage.weights.back() == 0);
    // Enabled, opacity and deltas play no part.
    LocalAdjustment other = adjustment;
    other.enabled = true;
    other.opacity = 1.0F;
    other.deltas.exposure = 2.0F;
    REQUIRE(maskCoverage(other, map, {}, {20, 20}).weights == coverage.weights);
}

TEST_CASE("Coverage reaches past the frame", "[developed-frame][coverage]") {
    constexpr SourceShape source{{40, 30}, ImageOrientation::Normal};
    const DevelopedFrameMap map(source, {});
    LocalAdjustment adjustment;
    adjustment.shape = RadialMask{
        .centre = {2.5F, 2.5F}, .radiusX = 0.2F, .radiusY = 0.2F, .angle = 0.0F, .feather = 0.0F};

    SECTION("a mask outside the frame covers none of it") {
        const MaskCoverage coverage = maskCoverage(adjustment, map, {}, {16, 12});
        REQUIRE(std::ranges::all_of(coverage.weights, [](std::uint8_t w) { return w == 0; }));
    }
    SECTION("a region around it sees it") {
        const MaskCoverage coverage = maskCoverage(adjustment, map, {1.5, 1.5, 2.0, 2.0}, {32, 32});
        REQUIRE(coverage.weights[16 * 32 + 16] == 255);
        REQUIRE(coverage.weights[0] == 0);
    }
    SECTION("a region wider than the frame is evaluated off the picture too") {
        adjustment.shape = LinearMask{{0.5F, 0.5F}, {0.5F, 1.5F}};
        const MaskCoverage coverage = maskCoverage(adjustment, map, {-1.0, -1.0, 3.0, 3.0}, {8, 8});
        REQUIRE(coverage.weights[0] == 255);
        REQUIRE(coverage.weights[7 * 8] == 0);
    }
}

TEST_CASE("Coverage refuses what it cannot evaluate", "[developed-frame][coverage]") {
    constexpr SourceShape source{{40, 30}, ImageOrientation::Normal};
    const DevelopedFrameMap map(source, {});
    LocalAdjustment adjustment;
    adjustment.shape = LinearMask{};
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {}, {0, 4}), std::invalid_argument);
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {}, {4, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {}, {maximumCoverageSide + 1, 4}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {}, {4, maximumCoverageSide + 1}),
                      std::invalid_argument);
    REQUIRE_NOTHROW(maskCoverage(adjustment, map, {}, {maximumCoverageSide, 1}));
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {0.0, 0.0, 0.0, 1.0}, {4, 4}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {std::nan(""), 0.0, 1.0, 1.0}, {4, 4}),
                      std::invalid_argument);
    adjustment.shape = LinearMask{{0.5F, 0.5F}, {0.5F, 0.5F}};
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {}, {4, 4}), std::invalid_argument);
}

TEST_CASE("The coverage follows the render under turns, flips, straighten and crop",
          "[developed-frame][coverage]") {
    struct Case {
        ImageOrientation orientation;
        QuarterTurn turn;
        bool flip;
        double straighten;
        bool crop;
    };
    const std::array<Case, 5> cases{
        {{ImageOrientation::Normal, QuarterTurn::None, false, 0.0, false},
         {ImageOrientation::Normal, QuarterTurn::Clockwise90, true, 0.0, false},
         {ImageOrientation::Rotate90, QuarterTurn::None, false, 7.5, false},
         {ImageOrientation::Normal, QuarterTurn::Clockwise270, true, -12.0, true},
         {ImageOrientation::Transpose, QuarterTurn::Clockwise180, false, 5.0, true}}};
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.orientation), static_cast<int>(c.turn), c.flip, c.straighten,
                c.crop);
        GeometrySettings geometry;
        geometry.rotation = c.turn;
        geometry.flipHorizontal = c.flip;
        geometry.straighten = c.straighten;
        if (c.crop) {
            geometry.crop.rectangle = UprightCropRect{0.15, 0.2, 0.85, 0.8};
        }
        const ImageSize size{240, 180};
        ImageBuffer source(size, workingFormat, workingEncoding, c.orientation);
        std::ranges::fill(source.samples<float>(), 0.2F);
        // Alpha is one.
        for (std::size_t i = 3; i < source.samples<float>().size(); i += 4) {
            source.samples<float>()[i] = 1.0F;
        }

        // A mask whose ramp is short on the source (about 60 pixels), so that its tail is brief.
        const DevelopState masked =
            stateWithMask(LinearMask{{0.3F, 0.5F}, {0.55F, 0.5F}}, false, geometry, 2.0F);
        DevelopState plain = masked;
        plain.localAdjustments.clear();

        const ImageBuffer with = develop(source, masked);
        const ImageBuffer without = develop(source, plain);
        REQUIRE(with.size() == without.size());
        const ImageSize out = with.size();
        const DevelopedFrameMap map(shapeOf(source), geometry);
        const MaskCoverage coverage = maskCoverage(masked.localAdjustments.front(), map, {}, out);
        REQUIRE(coverage.size == out);

        const auto a = with.samples<float>();
        const auto b = without.samples<float>();
        const auto covered = [&](std::int64_t x, std::int64_t y) {
            return coverage
                .weights[static_cast<std::size_t>(y) * out.width + static_cast<std::size_t>(x)];
        };
        std::size_t bright = 0;
        std::size_t untouched = 0;
        constexpr std::int64_t margin = 4;
        for (std::int64_t y = margin; y < static_cast<std::int64_t>(out.height) - margin; ++y) {
            for (std::int64_t x = margin; x < static_cast<std::int64_t>(out.width) - margin; ++x) {
                const std::size_t index = (static_cast<std::size_t>(y) * out.width + x) * 4;
                if (covered(x, y) >= 230) {
                    REQUIRE(a[index] > b[index]);
                    ++bright;
                }
                // Coverage of zero over the pixel and the cells that a bilinear tap reaches.
                bool quiet = true;
                for (std::int64_t dy = -margin; dy <= margin && quiet; ++dy) {
                    for (std::int64_t dx = -margin; dx <= margin && quiet; ++dx) {
                        quiet = covered(x + dx, y + dy) == 0;
                    }
                }
                if (quiet) {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        REQUIRE(a[index + channel] == b[index + channel]);
                    }
                    ++untouched;
                }
            }
        }
        REQUIRE(bright > 100);
        REQUIRE(untouched > 100);
    }
}

TEST_CASE("Timing of a 1024 by 1024 coverage", "[.timing]") {
    constexpr SourceShape source{{6000, 4000}, ImageOrientation::Rotate90};
    GeometrySettings geometry;
    geometry.straighten = 3.0;
    const DevelopedFrameMap map(source, geometry);
    const std::array<Mask, 2> shapes{LinearMask{{0.3F, 0.2F}, {0.6F, 0.8F}}, smallRadial};
    for (const Mask& shape : shapes) {
        LocalAdjustment adjustment;
        adjustment.shape = shape;
        double best = 1e9;
        for (int run = 0; run < 5; ++run) {
            const auto start = std::chrono::steady_clock::now();
            const MaskCoverage coverage = maskCoverage(adjustment, map, {}, {1024, 1024});
            const std::chrono::duration<double, std::milli> took =
                std::chrono::steady_clock::now() - start;
            REQUIRE(coverage.weights.size() == 1024U * 1024U);
            best = std::min(best, took.count());
        }
        std::cout << (std::holds_alternative<LinearMask>(shape) ? "linear" : "radial")
                  << " maskCoverage 1024x1024, best of five: " << best << " ms\n";
    }
}

TEST_CASE("Coverage of a brush mask is refused until the tint is drawn",
          "[developed-frame][coverage]") {
    constexpr SourceShape source{{40, 30}, ImageOrientation::Normal};
    const DevelopedFrameMap map(source, {});
    LocalAdjustment adjustment;
    adjustment.shape = BrushMask{};
    REQUIRE_THROWS_AS(maskCoverage(adjustment, map, {}, {4, 4}), std::invalid_argument);
}
