#include "ChromeHider.h"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QWidget>

namespace {

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

TEST_CASE("hide() hides every chrome widget and reports hidden", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    auto* b = new QWidget(&host);
    ChromeHider hider({a, b});

    hider.hide();

    CHECK(hider.hidden());
    CHECK(a->isHidden());
    CHECK(b->isHidden());
}

TEST_CASE("restore() brings back widgets that were visible", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    auto* b = new QWidget(&host);
    ChromeHider hider({a, b});
    hider.hide();

    hider.restore();

    CHECK_FALSE(hider.hidden());
    CHECK_FALSE(a->isHidden());
    CHECK_FALSE(b->isHidden());
}

TEST_CASE("restore() keeps an already-hidden widget hidden", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* shown = new QWidget(&host);
    auto* alreadyHidden = new QWidget(&host);
    alreadyHidden->hide();
    ChromeHider hider({shown, alreadyHidden});

    hider.hide();
    hider.restore();

    CHECK_FALSE(shown->isHidden());
    CHECK(alreadyHidden->isHidden());
}

TEST_CASE("a second hide() does not clobber the snapshot", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* shown = new QWidget(&host);
    auto* alreadyHidden = new QWidget(&host);
    alreadyHidden->hide();
    ChromeHider hider({shown, alreadyHidden});

    hider.hide();
    hider.hide();
    hider.restore();

    CHECK_FALSE(shown->isHidden());
    CHECK(alreadyHidden->isHidden());
}

TEST_CASE("restore() before any hide() is a no-op", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    ChromeHider hider({a});

    hider.restore();

    CHECK_FALSE(hider.hidden());
    CHECK_FALSE(a->isHidden());
}

TEST_CASE("nullptr widgets are ignored", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    ChromeHider hider({a, nullptr});

    hider.hide();
    hider.restore();

    CHECK_FALSE(a->isHidden());
}
