// CollapsiblePane: the single owner of the "dock and reveal strip are always
// opposite" invariant (ADR 0012). Tested through plain QWidgets via isHidden()
// — the explicit-hidden flag, which is meaningful even when no window is shown.

#include "CollapsiblePane.h"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QWidget>

namespace {

// QWidget construction requires a QApplication. Mirrors the lazy-static idiom in
// test_GoldenImages.cpp; each Catch test is its own ctest process, so this does
// not collide with other files' app instances. In a combined single-process run
// a QCoreApplication-only test (test_FilmStripModel) may already hold qApp, in
// which case widgets are impossible — skip, as the golden tests do.
void ensureApp() {
    static int argc = 1;
    static char arg0[] = "arraw_tests";
    static char* argv[] = {arg0, nullptr};
    if (!qApp)
        new QApplication(argc, argv);
    if (!qobject_cast<QApplication*>(qApp))
        SKIP("needs a QApplication; run isolated (ctest does this per-test)");
}

} // namespace

TEST_CASE("a fresh pane is expanded", "[collapsible]") {
    ensureApp();
    // Children of a host so isHidden() tracks explicit hide() alone, not the
    // "never shown" state a top-level widget reports.
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);

    CollapsiblePane pane(expanded, strip);

    CHECK_FALSE(pane.isCollapsed());
    CHECK_FALSE(expanded->isHidden());
    CHECK(strip->isHidden());
}

TEST_CASE("toggle flips between collapsed and expanded", "[collapsible]") {
    ensureApp();
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);
    CollapsiblePane pane(expanded, strip);

    pane.toggle();
    CHECK(pane.isCollapsed());
    CHECK(expanded->isHidden());

    pane.toggle();
    CHECK_FALSE(pane.isCollapsed());
    CHECK_FALSE(expanded->isHidden());
}

TEST_CASE("collapse hides the content and shows the strip", "[collapsible]") {
    ensureApp();
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);
    CollapsiblePane pane(expanded, strip);

    pane.collapse();

    CHECK(pane.isCollapsed());
    CHECK(expanded->isHidden());
    CHECK_FALSE(strip->isHidden());
}

TEST_CASE("expand restores the content and hides the strip", "[collapsible]") {
    ensureApp();
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);
    CollapsiblePane pane(expanded, strip);
    pane.collapse();

    pane.expand();

    CHECK_FALSE(pane.isCollapsed());
    CHECK_FALSE(expanded->isHidden());
    CHECK(strip->isHidden());
}

TEST_CASE("hide conceals both widgets; show restores the expanded state", "[collapsible]") {
    ensureApp();
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);
    CollapsiblePane pane(expanded, strip); // expanded

    pane.hide();
    CHECK(pane.isHidden());
    CHECK(expanded->isHidden());
    CHECK(strip->isHidden());

    pane.show();
    CHECK_FALSE(pane.isHidden());
    CHECK_FALSE(expanded->isHidden()); // back to expanded
    CHECK(strip->isHidden());
}

TEST_CASE("a hide/show cycle preserves the collapsed state", "[collapsible]") {
    ensureApp();
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);
    CollapsiblePane pane(expanded, strip);
    pane.collapse(); // strip showing

    pane.hide();
    CHECK(expanded->isHidden());
    CHECK(strip->isHidden());

    pane.show();
    CHECK(pane.isCollapsed());
    CHECK(expanded->isHidden());
    CHECK_FALSE(strip->isHidden()); // returns collapsed, not expanded
}

TEST_CASE("toggling while hidden updates state without revealing a widget", "[collapsible]") {
    ensureApp();
    QWidget host;
    auto* expanded = new QWidget(&host);
    auto* strip = new QWidget(&host);
    CollapsiblePane pane(expanded, strip); // expanded
    pane.hide();

    pane.toggle(); // flips remembered state to collapsed, but stays hidden

    CHECK(pane.isCollapsed());
    CHECK(expanded->isHidden());
    CHECK(strip->isHidden()); // nothing pops back into view during lights-out

    pane.show();
    CHECK(pane.isCollapsed());
    CHECK_FALSE(strip->isHidden());
}
