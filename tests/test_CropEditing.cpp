#include "CropEditing.h"
#include "GeometryPlan.h"

#include <GeometrySettings.h>
#include <ImageBuffer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <random>
#include <variant>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// The crop mode's interaction model, apart from painting (ADR 040).

namespace {

constexpr ImageSize landscape{300, 200};

/// @brief Checks the contract every operation keeps: a valid geometry, and an explicit crop
/// that is well formed, agrees with its aspect and lies inside valid content.
void requireSound(const CropEditing& editing, ImageSize size, ImageOrientation orientation) {
    const GeometrySettings& geometry = editing.geometry();
    REQUIRE(geometry.straighten >= minimumStraighten);
    REQUIRE(geometry.straighten <= maximumStraighten);
    // Throws when the rectangle disagrees with a locked aspect by more than 1e-6.
    const GeometryPlan plan = geometryPlanFor(size, orientation, geometry);
    if (const auto& rectangle = geometry.crop.rectangle) {
        REQUIRE(isWellFormed(*rectangle));
        const UprightBox stored{rectangle->left * plan.uprightWidth,
                                rectangle->top * plan.uprightHeight,
                                (rectangle->right - rectangle->left) * plan.uprightWidth,
                                (rectangle->bottom - rectangle->top) * plan.uprightHeight};
        REQUIRE(isInsideContent(plan, stored, 1e-6 * (plan.uprightWidth + plan.uprightHeight)));
    }
    const CropBox& crop = editing.crop();
    REQUIRE(crop.left == Approx(plan.left).margin(1e-9));
    REQUIRE(crop.top == Approx(plan.top).margin(1e-9));
    REQUIRE(crop.width == Approx(plan.width));
    REQUIRE(crop.height == Approx(plan.height));
    REQUIRE(editing.uprightWidth() == Approx(plan.uprightWidth));
}

CropEditing explicitCrop(UprightCropRect rectangle, CropAspect aspect = FreeCropAspect{},
                         double straighten = 0.0) {
    GeometrySettings geometry;
    geometry.straighten = straighten;
    geometry.crop = {rectangle, aspect};
    return CropEditing(landscape, ImageOrientation::Normal, geometry);
}

} // namespace

TEST_CASE("A free corner follows the pointer and the opposite corner stays", "[crop]") {
    CropEditing editing = explicitCrop({0.2, 0.2, 0.8, 0.8});
    editing.beginGesture();
    editing.resizeTo(CropHandle::BottomRight, {200, 150});
    CHECK(editing.crop().left == Approx(60));
    CHECK(editing.crop().top == Approx(40));
    CHECK(editing.crop().right() == Approx(200));
    CHECK(editing.crop().bottom() == Approx(150));

    SECTION("past the photograph it stops at the edge") {
        editing.resizeTo(CropHandle::BottomRight, {400, 120});
        CHECK(editing.crop().right() == Approx(300));
        CHECK(editing.crop().bottom() == Approx(120));
    }
    SECTION("across the opposite corner it stops at the minimum") {
        editing.resizeTo(CropHandle::BottomRight, {0, 0});
        CHECK(editing.crop().width == Approx(editing.minimumSide()));
        CHECK(editing.crop().height == Approx(editing.minimumSide()));
        CHECK(editing.crop().left == Approx(60));
    }
    SECTION("steps start from the gesture, so moving back restores") {
        editing.resizeTo(CropHandle::BottomRight, {240, 160});
        CHECK(editing.crop().left == Approx(60));
        CHECK(editing.crop().right() == Approx(240));
        CHECK(editing.crop().bottom() == Approx(160));
    }
    requireSound(editing, landscape, ImageOrientation::Normal);
}

TEST_CASE("A free edge moves one side only", "[crop]") {
    CropEditing editing = explicitCrop({0.2, 0.2, 0.8, 0.8});
    editing.beginGesture();
    editing.resizeTo(CropHandle::Left, {10, 999});
    CHECK(editing.crop().left == Approx(10));
    CHECK(editing.crop().top == Approx(40));
    CHECK(editing.crop().right() == Approx(240));
    CHECK(editing.crop().bottom() == Approx(160));
    editing.resizeTo(CropHandle::Left, {-50, 0});
    CHECK(editing.crop().left == Approx(0).margin(1e-9));
}

