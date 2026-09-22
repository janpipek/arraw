#include "Develop.h"
#include "GeometryPlan.h"
#include "ImageImport.h"
#include "ProcessingPlan.h"

#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <QImage>
#include <QImageIOHandler>
#include <QImageWriter>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace arraw;

namespace {

/// @brief Builds distinguishable pixels, including a coloured transparent pixel.
ImageBuffer labelled(ImageSize size, ImageOrientation orientation = ImageOrientation::Normal) {
    ImageBuffer image(size, workingFormat, workingEncoding, orientation);
    auto samples = image.samples<float>();
    for (std::size_t index = 0; index < size.pixelCount(); ++index) {
        samples[index * 4] = static_cast<float>(index + 1) / 64.0F;
        samples[index * 4 + 1] = 0.25F;
        samples[index * 4 + 2] = 0.5F;
        samples[index * 4 + 3] = index == 0 ? 0.0F : 1.0F;
    }
    return image;
}

/// @brief Checks that every crop corner lies inside the decoded frame.
void requireValidCrop(const GeometryPlan& plan) {
    for (const double x : {plan.left, plan.left + plan.width}) {
        for (const double y : {plan.top, plan.top + plan.height}) {
            const auto source = plan.toSource({x, y});
            REQUIRE(source.x >= -1e-8);
            REQUIRE(source.x <= plan.sourceSize.width + 1e-8);
            REQUIRE(source.y >= -1e-8);
            REQUIRE(source.y <= plan.sourceSize.height + 1e-8);
        }
    }
    REQUIRE(plan.width > 0);
    REQUIRE(plan.height > 0);
}

} // namespace

TEST_CASE("All eight camera orientations rearrange exact samples", "[geometry]") {
    const std::array<std::array<int, 6>, 8> expected{{
        {0, 1, 2, 3, 4, 5}, {2, 1, 0, 5, 4, 3}, {5, 4, 3, 2, 1, 0}, {3, 4, 5, 0, 1, 2},
        {0, 3, 1, 4, 2, 5}, {3, 0, 4, 1, 5, 2}, {5, 2, 4, 1, 3, 0}, {2, 5, 1, 4, 0, 3}}};
    for (int tag = 1; tag <= 8; ++tag) {
        CAPTURE(tag);
        const auto source = labelled({3, 2}, static_cast<ImageOrientation>(tag));
        REQUIRE(source.clone().orientation() == source.orientation());
        const auto output = develop(source, {.tone = {.filmicHighlights = 0}});
        REQUIRE(output.orientation() == ImageOrientation::Normal);
        REQUIRE(output.size() == (tag < 5 ? ImageSize{3, 2} : ImageSize{2, 3}));
        for (std::size_t pixel = 0; pixel < 6; ++pixel) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                REQUIRE(output.samples<float>()[pixel * 4 + channel] ==
                        source.samples<float>()[expected[tag - 1][pixel] * 4 + channel]);
            }
        }
    }
}

TEST_CASE("User rotation and flips follow camera orientation", "[geometry]") {
    const auto source = labelled({3, 2}, ImageOrientation::Rotate90);
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0;
    settings.geometry.rotation = QuarterTurn::Clockwise270;
    auto cancelled = develop(source, settings);
    REQUIRE(cancelled.size() == source.size());
    REQUIRE(cancelled.orientation() == ImageOrientation::Normal);
    REQUIRE(std::equal(cancelled.samples<float>().begin(), cancelled.samples<float>().end(),
                       source.samples<float>().begin()));

    settings.geometry.rotation = QuarterTurn::None;
    settings.geometry.flipHorizontal = true;
    const auto flipped = develop(source, settings);
    const int expected[]{0, 3, 1, 4, 2, 5};
    for (std::size_t index = 0; index < 6; ++index) {
        REQUIRE(flipped.samples<float>()[index * 4] == source.samples<float>()[expected[index] * 4]);
    }

    settings.geometry.flipVertical = true;
    const auto both = develop(source, settings);
    const int bothExpected[]{2, 5, 1, 4, 0, 3};
    for (std::size_t index = 0; index < 6; ++index) {
        REQUIRE(both.samples<float>()[index * 4] == source.samples<float>()[bothExpected[index] * 4]);
    }
}

