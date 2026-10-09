#include "CurveEditing.h"
#include "ui/CurveEditor.h"

#include <ToneCurveSettings.h>

#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The curve editor's mouse and keyboard, and the edits it reports (ADR 036).

namespace {

/// What an editor reported, in order: 'S' started, 'E' edited, 'F' finished.
struct Recorder {
    std::vector<char> events;
    std::vector<ToneCurve> curves;
    std::vector<CurveChannel> channels;

    explicit Recorder(CurveEditor& editor) {
        QObject::connect(&editor, &CurveEditor::editStarted, [this] { events.push_back('S'); });
        QObject::connect(&editor, &CurveEditor::curveEdited,
                         [this](CurveChannel channel, const ToneCurve& curve) {
                             events.push_back('E');
                             curves.push_back(curve);
                             channels.push_back(channel);
                         });
        QObject::connect(&editor, &CurveEditor::editFinished, [this] { events.push_back('F'); });
        QObject::connect(&editor, &CurveEditor::resetFinished, [this] { events.push_back('F'); });
    }

    /// Edits are well formed and come in whole edits: S, then E at least once, then F.
    [[nodiscard]] bool wellFormed() const {
        for (const ToneCurve& curve : curves) {
            if (!isWellFormed(curve)) {
                return false;
            }
        }
        bool open = false;
        for (const char event : events) {
            if ((event == 'S' && open) || (event != 'S' && !open)) {
                return false;
            }
            open = event != 'F';
        }
        return !open;
    }

    [[nodiscard]] std::size_t count(char kind) const {
        return static_cast<std::size_t>(std::ranges::count(events, kind));
    }
};

void mouse(CurveEditor& editor, QEvent::Type type, QPointF at,
           Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : button;
    QMouseEvent event(type, at, editor.mapToGlobal(at), button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &event);
}

void press(CurveEditor& editor, QPointF at, Qt::MouseButton button = Qt::LeftButton) {
    mouse(editor, QEvent::MouseButtonPress, at, button);
}

void move(CurveEditor& editor, QPointF at) {
    QMouseEvent event(QEvent::MouseMove, at, editor.mapToGlobal(at), Qt::NoButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &event);
}

void release(CurveEditor& editor, QPointF at) {
    mouse(editor, QEvent::MouseButtonRelease, at);
}

void key(CurveEditor& editor, int code) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &event);
}

/// An editor sized so that its plot is a known square.
struct Fixture {
    CurveEditor editor;
    Fixture() {
        editor.resize(212, 212);
    }

    [[nodiscard]] QPointF at(float x, float y) const {
        return editor.toWidget({x, y});
    }
};

ToneCurveSettings withLuma(std::vector<CurvePoint> points) {
    ToneCurveSettings curves;
    curves.luma.points = std::move(points);
    return curves;
}

} // namespace

TEST_CASE("Showing curves reports nothing", "[app][curve][editor]") {
    Fixture fixture;
    Recorder recorder(fixture.editor);
    fixture.editor.setCurves(withLuma({{0.0F, 0.1F}, {1.0F, 1.0F}}));
    fixture.editor.setChannel(CurveChannel::Blue);
    fixture.editor.setHistogram(CurveHistogram{});
    CHECK(recorder.events.empty());
    CHECK(fixture.editor.hasHistogram());
    CHECK(fixture.editor.curves().luma.points.front().y == 0.1F);
}

TEST_CASE("The plot is square and maps both ways", "[app][curve][editor]") {
    Fixture fixture;
    const QRectF plot = fixture.editor.plotRect();
    CHECK(plot.width() == plot.height());
    CHECK(fixture.editor.toWidget({0.0F, 0.0F}) == plot.bottomLeft());
    CHECK(fixture.editor.toWidget({1.0F, 1.0F}) == plot.topRight());
    const CurvePoint back = fixture.editor.toCurve(fixture.at(0.25F, 0.75F));
    CHECK(std::abs(back.x - 0.25F) < 1e-5F);
    CHECK(std::abs(back.y - 0.75F) < 1e-5F);
    CHECK(fixture.editor.heightForWidth(300) == 300);
}

