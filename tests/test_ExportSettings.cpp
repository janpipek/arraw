#include "support/TempDir.h"

#include <ExportSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <variant>

using namespace arraw;

TEST_CASE("Without resizing the request has no size", "[export]") {
    ExportSettings settings;
    settings.resize = false;
    settings.width = 800;
    settings.height = 600;

    const RenderRequest request = requestOf(settings);

    REQUIRE_FALSE(request.size.has_value());
    REQUIRE_FALSE(request.region.has_value());
}

TEST_CASE("Resizing fits the box, enlarging only when allowed", "[export]") {
    ExportSettings settings;
    settings.resize = true;
    settings.width = 800;
    settings.height = 600;

    settings.allowEnlarging = false;
    RenderRequest request = requestOf(settings);
    REQUIRE(request.size.has_value());
    const auto* box = std::get_if<RenderRequest::FitInside>(&*request.size);
    REQUIRE(box != nullptr);
    REQUIRE(box->width == 800);
    REQUIRE(box->height == 600);
    REQUIRE(request.upscale == Upscale::Never);

    settings.allowEnlarging = true;
    request = requestOf(settings);
    REQUIRE(request.upscale == Upscale::Allowed);
}

TEST_CASE("JPEG is always written at 8 bits", "[export]") {
    ExportSettings settings;
    settings.sixteenBit = true;
    settings.format = ImageFileFormat::Jpeg;
    REQUIRE(optionsOf(settings).bitDepth == 8);

    settings.format = ImageFileFormat::Png;
    REQUIRE(optionsOf(settings).bitDepth == 16);
    settings.format = ImageFileFormat::Tiff;
    REQUIRE(optionsOf(settings).bitDepth == 16);

    settings.sixteenBit = false;
    REQUIRE(optionsOf(settings).bitDepth == 8);
}

TEST_CASE("Format, colour, quality and sharpening pass through", "[export]") {
    ExportSettings settings;
    settings.format = ImageFileFormat::Tiff;
    settings.encoding = NamedEncoding::AdobeRgb;
    settings.quality = 42;
    settings.sharpening = 17;

    const ExportOptions options = optionsOf(settings);

    REQUIRE(options.format == ImageFileFormat::Tiff);
    REQUIRE(options.encoding == NamedEncoding::AdobeRgb);
    REQUIRE(options.quality == 42);
    REQUIRE(options.sharpening == 17);
}

TEST_CASE("Metadata choices default, map to a selection, and are remembered",
          "[export][metadata]") {
    const ExportSettings defaults;
    REQUIRE(selectionOf(defaults) == MetadataSelection{});
    REQUIRE(defaults.captureInfo);
    REQUIRE_FALSE(defaults.location);
    REQUIRE(defaults.descriptive);

    ExportSettings settings;
    settings.captureInfo = false;
    settings.location = true;
    settings.descriptive = false;
    REQUIRE(selectionOf(settings) ==
            MetadataSelection{.capture = false, .location = true, .descriptive = false});
}

TEST_CASE("The source is recognised however its path is spelled", "[export]") {
    const test::TempDir dir;
    const auto file = dir.file("a.jpg");
    std::ofstream(file).put('x');

    REQUIRE(isSameFile(file, dir.path() / "." / "a.jpg"));
    REQUIRE(isSameFile(file, dir.path() / "sub" / ".." / "a.jpg"));
    REQUIRE_FALSE(isSameFile(file, dir.file("b.jpg")));
    // Not yet existing paths are compared as spelled, normalised.
    REQUIRE(isSameFile(dir.file("new.png"), dir.path() / "x" / ".." / "new.png"));
}

TEST_CASE("Canonical settings drop what the format and size do not use", "[export]") {
    const ExportSettings loose{.format = ImageFileFormat::Jpeg,
                               .sixteenBit = true,
                               .resize = false,
                               .width = 800,
                               .height = 600,
                               .allowEnlarging = true};
    const ExportSettings tight = canonical(loose);
    CHECK_FALSE(tight.sixteenBit);
    CHECK(tight.width == 0);
    CHECK(tight.height == 0);
    CHECK_FALSE(tight.allowEnlarging);
    CHECK(tight == canonical(ExportSettings{}));
    CHECK_FALSE(loose == tight);

    // What the format and size do use stays.
    const ExportSettings kept{.format = ImageFileFormat::Tiff,
                              .sixteenBit = true,
                              .resize = true,
                              .width = 800,
                              .height = 600,
                              .allowEnlarging = true};
    CHECK(canonical(kept) == kept);
}

TEST_CASE("Validate refuses quality, sharpening and boxes out of range", "[export]") {
    CHECK_NOTHROW(validate(ExportSettings{}));
    CHECK_NOTHROW(validate(ExportSettings{.quality = 0, .sharpening = 100}));
    CHECK_NOTHROW(validate(ExportSettings{.resize = true, .width = 1, .height = 99999}));

    CHECK_THROWS_AS(validate(ExportSettings{.quality = -1}), std::invalid_argument);
    CHECK_THROWS_AS(validate(ExportSettings{.quality = 101}), std::invalid_argument);
    CHECK_THROWS_AS(validate(ExportSettings{.sharpening = -1}), std::invalid_argument);
    CHECK_THROWS_AS(validate(ExportSettings{.sharpening = 101}), std::invalid_argument);
    CHECK_THROWS_AS(validate(ExportSettings{.resize = true, .width = 0, .height = 10}),
                    std::invalid_argument);
    CHECK_THROWS_AS(validate(ExportSettings{.resize = true, .width = 10, .height = 100000}),
                    std::invalid_argument);
    CHECK_THROWS_AS(validate(ExportSettings{.encoding = NamedEncoding::LinearRec2020}),
                    std::invalid_argument);
    // A box that is not used is not checked.
    CHECK_NOTHROW(validate(ExportSettings{.resize = false, .width = 0, .height = 0}));
}

TEST_CASE("Every format and encoding has a token that reads back", "[export]") {
    for (const auto format : {ImageFileFormat::Jpeg, ImageFileFormat::Png, ImageFileFormat::Tiff}) {
        CHECK(parseImageFileFormat(tokenOf(format)) == format);
    }
    for (const auto encoding :
         {NamedEncoding::Srgb, NamedEncoding::DisplayP3, NamedEncoding::AdobeRgb}) {
        CHECK(parseOutputEncoding(tokenOf(encoding)) == encoding);
    }
    CHECK(tokenOf(ImageFileFormat::Jpeg) == "jpeg");
    CHECK(tokenOf(NamedEncoding::DisplayP3) == "display-p3");
    CHECK_THROWS_AS(tokenOf(NamedEncoding::LinearRec2020), std::invalid_argument);
}

TEST_CASE("Format aliases read, and the canonical token is written", "[export]") {
    CHECK(parseImageFileFormat("jpg") == ImageFileFormat::Jpeg);
    CHECK(parseImageFileFormat("tif") == ImageFileFormat::Tiff);
    CHECK(tokenOf(*parseImageFileFormat("jpg")) == "jpeg");
    CHECK(tokenOf(*parseImageFileFormat("tif")) == "tiff");
    // Lower case only; nothing fuzzy.
    CHECK_FALSE(parseImageFileFormat("JPEG"));
    CHECK_FALSE(parseImageFileFormat("avif"));
    CHECK_FALSE(parseImageFileFormat(""));
    CHECK_FALSE(parseOutputEncoding("sRGB"));
    CHECK_FALSE(parseOutputEncoding("rec2020"));
}