TEST_CASE("An explicit crop addresses final upright edges exactly", "[geometry]") {
    const auto source = labelled({5, 3});
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0;
    settings.geometry.rotation = QuarterTurn::Clockwise90;
    settings.geometry.crop.rectangle = UprightCropRect{0, 0.2, 2.0 / 3.0, 0.8};
    const auto output = develop(source, settings);
    REQUIRE(output.size() == ImageSize{2, 3});
    const int expected[]{11, 6, 12, 7, 13, 8};
    for (std::size_t index = 0; index < 6; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            REQUIRE(output.samples<float>()[index * 4 + channel] ==
                    source.samples<float>()[expected[index] * 4 + channel]);
        }
    }
}

TEST_CASE("Automatic framing finds the largest valid rectangle", "[geometry]") {
    GeometrySettings settings;
    settings.straighten = 45;
    const auto square = geometryPlanFor({100, 100}, ImageOrientation::Normal, settings);
    REQUIRE(std::abs(square.width - 100 / std::sqrt(2.0)) < 1e-8);
    REQUIRE(std::abs(square.height - square.width) < 1e-8);
    REQUIRE(square.outputSize == ImageSize{70, 70});
    requireValidCrop(square);

    settings.straighten = 30;
    const auto wide = geometryPlanFor({100, 50}, ImageOrientation::Normal, settings);
    REQUIRE(std::abs(wide.width - 50) < 1e-8);
    REQUIRE(std::abs(wide.height - 50 / std::sqrt(3.0)) < 1e-8);
    requireValidCrop(wide);
}

TEST_CASE("Locked crop aspects use physical dimensions and the camera orientation", "[geometry]") {
    GeometrySettings settings;
    settings.crop.aspect = CropRatio{1};
    const auto square = geometryPlanFor({61, 41}, ImageOrientation::Normal, settings);
    REQUIRE(square.outputSize == ImageSize{41, 41});
    REQUIRE(square.left == 10);

    settings.straighten = 12;
    settings.crop.aspect = OriginalCropAspect{};
    const auto portrait = geometryPlanFor({61, 41}, ImageOrientation::Rotate90, settings);
    REQUIRE(std::abs(portrait.width / portrait.height - 41.0 / 61.0) < 1e-10);
    requireValidCrop(portrait);
    settings.rotation = QuarterTurn::Clockwise90;
    const auto landscape = geometryPlanFor({61, 41}, ImageOrientation::Rotate90, settings);
    REQUIRE(std::abs(landscape.width / landscape.height - 61.0 / 41.0) < 1e-10);
    requireValidCrop(landscape);
}

TEST_CASE("Explicit crops shrink about their centre and move only when necessary", "[geometry]") {
    GeometrySettings settings;
    settings.straighten = 30;
    settings.crop.rectangle = UprightCropRect{0.15, 0.2, 0.75, 0.8};
    const auto original = settings;
    const auto fitted = geometryPlanFor({100, 60}, ImageOrientation::Normal, settings);
    REQUIRE(settings == original);
    REQUIRE(std::abs(fitted.left + fitted.width / 2 - fitted.uprightWidth * 0.45) < 1e-8);
    REQUIRE(std::abs(fitted.top + fitted.height / 2 - fitted.uprightHeight * 0.5) < 1e-8);
    REQUIRE(fitted.width <= fitted.uprightWidth * 0.6 + 1e-8);
    requireValidCrop(fitted);

    settings.crop.rectangle = UprightCropRect{0, 0, 0.1, 0.1};
    const auto moved = geometryPlanFor({100, 60}, ImageOrientation::Normal, settings);
    requireValidCrop(moved);
    REQUIRE(std::abs(moved.width / moved.height - moved.uprightWidth / moved.uprightHeight) < 1e-8);
}

