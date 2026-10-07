#include "GeometryPlan.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <CropGeometry.h>
#include <ImageImport.h>

#include <QImage>
#include <QImageIOHandler>
#include <QImageWriter>
#include <QString>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <random>
#include <stdexcept>
#include <variant>

using namespace arraw;
using Catch::Approx;

/// The rules of ADR 014 for editing a geometry, as pure functions.

namespace {

constexpr SourceShape landscape{{300, 200}, ImageOrientation::Normal};

/// @brief Checks the contract every rule keeps: a valid geometry, and an explicit crop that is
/// well formed, agrees with its aspect and lies inside valid content.
void requireSound(SourceShape source, const GeometrySettings& geometry) {
    REQUIRE(geometry.straighten >= minimumStraighten);
    REQUIRE(geometry.straighten <= maximumStraighten);
    // Throws when the rectangle disagrees with a locked aspect by more than 1e-6.
    const GeometryPlan plan = geometryPlanFor(source.size, source.orientation, geometry);
    if (const auto& rectangle = geometry.crop.rectangle) {
        REQUIRE(isWellFormed(*rectangle));
        const UprightBox stored{rectangle->left * plan.uprightWidth,
                                rectangle->top * plan.uprightHeight,
                                (rectangle->right - rectangle->left) * plan.uprightWidth,
                                (rectangle->bottom - rectangle->top) * plan.uprightHeight};
        REQUIRE(isInsideContent(plan, stored, 1e-6 * (plan.uprightWidth + plan.uprightHeight)));
    }
}

GeometrySettings explicitCrop(UprightCropRect rectangle, CropAspect aspect = FreeCropAspect{},
                              double straighten = 0.0) {
    GeometrySettings geometry;
    geometry.straighten = straighten;
    geometry.crop = {rectangle, aspect};
    return geometry;
}

CropBox cropOf(SourceShape source, const GeometrySettings& geometry) {
    return cropFrameFor(source, geometry).crop;
}

/// @brief Gives the point of the decoded photograph under the crop's centre.
SourcePoint pointUnderCentre(SourceShape source, GeometrySettings geometry) {
    const CropPoint centre = cropOf(source, geometry).centre();
    geometry.crop = {};
    return geometryPlanFor(source.size, source.orientation, geometry)
        .toSource({centre.x, centre.y});
}

} // namespace

TEST_CASE("Rotating shrinks the crop into rotated content, and turning back restores it",
          "[cropgeometry]") {
    const GeometrySettings start = explicitCrop({0.0, 0.0, 1.0, 1.0}, CropRatio{1.5});
    const GeometrySettings rotated = rotatedTo(landscape, start, 10.0);
    CHECK(rotated.straighten == 10.0);
    const CropBox crop = cropOf(landscape, rotated);
    const CropFrame frame = cropFrameFor(landscape, rotated);
    CHECK(crop.width / crop.height == Approx(1.5));
    CHECK(crop.width < 300);
    CHECK(crop.centre().x == Approx(frame.uprightWidth / 2));
    requireSound(landscape, rotated);
    CHECK(rotatedTo(landscape, start, 60.0).straighten == 45.0);
    const CropBox back = cropOf(landscape, rotatedTo(landscape, start, 0.0));
    CHECK(back.width == Approx(300));
    CHECK(back.height == Approx(200));

    SECTION("an automatic crop stays automatic") {
        const GeometrySettings automatic = rotatedTo(landscape, {}, -20.0);
        CHECK_FALSE(automatic.crop.rectangle);
        CHECK(automatic.straighten == -20.0);
    }
    SECTION("one flip reverses the stored angle") {
        const GeometrySettings flippedStart = flipped(start, true);
        const GeometrySettings next =
            rotatedTo(landscape, flippedStart, storedStraighten(flippedStart, 5.0));
        CHECK(next.straighten == -5.0);
        CHECK(displayedStraighten(next) == 5.0);
    }
    SECTION("a straighten that is not finite is refused") {
        CHECK_THROWS_AS(rotatedTo(landscape, start, std::nan("")), std::invalid_argument);
    }
}

