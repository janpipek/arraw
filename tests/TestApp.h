#pragma once
#include <QApplication>

// One shared QApplication for all widget tests — only one may exist per process,
// and Catch2 runs every test case in the same process. It is a function-local
// static, so it is constructed on first use and destroyed last; any widget
// static that wants it must call testApp() before constructing, so the widget
// (constructed later) is torn down first (reverse construction order).
inline QApplication& testApp() {
    static int argc = 1;
    static char arg0[] = "arraw_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
    return app;
}
