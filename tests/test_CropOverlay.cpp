#include "CropEditing.h"
#include "ui/CropOverlay.h"

#include <EditSession.h>
#include <GeometrySettings.h>
#include <Photo.h>

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// The crop mode's mouse and keys, and the one edit a session is (ADR 040).

namespace {

constexpr ImageSize photoSize{300, 200};

GeometrySettings centredCrop() {
    GeometrySettings geometry;
    geometry.crop.rectangle = UprightCropRect{0.25, 0.25, 0.75, 0.75};
    return geometry;
}

DevelopState stateWith(const GeometrySettings& geometry) {
    DevelopState state;
    state.settings.geometry = geometry;
    return state;
}

/// An overlay over a photograph, wired to a session as the window wires it.
struct Fixture {
    EditSession session;
    CropOverlay overlay;
    std::vector<bool> finished;
    int edits = 0;

    explicit Fixture(GeometrySettings geometry = centredCrop())
        : session(
              Photo{"photo.dng", ImageMetadata{photoSize, workingEncoding}, stateWith(geometry)}) {
        overlay.resize(400, 300);
        QObject::connect(&overlay, &CropOverlay::geometryEdited,
                         [this](const GeometrySettings& next) {
                             ++edits;
                             DevelopState updated = session.photo().state();
                             updated.settings.geometry = next;
                             session.update(updated);
                         });
        QObject::connect(&overlay, &CropOverlay::finished, [this](bool accepted) {
            finished.push_back(accepted);
            if (accepted) {
                session.commit();
            } else {
                session.cancel();
            }
        });
        session.begin();
        overlay.start(CropEditing(photoSize, ImageOrientation::Normal,
                                  session.photo().state().settings.geometry));
        overlay.setImage(QImage(30, 20, QImage::Format_RGB32));
    }

    [[nodiscard]] QPointF at(double x, double y) const {
        return overlay.widgetFromUpright({x, y});
    }

    void mouse(QEvent::Type type, QPointF position, Qt::KeyboardModifiers modifiers = {}) {
        const Qt::MouseButtons held =
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, position, overlay.mapToGlobal(position), Qt::LeftButton, held,
                          modifiers);
        QCoreApplication::sendEvent(&overlay, &event);
    }

    void drag(QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = {}) {
        mouse(QEvent::MouseButtonPress, from, modifiers);
        for (int step = 1; step <= 4; ++step) {
            mouse(QEvent::MouseMove, from + (to - from) * step / 4.0, modifiers);
        }
        mouse(QEvent::MouseButtonRelease, to, modifiers);
    }

    void key(Qt::Key code) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
        QCoreApplication::sendEvent(&overlay, &press);
    }

    [[nodiscard]] const GeometrySettings& geometry() const {
        return session.photo().state().settings.geometry;
    }
};

} // namespace

TEST_CASE("The overlay tells handles, inside and outside apart", "[app][crop]") {
    Fixture fixture;
    const QRectF frame = fixture.overlay.cropRect();
    REQUIRE(frame.left() == Approx(fixture.at(75, 50).x()));
    REQUIRE(frame.bottom() == Approx(fixture.at(225, 150).y()));

    const auto handleAt = [&](QPointF position) {
        const CropHit hit = fixture.overlay.hitAt(position);
        REQUIRE(hit.kind == CropHit::Kind::Handle);
        return hit.handle;
    };
    CHECK(handleAt(frame.topLeft()) == CropHandle::TopLeft);
    CHECK(handleAt(frame.topLeft() + QPointF(-6, 5)) == CropHandle::TopLeft);
    CHECK(handleAt(frame.bottomRight()) == CropHandle::BottomRight);
    CHECK(handleAt(frame.topRight()) == CropHandle::TopRight);
    CHECK(handleAt(QPointF(frame.center().x(), frame.top() + 4)) == CropHandle::Top);
    CHECK(handleAt(QPointF(frame.left() - 5, frame.center().y() + 20)) == CropHandle::Left);
    CHECK(handleAt(QPointF(frame.right() + 3, frame.top() + 30)) == CropHandle::Right);
    CHECK(handleAt(QPointF(frame.center().x() - 30, frame.bottom())) == CropHandle::Bottom);
    CHECK(fixture.overlay.hitAt(frame.center()).kind == CropHit::Kind::Inside);
    CHECK(fixture.overlay.hitAt(frame.center() + QPointF(-40, 30)).kind == CropHit::Kind::Inside);
    CHECK(fixture.overlay.hitAt(QPointF(frame.left() - 30, frame.center().y())).kind ==
          CropHit::Kind::Outside);
    CHECK(fixture.overlay.hitAt(QPointF(2, 2)).kind == CropHit::Kind::Outside);
}