TEST_CASE("Rotation turns about the crop's centre: the content there stays there",
          "[cropgeometry]") {
    const GeometrySettings start = explicitCrop({0.3, 0.3, 0.5, 0.5});
    const SourcePoint before = pointUnderCentre(landscape, start);
    const GeometrySettings next = rotatedTo(landscape, start, 7.0);
    const SourcePoint after = pointUnderCentre(landscape, next);
    CHECK(after.x == Approx(before.x));
    CHECK(after.y == Approx(before.y));
    CHECK(cropOf(landscape, next).width == Approx(60));
    requireSound(landscape, next);
}

TEST_CASE("A drawn line becomes level or plumb, whichever is nearer", "[cropgeometry]") {
    // Falls 10 degrees to the right: rotating 10 degrees anticlockwise levels it.
    GeometrySettings geometry =
        straightenedAlong(landscape, {}, {0, 0}, {std::cos(0.1745329252), std::sin(0.1745329252)});
    CHECK(displayedStraighten(geometry) == Approx(-10.0));
    // Nearly vertical, leaning 5 degrees: plumbed, from the angle already there.
    const double lean = 85.0 * 3.14159265358979 / 180.0;
    geometry =
        straightenedAlong(landscape, geometry, {0, 0}, {std::cos(lean) * 50, std::sin(lean) * 50});
    CHECK(displayedStraighten(geometry) == Approx(-5.0));
    // Drawn right to left, the same line.
    const GeometrySettings other = straightenedAlong(landscape, {}, {10, 0}, {0, 1});
    CHECK(displayedStraighten(other) == Approx(std::atan(0.1) * 180.0 / 3.14159265358979));
    // A point is no line.
    CHECK(straightenedAlong(landscape, other, {5, 5}, {5, 5}) == other);
}

TEST_CASE("Aspect presets fit inside the present crop; free keeps it", "[cropgeometry]") {
    GeometrySettings geometry =
        withAspect(landscape, explicitCrop({0.0, 0.0, 1.0, 1.0}), CropRatio{1.0});
    CropBox crop = cropOf(landscape, geometry);
    CHECK(crop.width == Approx(200));
    CHECK(crop.height == Approx(200));
    CHECK(crop.centre().x == Approx(150));
    geometry = withAspect(landscape, geometry, FreeCropAspect{});
    CHECK(cropOf(landscape, geometry).width == Approx(200));
    CHECK(std::holds_alternative<FreeCropAspect>(geometry.crop.aspect));
    geometry = withLockedAspect(landscape, geometry);
    const auto ratio = lockedRatio(cropFrameFor(landscape, geometry), geometry.crop.aspect);
    REQUIRE(ratio);
    CHECK(*ratio == Approx(1.0));
    CHECK(withLockedAspect(landscape, geometry) == geometry);

    SECTION("an automatic crop stays automatic at the new ratio") {
        const GeometrySettings original = withAspect(landscape, {}, OriginalCropAspect{});
        CHECK_FALSE(original.crop.rectangle);
        const GeometrySettings narrow = withAspect(landscape, original, CropRatio{0.8});
        CHECK_FALSE(narrow.crop.rectangle);
        const CropBox box = cropOf(landscape, narrow);
        CHECK(box.width / box.height == Approx(0.8));
        CHECK(box.height == Approx(200));
    }
    SECTION("a ratio that is not well formed is refused") {
        CHECK_THROWS_AS(withAspect(landscape, {}, CropRatio{0.0}), std::invalid_argument);
        CHECK_THROWS_AS(withAspect(landscape, {}, CropRatio{std::nan("")}), std::invalid_argument);
    }
}

TEST_CASE("Swapping orientation reciprocates the ratio about the centre", "[cropgeometry]") {
    const GeometrySettings start = explicitCrop({0.3, 0.3, 0.5, 0.5}, CropRatio{1.5});
    const CropPoint centre = cropOf(landscape, start).centre();
    const GeometrySettings next = withSwappedOrientation(landscape, start);
    CHECK(std::get<CropRatio>(next.crop.aspect).widthOverHeight == Approx(1 / 1.5));
    const CropBox crop = cropOf(landscape, next);
    CHECK(crop.width == Approx(40));
    CHECK(crop.height == Approx(60));
    CHECK(crop.centre().x == Approx(centre.x));
    requireSound(landscape, next);
}