TEST_CASE("A locked corner keeps the ratio, anchored at the opposite corner", "[crop]") {
    CropEditing editing = explicitCrop({0.1, 0.1, 0.5, 0.5}, CropRatio{1.5});
    REQUIRE(editing.crop().width / editing.crop().height == Approx(1.5));
    editing.beginGesture();
    editing.resizeTo(CropHandle::BottomRight, {270, 100});
    const CropBox& crop = editing.crop();
    CHECK(crop.left == Approx(30));
    CHECK(crop.top == Approx(20));
    CHECK(crop.width / crop.height == Approx(1.5));

    SECTION("it stops where the first edge meets the photograph") {
        editing.resizeTo(CropHandle::BottomRight, {1000, 1000});
        CHECK(editing.crop().right() == Approx(300));
        CHECK(editing.crop().bottom() == Approx(20 + 270 / 1.5));
        CHECK(editing.crop().width / editing.crop().height == Approx(1.5));
    }
    requireSound(editing, landscape, ImageOrientation::Normal);
}

TEST_CASE("A locked edge grows its neighbours about the middle of the opposite edge", "[crop]") {
    CropEditing editing = explicitCrop({0.4, 0.4, 0.6, 0.6}, CropRatio{1.5});
    const CropPoint middle{editing.crop().left, editing.crop().centre().y};
    editing.beginGesture();
    editing.resizeTo(CropHandle::Right, {middle.x + 90, 0});
    CHECK(editing.crop().left == Approx(middle.x));
    CHECK(editing.crop().width == Approx(90));
    CHECK(editing.crop().height == Approx(60));
    CHECK(editing.crop().centre().y == Approx(middle.y));
    requireSound(editing, landscape, ImageOrientation::Normal);
}

TEST_CASE("Dragging inside moves the image, so the crop goes the other way", "[crop]") {
    CropEditing editing = explicitCrop({0.2, 0.2, 0.6, 0.6});
    editing.beginGesture();
    editing.moveImageBy(30, -20);
    CHECK(editing.crop().left == Approx(30));
    CHECK(editing.crop().top == Approx(60));
    CHECK(editing.crop().width == Approx(120));

    SECTION("it slides along the edge of the photograph instead of leaving it") {
        editing.moveImageBy(500, -10);
        CHECK(editing.crop().left == Approx(0).margin(1e-9));
        CHECK(editing.crop().top == Approx(50));
        CHECK(editing.crop().width == Approx(120));
    }
    requireSound(editing, landscape, ImageOrientation::Normal);
}

TEST_CASE("Rotating shrinks the crop into rotated content, and turning back restores it",
          "[crop]") {
    CropEditing editing = explicitCrop({0.0, 0.0, 1.0, 1.0}, CropRatio{1.5});
    editing.rotateTo(10.0);
    CHECK(editing.geometry().straighten == 10.0);
    CHECK(editing.crop().width / editing.crop().height == Approx(1.5));
    CHECK(editing.crop().width < 300);
    CHECK(editing.crop().centre().x == Approx(editing.uprightWidth() / 2));
    requireSound(editing, landscape, ImageOrientation::Normal);
    editing.rotateTo(60.0);
    CHECK(editing.geometry().straighten == 45.0);
    editing.rotateTo(0.0);
    CHECK(editing.crop().width == Approx(300));
    CHECK(editing.crop().height == Approx(200));

    SECTION("an automatic crop stays automatic") {
        CropEditing automatic(landscape, ImageOrientation::Normal, {});
        automatic.rotateTo(-20.0);
        CHECK_FALSE(automatic.geometry().crop.rectangle);
        CHECK(automatic.geometry().straighten == -20.0);
    }
    SECTION("one flip reverses the stored angle") {
        editing.flip(true);
        editing.rotateTo(5.0);
        CHECK(editing.geometry().straighten == -5.0);
        CHECK(editing.displayedAngle() == 5.0);
    }
}