TEST_CASE("Geometry mappings round trip and crops contain no empty wedges", "[geometry]") {
    for (int orientation = 1; orientation <= 8; ++orientation) {
        for (const double angle : {-45.0, -23.5, 0.0, 17.0, 45.0}) {
            for (const auto size : {ImageSize{61, 41}, ImageSize{1, 7}, ImageSize{7, 1}}) {
                GeometrySettings settings;
                settings.rotation = QuarterTurn::Clockwise90;
                settings.flipHorizontal = true;
                settings.straighten = angle;
                const auto plan = geometryPlanFor(size, static_cast<ImageOrientation>(orientation), settings);
                CAPTURE(orientation, angle, size.width, size.height);
                requireValidCrop(plan);
                for (const SourcePoint point : {SourcePoint{0, 0}, SourcePoint{0.5, 0.5},
                                                SourcePoint{double(size.width), double(size.height)}}) {
                    const auto restored = plan.toSource(plan.toUpright(point));
                    REQUIRE(std::abs(restored.x - point.x) < 1e-8);
                    REQUIRE(std::abs(restored.y - point.y) < 1e-8);
                }
            }
        }
    }
}

TEST_CASE("Straighten samples the rotated image in linear colour", "[geometry]") {
    ImageBuffer source({21, 21}, workingFormat, workingEncoding);
    auto pixels = source.samples<float>();
    for (std::uint32_t y = 0; y < 21; ++y) {
        for (std::uint32_t x = 0; x < 21; ++x) {
            const auto index = (y * 21 + x) * 4;
            pixels[index] = (x + 0.5F) / 21;
            pixels[index + 1] = (y + 0.5F) / 21;
            pixels[index + 2] = 0;
            pixels[index + 3] = 1;
        }
    }
    DevelopSettings settings;
    settings.geometry.straighten = 30;
    settings.tone.filmicHighlights = 0;
    const auto output = develop(source, settings);
    const auto plan = geometryPlanFor(source.size(), source.orientation(), settings.geometry);
    const double radians = std::numbers::pi / 6;
    for (std::uint32_t y = 0; y < output.size().height; ++y) {
        for (std::uint32_t x = 0; x < output.size().width; ++x) {
            const double dx = (x + 0.5) * plan.width / output.size().width - plan.width / 2;
            const double dy = (y + 0.5) * plan.height / output.size().height - plan.height / 2;
            const double expectedX = (std::cos(radians) * dx + std::sin(radians) * dy + 10.5) / 21;
            const double expectedY = (-std::sin(radians) * dx + std::cos(radians) * dy + 10.5) / 21;
            const auto index = (y * output.size().width + x) * 4;
            REQUIRE(std::abs(output.samples<float>()[index] - expectedX) < 1e-6);
            REQUIRE(std::abs(output.samples<float>()[index + 1] - expectedY) < 1e-6);
            REQUIRE(output.samples<float>()[index + 3] == 1);
        }
    }
}

TEST_CASE("Fractional crops interpolate alpha without coloured transparent fringes", "[geometry]") {
    ImageBuffer source({2, 1}, workingFormat, workingEncoding);
    const std::array<float, 8> pixels{1, 0, 0, 1, 0, 0, 1, 0};
    std::copy(pixels.begin(), pixels.end(), source.samples<float>().begin());
    DevelopSettings settings;
    settings.geometry.crop.rectangle = UprightCropRect{0.25, 0, 0.75, 1};
    settings.tone.filmicHighlights = 0;
    const auto output = develop(source, settings);
    REQUIRE(output.size() == ImageSize{1, 1});
    REQUIRE(output.samples<float>()[0] == 1);
    REQUIRE(output.samples<float>()[1] == 0);
    REQUIRE(output.samples<float>()[2] == 0);
    REQUIRE(output.samples<float>()[3] == 0.5F);
}