TEST_CASE("Swapping the Original aspect twice gives the Original aspect back", "[cropgeometry]") {
    GeometrySettings geometry = withAspect(landscape, {}, OriginalCropAspect{});
    geometry = withSwappedOrientation(landscape, geometry);
    CHECK(std::holds_alternative<CropRatio>(geometry.crop.aspect));
    geometry = withSwappedOrientation(landscape, geometry);
    CHECK(std::holds_alternative<OriginalCropAspect>(geometry.crop.aspect));
}

TEST_CASE("Quarter-turns and flips carry the crop with the content", "[cropgeometry]") {
    const GeometrySettings before = explicitCrop({0.1, 0.2, 0.4, 0.5}, CropRatio{1.5});
    GeometrySettings geometry = turned(before, true);
    CHECK(geometry.rotation == QuarterTurn::Clockwise90);
    const CropFrame frame = cropFrameFor(landscape, geometry);
    CHECK(frame.uprightWidth == Approx(200));
    CHECK(frame.crop.left == Approx(200 * 0.5));
    requireSound(landscape, geometry);
    geometry = turned(geometry, false);
    CHECK(geometry.rotation == QuarterTurn::None);
    CHECK(geometry.crop.rectangle->left == Approx(0.1));
    CHECK(geometry.crop.rectangle->bottom == Approx(0.5));

    SECTION("a flip then a turn is a turn then the other flip") {
        const GeometrySettings first = turned(flipped(before, true), true);
        CHECK(first.flipVertical);
        CHECK_FALSE(first.flipHorizontal);
        const GeometrySettings second = flipped(turned(before, true), false);
        CHECK(second.rotation == first.rotation);
        CHECK(second.flipVertical == first.flipVertical);
        CHECK(second.crop.rectangle->left == Approx(first.crop.rectangle->left));
        CHECK(second.crop.rectangle->top == Approx(first.crop.rectangle->top));
    }
}

TEST_CASE("Setting the stored rotation carries the crop with the content", "[cropgeometry]") {
    const GeometrySettings base = explicitCrop({0.1, 0.2, 0.4, 0.5}, CropRatio{1.5});
    SECTION("with no flips it matches a clockwise turn, flips apart") {
        const GeometrySettings next = withRotation(base, QuarterTurn::Clockwise90);
        const GeometrySettings turnedOnce = turned(base, true);
        CHECK(next.rotation == turnedOnce.rotation);
        CHECK(next.crop == turnedOnce.crop);
        CHECK_FALSE(next.flipHorizontal);
        CHECK_FALSE(next.flipVertical);
    }
    SECTION("the content under the crop stays under it, whatever the flips") {
        for (const bool horizontal : {false, true}) {
            for (const bool vertical : {false, true}) {
                for (const auto target : {QuarterTurn::Clockwise90, QuarterTurn::Clockwise180,
                                          QuarterTurn::Clockwise270, QuarterTurn::None}) {
                    GeometrySettings start = base;
                    start.flipHorizontal = horizontal;
                    start.flipVertical = vertical;
                    const GeometrySettings next = withRotation(start, target);
                    CHECK(next.flipHorizontal == horizontal);
                    CHECK(next.flipVertical == vertical);
                    const SourcePoint a = pointUnderCentre(landscape, start);
                    const SourcePoint b = pointUnderCentre(landscape, next);
                    CHECK(a.x == Approx(b.x));
                    CHECK(a.y == Approx(b.y));
                    requireSound(landscape, next);
                }
            }
        }
    }
    SECTION("a ratio is reciprocated on an odd change only") {
        CHECK(std::get<CropRatio>(withRotation(base, QuarterTurn::Clockwise90).crop.aspect)
                  .widthOverHeight == Approx(1 / 1.5));
        CHECK(std::get<CropRatio>(withRotation(base, QuarterTurn::Clockwise180).crop.aspect)
                  .widthOverHeight == Approx(1.5));
    }
    SECTION("setting the value it has changes nothing") {
        CHECK(withRotation(base, QuarterTurn::None) == base);
    }
}