TEST_CASE("A corner drag resizes, and the session holds one edit", "[app][crop]") {
    Fixture fixture;
    const GeometrySettings before = fixture.geometry();
    fixture.drag(fixture.at(225, 150), fixture.at(260, 180));
    CHECK(fixture.edits > 1);
    REQUIRE(fixture.geometry().crop.rectangle);
    CHECK(fixture.geometry().crop.rectangle->right == Approx(260.0 / 300));
    CHECK(fixture.geometry().crop.rectangle->bottom == Approx(180.0 / 200));
    CHECK(fixture.geometry().crop.rectangle->left == Approx(0.25));

    fixture.key(Qt::Key_Return);
    REQUIRE(fixture.finished == std::vector<bool>{true});
    CHECK_FALSE(fixture.overlay.isActive());
    CHECK_FALSE(fixture.session.editing());
    // One step: undoing it returns to where the session began.
    REQUIRE(fixture.session.canUndo());
    fixture.session.undo();
    CHECK(fixture.geometry() == before);
    CHECK_FALSE(fixture.session.canUndo());
}

TEST_CASE("Several drags in one session are still one edit", "[app][crop]") {
    Fixture fixture;
    const GeometrySettings before = fixture.geometry();
    const QPointF centre = fixture.overlay.cropRect().center();
    fixture.drag(centre, centre + QPointF(20, 10));
    const QRectF frame = fixture.overlay.cropRect();
    fixture.drag(QPointF(frame.left(), frame.center().y()),
                 QPointF(frame.left() - 15, frame.center().y()));
    fixture.drag(QPointF(frame.right() + 40, frame.center().y()),
                 QPointF(frame.right() + 40, frame.center().y() + 30));
    CHECK(fixture.geometry().straighten != 0.0);
    fixture.key(Qt::Key_Enter);
    fixture.session.undo();
    CHECK(fixture.geometry() == before);
    CHECK_FALSE(fixture.session.canUndo());
}

TEST_CASE("Dragging inside moves the image the other way from the crop", "[app][crop]") {
    Fixture fixture;
    const QPointF centre = fixture.overlay.cropRect().center();
    const QPointF delta = fixture.at(30, 20) - fixture.at(0, 0);
    fixture.mouse(QEvent::MouseButtonPress, centre);
    fixture.mouse(QEvent::MouseMove, centre + delta);
    // The frame stays where it was on screen while the image moves under it.
    CHECK(fixture.overlay.cropRect().center().x() == Approx(centre.x()));
    fixture.mouse(QEvent::MouseButtonRelease, centre + delta);
    REQUIRE(fixture.geometry().crop.rectangle);
    CHECK(fixture.geometry().crop.rectangle->left == Approx(45.0 / 300));
    CHECK(fixture.geometry().crop.rectangle->top == Approx(30.0 / 200));
}

TEST_CASE("A drag outside rotates and shrinks the crop into the photograph", "[app][crop]") {
    GeometrySettings full;
    full.crop.rectangle = UprightCropRect{};
    Fixture fixture(full);
    const QRectF frame = fixture.overlay.cropRect();
    const QPointF pivot = frame.center();
    const QPointF start(frame.right() + 15, pivot.y());
    // A quarter of the way to straight down: clockwise on screen.
    fixture.mouse(QEvent::MouseButtonPress, start);
    fixture.mouse(QEvent::MouseMove, pivot + QPointF(100, 100 * 0.17632698));
    CHECK(fixture.geometry().straighten == Approx(10.0).margin(0.01));
    CHECK(fixture.overlay.editing().crop().width < 300);
    fixture.mouse(QEvent::MouseButtonRelease, pivot + QPointF(100, 100 * 0.17632698));
}

TEST_CASE("Esc restores the state the session began with", "[app][crop]") {
    Fixture fixture;
    const GeometrySettings before = fixture.geometry();
    fixture.drag(fixture.at(75, 50), fixture.at(20, 20));
    REQUIRE(fixture.geometry() != before);
    fixture.key(Qt::Key_Escape);
    REQUIRE(fixture.finished == std::vector<bool>{false});
    CHECK(fixture.geometry() == before);
    CHECK_FALSE(fixture.session.canUndo());
}

