#include "DisplayImage.h"
#include "support/TestImages.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace arraw;

/// The window shows what an export of the same size would write, fitted to
/// the room it has (ADR 007).

TEST_CASE("A photograph is fitted inside the viewport in device pixels", "[app]") {
    const auto source = test::rainbow({400, 200}, PixelFormat::RgbaF32, workingEncoding);

    const QImage image = app::renderForViewport(source, {}, {100, 100}, 2.0);

    REQUIRE(image.size() == QSize(100, 50));
    REQUIRE(image.devicePixelRatio() == 2.0);
    // Drawn, it covers 50 by 25 logical pixels.
    REQUIRE(image.deviceIndependentSize() == QSizeF(50, 25));
}

TEST_CASE("A photograph smaller than the viewport is not enlarged", "[app]") {
    const auto source = test::rainbow({40, 20}, PixelFormat::RgbaF32, workingEncoding);

    const QImage image = app::renderForViewport(source, {}, {300, 300}, 1.0);

    REQUIRE(image.size() == QSize(40, 20));
}

TEST_CASE("An empty viewport is refused", "[app]") {
    const auto source = test::rainbow({4, 4}, PixelFormat::RgbaF32, workingEncoding);

    REQUIRE_THROWS_AS(app::renderForViewport(source, {}, {0, 10}, 1.0), std::invalid_argument);
}