TEST_CASE("A straighten from elsewhere shrinks the crop; other changes are fitted",
          "[cropgeometry]") {
    const GeometrySettings start = explicitCrop({0.0, 0.0, 1.0, 1.0});
    const GeometrySettings slider = rotatedTo(landscape, start, 8.0);
    CHECK(slider.straighten == 8.0);
    CHECK(cropOf(landscape, slider).width < 300);
    requireSound(landscape, slider);
    // From the same baseline, turning back restores the crop.
    CHECK(rotatedTo(landscape, start, 0.0) == start);
}

TEST_CASE("Moving the crop slides it along valid content", "[cropgeometry]") {
    const GeometrySettings start = explicitCrop({0.2, 0.2, 0.6, 0.6});
    const GeometrySettings moved = withCropMovedBy(landscape, start, -30, 20);
    CHECK(cropOf(landscape, moved).left == Approx(30));
    CHECK(cropOf(landscape, moved).top == Approx(60));
    CHECK(cropOf(landscape, moved).width == Approx(120));
    const GeometrySettings slid = withCropMovedBy(landscape, start, -500, 10);
    CHECK(cropOf(landscape, slid).left == Approx(0).margin(1e-9));
    CHECK(cropOf(landscape, slid).top == Approx(50));
    CHECK(cropOf(landscape, slid).width == Approx(120));
    requireSound(landscape, slid);
}

TEST_CASE("Resetting the crop keeps the aspect", "[cropgeometry]") {
    const GeometrySettings reset =
        withCropReset(explicitCrop({0.2, 0.2, 0.6, 0.6}, CropRatio{1.0}));
    CHECK_FALSE(reset.crop.rectangle);
    CHECK(std::holds_alternative<CropRatio>(reset.crop.aspect));
}

TEST_CASE("A crop inside content is kept, one outside is fitted, a mismatch is refused",
          "[cropgeometry]") {
    const GeometrySettings inside = explicitCrop({0.2, 0.2, 0.6, 0.6}, FreeCropAspect{}, 5.0);
    CHECK(fittedCrop(landscape, inside) == inside);
    CHECK(fittedCrop(landscape, {}) == GeometrySettings{});

    const GeometrySettings outside = explicitCrop({0.0, 0.0, 1.0, 1.0}, FreeCropAspect{}, 8.0);
    const GeometrySettings fitted = fittedCrop(landscape, outside);
    CHECK_FALSE(fitted == outside);
    requireSound(landscape, fitted);
    CHECK(cropOf(landscape, fitted).width == Approx(cropOf(landscape, outside).width));
    CHECK(cropOf(landscape, fitted).height == Approx(cropOf(landscape, outside).height));

    CHECK_THROWS_AS(fittedCrop(landscape, explicitCrop({0.0, 0.0, 1.0, 1.0}, CropRatio{3.0})),
                    std::invalid_argument);
}

TEST_CASE("A frame matches the engine's plan", "[cropgeometry]") {
    for (const auto orientation :
         {ImageOrientation::Normal, ImageOrientation::Rotate90, ImageOrientation::Transverse}) {
        const SourceShape source{{300, 200}, orientation};
        GeometrySettings geometry = explicitCrop({0.1, 0.2, 0.7, 0.8}, FreeCropAspect{}, 6.0);
        geometry.flipHorizontal = true;
        const CropFrame frame = cropFrameFor(source, geometry);
        const GeometryPlan plan = geometryPlanFor(source.size, source.orientation, geometry);
        CHECK(frame.uprightWidth == plan.uprightWidth);
        CHECK(frame.uprightHeight == plan.uprightHeight);
        CHECK(frame.crop == CropBox{plan.left, plan.top, plan.width, plan.height});
        const auto corners = contentCorners(plan);
        for (std::size_t index = 0; index < corners.size(); ++index) {
            CHECK(frame.content[index] == CropPoint{corners[index].x, corners[index].y});
        }
    }
    GeometrySettings invalid;
    invalid.straighten = 46.0;
    CHECK_THROWS_AS(cropFrameFor(landscape, invalid), std::invalid_argument);
}