TEST_CASE("Rotation turns about the crop's centre: the content there stays there", "[crop]") {
    CropEditing editing = explicitCrop({0.3, 0.3, 0.5, 0.5});
    const auto pointUnderCentre = [&] {
        GeometrySettings frame = editing.geometry();
        frame.crop = {};
        const GeometryPlan plan = geometryPlanFor(landscape, ImageOrientation::Normal, frame);
        const CropPoint centre = editing.crop().centre();
        return plan.toSource({centre.x, centre.y});
    };
    const SourcePoint before = pointUnderCentre();
    editing.rotateTo(7.0);
    const SourcePoint after = pointUnderCentre();
    CHECK(after.x == Approx(before.x));
    CHECK(after.y == Approx(before.y));
    CHECK(editing.crop().width == Approx(60));
    requireSound(editing, landscape, ImageOrientation::Normal);
}

TEST_CASE("A drawn line becomes level or plumb, whichever is nearer", "[crop]") {
    CropEditing editing(landscape, ImageOrientation::Normal, {});
    // Falls 10 degrees to the right: rotating 10 degrees anticlockwise levels it.
    editing.straightenAlong({0, 0}, {std::cos(0.1745329252), std::sin(0.1745329252)});
    CHECK(editing.displayedAngle() == Approx(-10.0));
    // Nearly vertical, leaning 5 degrees: plumbed, from the angle already there.
    const double lean = 85.0 * 3.14159265358979 / 180.0;
    editing.straightenAlong({0, 0}, {std::cos(lean) * 50, std::sin(lean) * 50});
    CHECK(editing.displayedAngle() == Approx(-5.0));
    // Drawn right to left, the same line.
    CropEditing other(landscape, ImageOrientation::Normal, {});
    other.straightenAlong({10, 0}, {0, 1});
    CHECK(other.displayedAngle() == Approx(std::atan(0.1) * 180.0 / 3.14159265358979));
    // A point is no line.
    other.straightenAlong({5, 5}, {5, 5});
    CHECK(other.displayedAngle() == Approx(std::atan(0.1) * 180.0 / 3.14159265358979));
}

TEST_CASE("Aspect presets fit inside the present crop; free keeps it", "[crop]") {
    CropEditing editing = explicitCrop({0.0, 0.0, 1.0, 1.0});
    editing.setAspect(CropRatio{1.0});
    CHECK(editing.crop().width == Approx(200));
    CHECK(editing.crop().height == Approx(200));
    CHECK(editing.crop().centre().x == Approx(150));
    editing.setAspect(FreeCropAspect{});
    CHECK(editing.crop().width == Approx(200));
    CHECK(std::holds_alternative<FreeCropAspect>(editing.geometry().crop.aspect));
    editing.setLocked(true);
    REQUIRE(editing.lockedRatio());
    CHECK(*editing.lockedRatio() == Approx(1.0));

    SECTION("an automatic crop stays automatic at the new ratio") {
        CropEditing automatic(landscape, ImageOrientation::Normal, {});
        automatic.setAspect(OriginalCropAspect{});
        CHECK_FALSE(automatic.geometry().crop.rectangle);
        automatic.setAspect(CropRatio{0.8});
        CHECK(automatic.crop().width / automatic.crop().height == Approx(0.8));
        CHECK(automatic.crop().height == Approx(200));
    }
}

TEST_CASE("Swapping orientation reciprocates the ratio about the centre", "[crop]") {
    CropEditing editing = explicitCrop({0.3, 0.3, 0.5, 0.5}, CropRatio{1.5});
    const CropPoint centre = editing.crop().centre();
    editing.swapOrientation();
    CHECK(std::get<CropRatio>(editing.geometry().crop.aspect).widthOverHeight == Approx(1 / 1.5));
    CHECK(editing.crop().width == Approx(40));
    CHECK(editing.crop().height == Approx(60));
    CHECK(editing.crop().centre().x == Approx(centre.x));
    requireSound(editing, landscape, ImageOrientation::Normal);
}

