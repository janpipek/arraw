#include "core/ImageBuffer.h"
#include "develop/GlobalAdjustment.h"
#include "render/HeadlessRenderContext.h"
#include "render/OffscreenRender.h"
#include "render/RendererCore.h"
#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>

namespace {
ImageBuffer tinyScene() {
    ImageBuffer b;
    b.width = 8;
    b.height = 8;
    b.data.assign(size_t(8 * 8 * 3), 0.5f);
    return b;
}
} // namespace

TEST_CASE("HeadlessRenderContext renders without a widget", "[golden]") {
    testApp(); // platform plugin must exist before GL context creation

    QString error;
    auto ctx = HeadlessRenderContext::create(&error);
    if (!ctx)
        SKIP("no headless GPU backend: " + error.toStdString());

    RendererCore core;
    core.initialize(ctx->rhi());
    const QImage img = offscreen::renderToImage(core, tinyScene(), GlobalAdjustment{}, 8, 8);
    core.release();

    REQUIRE(!img.isNull());
    REQUIRE(img.width() == 8);
    REQUIRE(img.height() == 8);
    REQUIRE(img.format() == QImage::Format_RGBX32FPx4);
}

TEST_CASE("ARRAW_RHI_BACKEND rejects unknown values with an error") {
    testApp();

    qputenv("ARRAW_RHI_BACKEND", "bogus");
    QString error;
    auto ctx = HeadlessRenderContext::create(&error);
    qunsetenv("ARRAW_RHI_BACKEND");

    REQUIRE_FALSE(ctx);
    REQUIRE(error.contains("bogus"));
}

TEST_CASE("ARRAW_RHI_BACKEND=null creates the Null backend") {
    testApp();

    qputenv("ARRAW_RHI_BACKEND", "null");
    QString error;
    auto ctx = HeadlessRenderContext::create(&error);
    qunsetenv("ARRAW_RHI_BACKEND");

    REQUIRE(ctx);
    REQUIRE(ctx->rhi()->backend() == QRhi::Null);
}