TEST_CASE("A click on the empty plot adds a point and drags it as one edit",
          "[app][curve][editor]") {
    Fixture fixture;
    Recorder recorder(fixture.editor);
    press(fixture.editor, fixture.at(0.25F, 0.5F));
    move(fixture.editor, fixture.at(0.3F, 0.6F));
    release(fixture.editor, fixture.at(0.3F, 0.6F));

    CHECK(recorder.wellFormed());
    CHECK(recorder.count('S') == 1);
    CHECK(recorder.count('F') == 1);
    const ToneCurve& luma = fixture.editor.curves().luma;
    REQUIRE(luma.points.size() == 3);
    CHECK(std::abs(luma.points[1].x - 0.3F) < 0.01F);
    CHECK(std::abs(luma.points[1].y - 0.6F) < 0.01F);
    CHECK(recorder.curves.back() == luma);
    CHECK(fixture.editor.selectedPoint() == 1U);
}

TEST_CASE("A press on a point that does not move is no edit", "[app][curve][editor]") {
    Fixture fixture;
    fixture.editor.setCurves(withLuma({{0.0F, 0.0F}, {0.5F, 0.4F}, {1.0F, 1.0F}}));
    Recorder recorder(fixture.editor);
    press(fixture.editor, fixture.at(0.5F, 0.4F));
    release(fixture.editor, fixture.at(0.5F, 0.4F));
    CHECK(recorder.events.empty());
    CHECK(fixture.editor.selectedPoint() == 1U);
}

TEST_CASE("A dragged point stops at its neighbours, and an end moves only up and down",
          "[app][curve][editor]") {
    Fixture fixture;
    fixture.editor.setCurves(withLuma({{0.0F, 0.0F}, {0.5F, 0.4F}, {0.7F, 0.8F}, {1.0F, 1.0F}}));
    Recorder recorder(fixture.editor);

    press(fixture.editor, fixture.at(0.5F, 0.4F));
    for (const float x : {0.55F, 0.65F, 0.9F}) {
        move(fixture.editor, fixture.at(x, 0.5F));
    }
    release(fixture.editor, fixture.at(0.9F, 0.5F));
    CHECK(fixture.editor.curves().luma.points[1].x <= 0.7F - minimumCurvePointSpacing + 1e-6F);

    press(fixture.editor, fixture.at(0.0F, 0.0F));
    move(fixture.editor, fixture.at(0.3F, 0.2F));
    release(fixture.editor, fixture.at(0.3F, 0.2F));
    const CurvePoint first = fixture.editor.curves().luma.points.front();
    CHECK(first.x == 0.0F);
    CHECK(std::abs(first.y - 0.2F) < 0.01F);

    CHECK(recorder.wellFormed());
    CHECK(recorder.count('S') == 2);
}

TEST_CASE("A point is removed by dragging it out, a right-click or a double-click",
          "[app][curve][editor]") {
    Fixture fixture;
    const ToneCurveSettings three = withLuma({{0.0F, 0.0F}, {0.5F, 0.4F}, {1.0F, 1.0F}});
    fixture.editor.setCurves(three);
    Recorder recorder(fixture.editor);

    SECTION("dragged out of the plot") {
        press(fixture.editor, fixture.at(0.5F, 0.4F));
        move(fixture.editor, fixture.at(0.5F, -0.8F));
        CHECK(fixture.editor.curves().luma.isIdentity());
        CHECK_FALSE(fixture.editor.selectedPoint());
        SECTION("and brought back") {
            move(fixture.editor, fixture.at(0.5F, 0.3F));
            CHECK(fixture.editor.curves().luma.points.size() == 3);
        }
        release(fixture.editor, fixture.editor.toWidget({0.5F, -0.8F}));
    }
    SECTION("right-clicked") {
        press(fixture.editor, fixture.at(0.5F, 0.4F), Qt::RightButton);
        CHECK(fixture.editor.curves().luma.isIdentity());
        CHECK(recorder.events == std::vector<char>{'S', 'E', 'F'});
    }
    SECTION("double-clicked") {
        mouse(fixture.editor, QEvent::MouseButtonDblClick, fixture.at(0.5F, 0.4F));
        CHECK(fixture.editor.curves().luma.isIdentity());
    }
    SECTION("an end is never removed") {
        press(fixture.editor, fixture.at(1.0F, 1.0F), Qt::RightButton);
        mouse(fixture.editor, QEvent::MouseButtonDblClick, fixture.at(0.0F, 0.0F));
        CHECK(fixture.editor.curves() == three);
        CHECK(recorder.events.empty());
    }
    CHECK(recorder.wellFormed());
}