TEST_CASE("Swapping the Original aspect twice gives the Original aspect back", "[crop]") {
    CropEditing editing(landscape, ImageOrientation::Normal, {});
    editing.setAspect(OriginalCropAspect{});
    editing.swapOrientation();
    CHECK(std::holds_alternative<CropRatio>(editing.geometry().crop.aspect));
    editing.swapOrientation();
    CHECK(std::holds_alternative<OriginalCropAspect>(editing.geometry().crop.aspect));
}

TEST_CASE("Quarter-turns and flips carry the crop with the content", "[crop]") {
    CropEditing editing = explicitCrop({0.1, 0.2, 0.4, 0.5}, CropRatio{1.5 * 0.3 / 0.3});
    const auto before = editing.geometry();
    editing.turn(true);
    CHECK(editing.geometry().rotation == QuarterTurn::Clockwise90);
    CHECK(editing.uprightWidth() == Approx(200));
    CHECK(editing.crop().left == Approx(200 * 0.5));
    requireSound(editing, landscape, ImageOrientation::Normal);
    editing.turn(false);
    CHECK(editing.geometry().rotation == QuarterTurn::None);
    CHECK(editing.geometry().crop.rectangle->left == Approx(0.1));
    CHECK(editing.geometry().crop.rectangle->bottom == Approx(0.5));

    SECTION("a flip then a turn is a turn then the other flip") {
        CropEditing first(landscape, ImageOrientation::Normal, before);
        first.flip(true);
        first.turn(true);
        CHECK(first.geometry().flipVertical);
        CHECK_FALSE(first.geometry().flipHorizontal);
        CropEditing second(landscape, ImageOrientation::Normal, before);
        second.turn(true);
        second.flip(false);
        CHECK(second.geometry().rotation == first.geometry().rotation);
        CHECK(second.geometry().flipVertical == first.geometry().flipVertical);
        CHECK(second.geometry().crop.rectangle->left ==
              Approx(first.geometry().crop.rectangle->left));
        CHECK(second.geometry().crop.rectangle->top ==
              Approx(first.geometry().crop.rectangle->top));
    }
}

TEST_CASE("A straighten from elsewhere shrinks the crop; other changes are fitted", "[crop]") {
    CropEditing editing = explicitCrop({0.0, 0.0, 1.0, 1.0});
    GeometrySettings slider = editing.geometry();
    slider.straighten = 8.0;
    editing.adopt(slider);
    CHECK(editing.geometry().straighten == 8.0);
    requireSound(editing, landscape, ImageOrientation::Normal);
    slider = editing.geometry();
    slider.straighten = 0.0;
    editing.adopt(slider);
    CHECK(editing.crop().width == Approx(300));
}