TEST_CASE("Invalid geometry cannot reach sampling", "[geometry]") {
    const auto source = labelled({5, 3});
    DevelopSettings settings;
    for (const double angle : {46.0, -46.0, std::numeric_limits<double>::quiet_NaN()}) {
        settings.geometry.straighten = angle;
        REQUIRE_THROWS_AS(develop(source, settings), std::invalid_argument);
    }
    settings = {};
    settings.geometry.rotation = static_cast<QuarterTurn>(42);
    REQUIRE_THROWS_AS(develop(source, settings), std::invalid_argument);
    settings = {};
    for (const auto crop : {UprightCropRect{0, 0, 0, 1}, UprightCropRect{-0.1, 0, 1, 1},
                            UprightCropRect{0, 0, 1, std::numeric_limits<double>::infinity()}}) {
        settings.geometry.crop.rectangle = crop;
        REQUIRE_THROWS_AS(develop(source, settings), std::invalid_argument);
    }
    settings = {};
    for (const double ratio : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
        settings.geometry.crop.aspect = CropRatio{ratio};
        REQUIRE_THROWS_AS(develop(source, settings), std::invalid_argument);
    }
    settings.geometry.crop.aspect = CropRatio{1};
    settings.geometry.crop.rectangle = UprightCropRect{};
    REQUIRE_THROWS_AS(develop(source, settings), std::invalid_argument);
}

TEST_CASE("A RAW camera orientation survives decoding and is applied once", "[geometry][raw]") {
    const auto path = test::fixture("linear-32x24-rotated.dng");
    const auto metadata = readImageMetadata(path);
    const auto source = loadImage(path);
    REQUIRE(metadata.orientation == ImageOrientation::Rotate90);
    REQUIRE(source.orientation() == metadata.orientation);
    REQUIRE(source.size() == ImageSize{32, 24});
    const auto output = develop(source, {});
    REQUIRE(output.size() == ImageSize{24, 32});
    REQUIRE(output.orientation() == ImageOrientation::Normal);
    REQUIRE(develop(output, {}).size() == output.size());
}

TEST_CASE("Qt orientation metadata survives decoding without rearranging pixels", "[geometry]") {
    const test::TempDir directory;
    QImage source(5, 3, QImage::Format_RGB32);
    source.fill(Qt::red);
    const QImageIOHandler::Transformation transforms[]{
        QImageIOHandler::TransformationNone, QImageIOHandler::TransformationMirror,
        QImageIOHandler::TransformationRotate180, QImageIOHandler::TransformationFlip,
        QImageIOHandler::TransformationFlipAndRotate90, QImageIOHandler::TransformationRotate90,
        QImageIOHandler::TransformationMirrorAndRotate90, QImageIOHandler::TransformationRotate270};
    for (int index = 0; index < 8; ++index) {
        const auto path = directory.file("oriented.tif");
        QImageWriter writer(QString::fromStdString(path.string()), "tiff");
        writer.setTransformation(transforms[index]);
        REQUIRE(writer.write(source));
        const auto decoded = loadImage(path);
        REQUIRE(decoded.size() == ImageSize{5, 3});
        REQUIRE(decoded.orientation() == static_cast<ImageOrientation>(index + 1));
        REQUIRE(readImageMetadata(path).orientation == decoded.orientation());
    }
}

TEST_CASE("Resolved plans distinguish orientation and crop changes", "[geometry][plan]") {
    const auto normal = labelled({5, 3});
    const auto rotated = labelled({5, 3}, ImageOrientation::Rotate90);
    const auto plan = planFor(normal, {});
    REQUIRE(plan.geometry.has_value());
    REQUIRE_FALSE(plan == planFor(rotated, {}));
    DevelopSettings settings;
    settings.geometry.crop.aspect = CropRatio{1};
    REQUIRE_FALSE(plan == planFor(normal, settings));
}