TEST_CASE("A full curve takes no more points from a click", "[app][curve][editor]") {
    Fixture fixture;
    ToneCurveSettings curves;
    for (std::size_t i = 1; curves.luma.points.size() < maximumCurvePoints; ++i) {
        REQUIRE(insertPoint(curves.luma, {static_cast<float>(i) / 15.0F, 0.5F}));
    }
    fixture.editor.setCurves(curves);
    Recorder recorder(fixture.editor);
    press(fixture.editor, fixture.at(0.97F, 0.1F));
    release(fixture.editor, fixture.at(0.97F, 0.1F));
    CHECK(recorder.events.empty());
    CHECK(fixture.editor.curves() == curves);
}

TEST_CASE("Keys move and remove the selected point", "[app][curve][editor]") {
    Fixture fixture;
    fixture.editor.setCurves(withLuma({{0.0F, 0.0F}, {0.5F, 0.4F}, {1.0F, 1.0F}}));
    Recorder recorder(fixture.editor);
    key(fixture.editor, Qt::Key_PageDown);
    key(fixture.editor, Qt::Key_PageDown);
    REQUIRE(fixture.editor.selectedPoint() == 1U);

    key(fixture.editor, Qt::Key_Up);
    key(fixture.editor, Qt::Key_Up);
    key(fixture.editor, Qt::Key_Right);
    // Moves in a row are one edit, which ends when asked to.
    CHECK(recorder.count('S') == 1);
    CHECK(recorder.count('F') == 0);
    fixture.editor.finishPendingEdit();
    CHECK(recorder.count('F') == 1);
    const CurvePoint moved = fixture.editor.curves().luma.points[1];
    CHECK(std::abs(moved.x - 0.51F) < 1e-5F);
    CHECK(std::abs(moved.y - 0.42F) < 1e-5F);

    key(fixture.editor, Qt::Key_Delete);
    CHECK(fixture.editor.curves().luma.isIdentity());
    CHECK_FALSE(fixture.editor.selectedPoint());
    CHECK(recorder.wellFormed());
    CHECK(recorder.count('S') == 2);
}

TEST_CASE("Edits go to the channel shown, and a reset straightens it", "[app][curve][editor]") {
    Fixture fixture;
    Recorder recorder(fixture.editor);
    fixture.editor.setChannel(CurveChannel::Red);
    press(fixture.editor, fixture.at(0.5F, 0.7F));
    release(fixture.editor, fixture.at(0.5F, 0.7F));
    CHECK(fixture.editor.curves().luma.isIdentity());
    CHECK(fixture.editor.curves().red.points.size() == 3);
    CHECK(recorder.channels.back() == CurveChannel::Red);

    fixture.editor.resetChannel();
    CHECK(fixture.editor.curves().red.isIdentity());
    CHECK(recorder.events == std::vector<char>{'S', 'E', 'F', 'S', 'E', 'F'});

    // Nothing to straighten: no edit.
    fixture.editor.resetChannel();
    CHECK(recorder.count('S') == 2);
    CHECK(recorder.wellFormed());
}

TEST_CASE("The editor paints a curve over a histogram on every channel", "[app][curve][editor]") {
    Fixture fixture;
    fixture.editor.setCurves(withLuma({{0.0F, 0.1F}, {0.3F, 0.2F}, {0.7F, 0.9F}, {1.0F, 1.0F}}));
    CurveHistogram histogram;
    for (std::size_t bin = 0; bin < curveHistogramBins; ++bin) {
        const auto count = static_cast<std::uint64_t>(bin * (curveHistogramBins - bin));
        histogram.luma[bin] = count;
        histogram.red[bin] = count / 2;
        histogram.green[bin] = bin;
        histogram.blue[bin] = 0;
    }
    // A clipped spike: cut off at the top rather than flattening the rest.
    histogram.luma.back() = 1'000'000'000;
    fixture.editor.setHistogram(histogram);
    for (const CurveChannel channel : curveChannels) {
        fixture.editor.setChannel(channel);
        const QImage painted = fixture.editor.grab().toImage();
        CHECK(painted.size() == fixture.editor.size() * painted.devicePixelRatio());
        if (const QByteArray path = qgetenv("ARRAW_CURVE_EDITOR_SNAPSHOT"); !path.isEmpty()) {
            painted.save(QString::fromUtf8(path) + QString::number(static_cast<int>(channel)) +
                         ".png");
        }
    }
}