TEST_CASE("Ctrl with a drag straightens along the line drawn", "[app][crop]") {
    Fixture fixture(GeometrySettings{});
    const QPointF from = fixture.at(50, 100);
    // Falls by 10 units over 100: rotating anticlockwise levels it.
    fixture.drag(from, fixture.at(150, 110), Qt::ControlModifier);
    CHECK(fixture.geometry().straighten == Approx(-5.7105931));

    SECTION("the straighten tool does the same without Ctrl, once") {
        fixture.overlay.setStraightening(true);
        fixture.drag(fixture.at(100, 50), fixture.at(110, 150));
        CHECK_FALSE(fixture.overlay.isStraightening());
    }
}

TEST_CASE("O cycles the guides and X swaps orientation", "[app][crop]") {
    Fixture fixture;
    CHECK(fixture.overlay.guide() == CropGuide::Thirds);
    fixture.key(Qt::Key_O);
    CHECK(fixture.overlay.guide() == CropGuide::Grid);
    fixture.key(Qt::Key_O);
    fixture.key(Qt::Key_O);
    fixture.key(Qt::Key_O);
    CHECK(fixture.overlay.guide() == CropGuide::Thirds);
    fixture.key(Qt::Key_X);
    const auto& crop = fixture.overlay.editing().crop();
    CHECK(crop.width == Approx(100));
    CHECK(crop.height == Approx(150));
}

TEST_CASE("A double-click inside accepts", "[app][crop]") {
    Fixture fixture;
    const QPointF centre = fixture.overlay.cropRect().center();
    fixture.mouse(QEvent::MouseButtonDblClick, centre);
    CHECK(fixture.finished == std::vector<bool>{true});
}

TEST_CASE("The render asked for follows the scale shown", "[app][crop]") {
    Fixture fixture;
    const QSize size = fixture.overlay.renderSize();
    CHECK(size.width() <= 300);
    CHECK(size.width() >= 300 * 352 / 300 / 2);
    CHECK(static_cast<double>(size.width()) / size.height() == Approx(1.5).epsilon(0.02));
}

TEST_CASE("Handles are grabbed wherever they are drawn", "[app][crop]") {
    Fixture fixture;
    for (const CropHandle handle : cropHandles) {
        const std::vector<QRectF> marks = fixture.overlay.handleMarks(handle);
        REQUIRE(marks.size() == (isCorner(handle) ? 2U : 1U));
        double longest = 0.0;
        for (const QRectF& mark : marks) {
            longest = std::max({longest, mark.width(), mark.height()});
            CHECK(std::min(mark.width(), mark.height()) >= 3.0);
            const QRectF inner = mark.adjusted(0.25, 0.25, -0.25, -0.25);
            for (const QPointF& point : {inner.topLeft(), inner.topRight(), inner.bottomLeft(),
                                         inner.bottomRight(), inner.center()}) {
                const CropHit hit = fixture.overlay.hitAt(point);
                INFO("handle " << static_cast<int>(handle) << " at " << point.x() << ", "
                               << point.y());
                CHECK(hit.kind == CropHit::Kind::Handle);
                CHECK(hit.handle == handle);
            }
        }
        // Brackets and bars of about 16 logical pixels, as Lightroom's.
        CHECK(longest >= 16.0);
    }
}

TEST_CASE("A handle grabbed off its centre does not jump", "[app][crop]") {
    Fixture fixture;
    const QRectF frame = fixture.overlay.cropRect();
    const QPointF grab = frame.bottomRight() + QPointF(-6, -12);
    REQUIRE(fixture.overlay.hitAt(grab).handle == CropHandle::BottomRight);
    fixture.mouse(QEvent::MouseButtonPress, grab);
    fixture.mouse(QEvent::MouseMove, grab + QPointF(0.0, 0.0));
    CHECK(fixture.overlay.cropRect().bottomRight().x() == Approx(frame.right()));
    CHECK(fixture.overlay.cropRect().bottomRight().y() == Approx(frame.bottom()));
    fixture.mouse(QEvent::MouseMove, grab + QPointF(10.0, 5.0));
    CHECK(fixture.overlay.cropRect().right() == Approx(frame.right() + 10.0));
    CHECK(fixture.overlay.cropRect().bottom() == Approx(frame.bottom() + 5.0));
    fixture.mouse(QEvent::MouseButtonRelease, grab + QPointF(10.0, 5.0));
}