TEST_CASE("Every operation keeps the crop well formed and inside content", "[crop][fuzz]") {
    std::mt19937 random(40);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const std::array orientations{ImageOrientation::Normal, ImageOrientation::Rotate90,
                                  ImageOrientation::MirrorHorizontal, ImageOrientation::Transverse};
    const std::array sizes{ImageSize{300, 200}, ImageSize{61, 41}, ImageSize{24, 32},
                           ImageSize{4000, 3000}};
    for (int round = 0; round < 40; ++round) {
        const ImageSize size = sizes[static_cast<std::size_t>(round) % sizes.size()];
        const ImageOrientation orientation =
            orientations[static_cast<std::size_t>(round / 4) % orientations.size()];
        CropEditing editing(size, orientation, {});
        for (int step = 0; step < 150; ++step) {
            const double width = editing.uprightWidth();
            const double height = editing.uprightHeight();
            const CropPoint anywhere{(unit(random) * 1.6 - 0.3) * width,
                                     (unit(random) * 1.6 - 0.3) * height};
            switch (random() % 11) {
            case 0:
            case 1:
            case 2: {
                editing.beginGesture();
                const CropHandle handle = cropHandles[random() % cropHandles.size()];
                for (int move = 0; move < 3; ++move) {
                    editing.resizeTo(handle, {(unit(random) * 1.6 - 0.3) * width,
                                              (unit(random) * 1.6 - 0.3) * height});
                    requireSound(editing, size, orientation);
                }
                break;
            }
            case 3:
                editing.beginGesture();
                editing.moveImageBy((unit(random) - 0.5) * width, (unit(random) - 0.5) * height);
                break;
            case 4:
                editing.rotateTo(unit(random) * 100 - 50);
                break;
            case 5:
                editing.straightenAlong(
                    anywhere, {anywhere.x + unit(random) - 0.5, anywhere.y + unit(random) - 0.5});
                break;
            case 6: {
                const std::array<CropAspect, 5> aspects{FreeCropAspect{}, OriginalCropAspect{},
                                                        CropRatio{1.0}, CropRatio{0.8},
                                                        CropRatio{16.0 / 9.0}};
                editing.setAspect(aspects[random() % aspects.size()]);
                break;
            }
            case 7:
                editing.swapOrientation();
                break;
            case 8:
                editing.turn(random() % 2 == 0);
                break;
            case 9:
                editing.flip(random() % 2 == 0);
                break;
            default:
                if (random() % 4 == 0) {
                    editing.resetCrop();
                } else {
                    editing.setLocked(random() % 2 == 0);
                }
                break;
            }
            requireSound(editing, size, orientation);
        }
    }
}

TEST_CASE("The history undoes each step whole, and never past where editing began",
          "[crop][history]") {
    CropEditing editing = explicitCrop({0.2, 0.2, 0.8, 0.8});
    const GeometrySettings start = editing.geometry();
    CHECK_FALSE(editing.canUndo());
    CHECK_FALSE(editing.canRedo());

    // A rotation drag: many changes, one step.
    editing.beginStep();
    editing.rotateTo(3.0);
    editing.rotateTo(6.0);
    CHECK(editing.canUndo());
    editing.endStep();
    const GeometrySettings rotated = editing.geometry();

    // A command inside a slider's step joins it.
    editing.beginStep();
    editing.beginStep();
    editing.turn(true);
    editing.endStep();
    CHECK(editing.inStep());
    editing.endStep();
    const GeometrySettings turned = editing.geometry();

    // A step that changed nothing is not recorded.
    editing.beginStep();
    editing.endStep();

    editing.undo();
    CHECK(editing.geometry() == rotated);
    editing.undo();
    CHECK(editing.geometry() == start);
    CHECK_FALSE(editing.canUndo());
    editing.undo();
    CHECK(editing.geometry() == start);
    requireSound(editing, landscape, ImageOrientation::Normal);

    editing.redo();
    CHECK(editing.geometry() == rotated);
    editing.redo();
    CHECK(editing.geometry() == turned);
    CHECK_FALSE(editing.canRedo());

    // A new step after an undo forgets what could be redone.
    editing.undo();
    editing.beginStep();
    editing.swapOrientation();
    editing.endStep();
    CHECK_FALSE(editing.canRedo());
    editing.undo();
    CHECK(editing.geometry() == rotated);
}

TEST_CASE("Undo closes an open step first, and rotation starts afresh after it",
          "[crop][history]") {
    CropEditing editing = explicitCrop({0.0, 0.0, 1.0, 1.0});
    const GeometrySettings start = editing.geometry();
    editing.beginStep();
    editing.rotateTo(10.0);
    editing.undo();
    CHECK_FALSE(editing.inStep());
    CHECK(editing.geometry() == start);
    CHECK(editing.canRedo());
    // The next rotation works from the restored geometry, not from the undone run.
    editing.rotateTo(4.0);
    CHECK(editing.geometry().straighten == Approx(4.0));
    requireSound(editing, landscape, ImageOrientation::Normal);
}