TEST_CASE("The readout gives the selected point's input and output", "[app][curve][editor]") {
    Fixture fixture;
    fixture.editor.setCurves(withLuma({{0.0F, 0.0F}, {0.25F, 0.313F}, {1.0F, 1.0F}}));
    std::vector<QString> announced;
    QObject::connect(&fixture.editor, &CurveEditor::readoutChanged,
                     [&](const QString& text) { announced.push_back(text); });
    CHECK(fixture.editor.readout().isEmpty());

    key(fixture.editor, Qt::Key_PageDown);
    key(fixture.editor, Qt::Key_PageDown);
    CHECK(fixture.editor.readout() == QString::fromUtf8("In 0.25 → Out 0.31"));

    // A drag updates it as the point moves.
    press(fixture.editor, fixture.at(0.25F, 0.313F));
    move(fixture.editor, fixture.at(0.4F, 0.6F));
    release(fixture.editor, fixture.at(0.4F, 0.6F));
    CHECK(fixture.editor.readout() == QString::fromUtf8("In 0.40 → Out 0.60"));

    fixture.editor.setChannel(CurveChannel::Red);
    CHECK(fixture.editor.readout().isEmpty());
    // The first end, the point, the drag, then nothing.
    REQUIRE(announced.size() == 4);
    CHECK(announced.back().isEmpty());
}

TEST_CASE("The editor names itself for assistive technology", "[app][curve][editor]") {
    const CurveEditor editor;
    CHECK_FALSE(editor.accessibleName().isEmpty());
    CHECK_FALSE(editor.accessibleDescription().isEmpty());
}

TEST_CASE("Esc and Enter hand the focus back, and Esc is left to the window's shortcuts",
          "[app][curve][editor]") {
    Fixture fixture;
    fixture.editor.setCurves(withLuma({{0.0F, 0.0F}, {0.5F, 0.4F}, {1.0F, 1.0F}}));
    int released = 0;
    QObject::connect(&fixture.editor, &CurveEditor::focusReleased, [&] { ++released; });
    key(fixture.editor, Qt::Key_PageDown);

    const auto claimed = [&](int code) {
        QKeyEvent event(QEvent::ShortcutOverride, code, Qt::NoModifier);
        event.ignore();
        QCoreApplication::sendEvent(&fixture.editor, &event);
        return event.isAccepted();
    };
    // Esc cancels the white balance picker through a window shortcut, so the
    // editor must not take it first; Enter and the arrows are its own.
    CHECK_FALSE(claimed(Qt::Key_Escape));
    CHECK(claimed(Qt::Key_Return));
    CHECK(claimed(Qt::Key_Left));

    key(fixture.editor, Qt::Key_Escape);
    key(fixture.editor, Qt::Key_Return);
    key(fixture.editor, Qt::Key_Enter);
    CHECK(released == 3);
}

TEST_CASE("A double-click on the empty plot adds a point and keeps it", "[app][curve][editor]") {
    Fixture fixture;
    Recorder recorder(fixture.editor);
    const QPointF where = fixture.at(0.25F, 0.5F);
    // As Qt delivers it: press, release, double-click, release.
    press(fixture.editor, where);
    release(fixture.editor, where);
    mouse(fixture.editor, QEvent::MouseButtonDblClick, where);
    release(fixture.editor, where);

    CHECK(fixture.editor.curves().luma.points.size() == 3);
    CHECK(recorder.wellFormed());

    // A later double-click on that point, after another press, removes it as before.
    press(fixture.editor, where);
    release(fixture.editor, where);
    mouse(fixture.editor, QEvent::MouseButtonDblClick, where);
    CHECK(fixture.editor.curves().luma.points.size() == 2);
}