TEST_CASE("Undo walks back through the session's gestures, never past its start",
          "[app][crop][history]") {
    Fixture fixture;
    int historyChanges = 0;
    QObject::connect(&fixture.overlay, &CropOverlay::historyChanged, [&] { ++historyChanges; });
    const GeometrySettings start = fixture.geometry();
    CHECK_FALSE(fixture.overlay.canUndo());

    fixture.drag(fixture.at(225, 150), fixture.at(250, 170));
    const GeometrySettings resized = fixture.geometry();
    fixture.key(Qt::Key_X);
    const GeometrySettings swapped = fixture.geometry();
    // A slider's edit: many geometries, one gesture.
    fixture.overlay.beginStep();
    for (const double angle : {1.0, 2.0, 3.0}) {
        GeometrySettings next = fixture.geometry();
        next.straighten = angle;
        fixture.overlay.adopt(next);
        DevelopState state = fixture.session.photo().state();
        state.settings.geometry = fixture.overlay.editing().geometry();
        fixture.session.update(state);
    }
    fixture.overlay.endStep();
    CHECK(historyChanges > 0);

    fixture.overlay.undo();
    CHECK(fixture.geometry() == swapped);
    fixture.overlay.undo();
    CHECK(fixture.geometry() == resized);
    fixture.overlay.undo();
    CHECK(fixture.geometry() == start);
    CHECK_FALSE(fixture.overlay.canUndo());
    fixture.overlay.undo();
    CHECK(fixture.geometry() == start);
    CHECK(fixture.session.editing());

    fixture.overlay.redo();
    CHECK(fixture.geometry() == resized);

    // Leaving keeps one step in the photograph's history.
    fixture.key(Qt::Key_Return);
    REQUIRE(fixture.finished == std::vector<bool>{true});
    fixture.session.undo();
    CHECK(fixture.geometry() == start);
    CHECK_FALSE(fixture.session.canUndo());
}

TEST_CASE("Images turn and flip with the geometry, exactly", "[app][crop]") {
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(Qt::black);
    image.setPixelColor(0, 0, Qt::red);
    GeometrySettings turned;
    turned.rotation = QuarterTurn::Clockwise90;
    const QImage right = reorientedImage(image, GeometrySettings{}, turned);
    REQUIRE(right.size() == QSize(2, 3));
    CHECK(right.pixelColor(1, 0) == QColor(Qt::red));

    GeometrySettings mirrored = turned;
    mirrored.flipHorizontal = true;
    const QImage both = reorientedImage(image, GeometrySettings{}, mirrored);
    REQUIRE(both.size() == QSize(2, 3));
    CHECK(both.pixelColor(0, 0) == QColor(Qt::red));
    CHECK(reorientedImage(both, mirrored, GeometrySettings{}).convertToFormat(image.format()) ==
          image);
}

TEST_CASE("A placeholder shows until the render, and a turn shows at once", "[app][crop]") {
    Fixture fixture;
    fixture.overlay.start(CropEditing(photoSize, ImageOrientation::Normal, fixture.geometry()));
    CHECK(fixture.overlay.image().isNull());
    QImage preview(30, 20, QImage::Format_RGB32);
    preview.fill(Qt::black);
    preview.setPixelColor(0, 0, Qt::red);
    fixture.overlay.setPlaceholder(preview, QImage(15, 10, QImage::Format_RGB32));
    CHECK(fixture.overlay.image() == preview);
    CHECK_FALSE(fixture.overlay.hasRender());

    const GeometrySettings before = fixture.overlay.editing().geometry();
    fixture.overlay.edit([](CropEditing& editing) { editing.turn(true); });
    // Turned with the geometry, not stretched into the new frame.
    REQUIRE(fixture.overlay.image().size() == QSize(20, 30));
    CHECK(fixture.overlay.image().pixelColor(19, 0) == QColor(Qt::red));

    // A render made before the turn is turned on arrival.
    fixture.overlay.setImage(preview, before);
    CHECK(fixture.overlay.hasRender());
    CHECK(fixture.overlay.image().size() == QSize(20, 30));
    // A placeholder never replaces a render.
    fixture.overlay.setPlaceholder(preview, {});
    CHECK(fixture.overlay.image().size() == QSize(20, 30));
}
