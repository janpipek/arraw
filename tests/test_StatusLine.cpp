#include "ui/RenderProgressPie.h"
#include "ui/StatusLine.h"

#include <QLabel>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

using namespace arraw::app;

/// The status bar's own line: pie, message, device, zoom (ADR 042).

TEST_CASE("The pie is left of the message and stays visible while a message shows",
          "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.resize(600, 30);
    line.show();
    REQUIRE(QTest::qWaitForWindowExposed(&line));
    CHECK(line.pie()->isVisible());
    CHECK(line.pie()->geometry().right() < line.messageLabel()->geometry().left());
    line.showMessage("Exported a.dng");
    CHECK(line.pie()->isVisible());
    CHECK(line.message() == "Exported a.dng");
    CHECK(line.messageLabel()->text() == "Exported a.dng");
    CHECK(line.pie()->geometry().right() < line.messageLabel()->geometry().left());
}

TEST_CASE("A timed message clears itself; clearMessage clears at once", "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.show();
    line.showMessage("Soon gone", 50);
    CHECK(line.message() == "Soon gone");
    CHECK(QTest::qWaitFor([&] { return line.message().isEmpty(); }, 2000));
    CHECK(line.messageLabel()->text().isEmpty());

    line.showMessage("Kept");
    QTest::qWait(150);
    CHECK(line.message() == "Kept");
    line.clearMessage();
    CHECK(line.message().isEmpty());
    CHECK(line.messageLabel()->text().isEmpty());
}

TEST_CASE("A new message replaces the old and restarts the timer", "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.show();
    line.showMessage("First", 1000);
    QTest::qWait(600);
    line.showMessage("Second", 1000);
    QTest::qWait(600);
    // The first timeout has passed, but it was replaced.
    CHECK(line.message() == "Second");
    CHECK(QTest::qWaitFor([&] { return line.message().isEmpty(); }, 2000));

    // A message without a timeout cancels an earlier one's timer.
    line.showMessage("Timed", 100);
    line.showMessage("Untimed");
    QTest::qWait(300);
    CHECK(line.message() == "Untimed");
}

TEST_CASE("The message tooltip goes with the message", "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.resize(600, 30);
    line.show();
    line.showMessage("Exported", 0, "lens missing");
    CHECK(line.messageLabel()->toolTip() == "lens missing");
    line.showMessage("Other");
    CHECK(line.messageLabel()->toolTip().isEmpty());
    line.showMessage("Exported", 0, "lens missing");
    line.clearMessage();
    CHECK(line.messageLabel()->toolTip().isEmpty());
}

TEST_CASE("An elided message has its full text as tooltip", "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.resize(300, 30);
    line.show();
    const QString longText(500, QChar('x'));
    line.showMessage(longText);
    CHECK(line.messageLabel()->toolTip() == longText);
}

TEST_CASE("A long message is elided, not widening the line", "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.resize(300, 30);
    line.show();
    const int hint = line.sizeHint().width();
    const int minimumHint = line.minimumSizeHint().width();
    const QString longText(500, QChar('x'));
    line.showMessage(longText);
    auto* label = line.messageLabel();
    CHECK(line.message() == longText);
    CHECK(label->text().size() < longText.size());
    CHECK(label->text().endsWith(QChar(0x2026)));
    CHECK(line.sizeHint().width() <= hint);
    CHECK(line.minimumSizeHint().width() <= minimumHint);
    CHECK(label->fontMetrics().horizontalAdvance(label->text()) <= label->width());
}

TEST_CASE("The message is elided again when its label changes width", "[app][statusline]") {
    StatusLine line(new QToolButton);
    line.resize(500, 30);
    line.show();
    const QString longText(500, QChar('x'));
    line.showMessage(longText);
    auto* label = line.messageLabel();
    const int before = label->width();
    line.deviceLabel()->setText(QString(40, QChar('D')));
    QTest::qWait(50);
    CHECK(label->width() < before);
    CHECK(label->text().endsWith(QChar(0x2026)));
    CHECK(label->fontMetrics().horizontalAdvance(label->text()) <= label->width());
}