TEST_CASE("The displayed and the stored straighten convert back and forth", "[cropgeometry]") {
    for (const bool horizontal : {false, true}) {
        for (const bool vertical : {false, true}) {
            GeometrySettings geometry;
            geometry.flipHorizontal = horizontal;
            geometry.flipVertical = vertical;
            geometry.straighten = storedStraighten(geometry, 12.5);
            CHECK(displayedStraighten(geometry) == 12.5);
        }
    }
}

TEST_CASE("A shape from the metadata is the shape of the decoded photograph", "[cropgeometry]") {
    for (const auto* name : {"linear-32x24-neutral.dng", "linear-32x24-rotated.dng",
                             "bayer-32x24.dng", "testcard-61x41-srgb8.png"}) {
        const auto path = test::fixture(name);
        CHECK(shapeOf(readImageMetadata(path)) == shapeOf(loadImage(path)));
    }

    const test::TempDir directory;
    QImage source(5, 3, QImage::Format_RGB32);
    source.fill(Qt::red);
    const QImageIOHandler::Transformation transforms[]{
        QImageIOHandler::TransformationNone,
        QImageIOHandler::TransformationMirror,
        QImageIOHandler::TransformationRotate180,
        QImageIOHandler::TransformationFlip,
        QImageIOHandler::TransformationFlipAndRotate90,
        QImageIOHandler::TransformationRotate90,
        QImageIOHandler::TransformationMirrorAndRotate90,
        QImageIOHandler::TransformationRotate270};
    for (const auto transform : transforms) {
        const auto path = directory.file("oriented.tif");
        QImageWriter writer(QString::fromStdString(path.string()), "tiff");
        writer.setTransformation(transform);
        REQUIRE(writer.write(source));
        CHECK(shapeOf(readImageMetadata(path)) == shapeOf(loadImage(path)));
    }
}

TEST_CASE("Every rule keeps the crop well formed and inside content", "[cropgeometry][fuzz]") {
    std::mt19937 random(40);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const std::array orientations{ImageOrientation::Normal, ImageOrientation::Rotate90,
                                  ImageOrientation::MirrorHorizontal, ImageOrientation::Transverse};
    const std::array sizes{ImageSize{300, 200}, ImageSize{61, 41}, ImageSize{24, 32},
                           ImageSize{4000, 3000}};
    for (int round = 0; round < 40; ++round) {
        const SourceShape source{
            sizes[static_cast<std::size_t>(round) % sizes.size()],
            orientations[static_cast<std::size_t>(round / 4) % orientations.size()]};
        GeometrySettings geometry;
        for (int step = 0; step < 150; ++step) {
            const CropFrame frame = cropFrameFor(source, geometry);
            const double width = frame.uprightWidth;
            const double height = frame.uprightHeight;
            const CropPoint anywhere{(unit(random) * 1.6 - 0.3) * width,
                                     (unit(random) * 1.6 - 0.3) * height};
            switch (random() % 10) {
            case 0:
            case 1:
                geometry = withCropMovedBy(source, geometry, (unit(random) - 0.5) * width,
                                           (unit(random) - 0.5) * height);
                break;
            case 2:
                geometry = rotatedTo(source, geometry, unit(random) * 100 - 50);
                break;
            case 3:
                geometry = straightenedAlong(
                    source, geometry, anywhere,
                    {anywhere.x + unit(random) - 0.5, anywhere.y + unit(random) - 0.5});
                break;
            case 4: {
                const std::array<CropAspect, 5> aspects{FreeCropAspect{}, OriginalCropAspect{},
                                                        CropRatio{1.0}, CropRatio{0.8},
                                                        CropRatio{16.0 / 9.0}};
                geometry = withAspect(source, geometry, aspects[random() % aspects.size()]);
                break;
            }
            case 5:
                geometry = withSwappedOrientation(source, geometry);
                break;
            case 6:
                geometry = turned(geometry, random() % 2 == 0);
                break;
            case 7:
                geometry = flipped(geometry, random() % 2 == 0);
                break;
            case 8:
                geometry = random() % 2 == 0
                               ? withRotation(geometry, static_cast<QuarterTurn>(random() % 4))
                               : fittedCrop(source, geometry);
                break;
            default:
                geometry = random() % 4 == 0 ? withCropReset(geometry)
                                             : withLockedAspect(source, geometry);
                break;
            }
            requireSound(source, geometry);
        }
    }
}
