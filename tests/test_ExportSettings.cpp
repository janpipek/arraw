#include "ExportSettings.h"
#include "support/TempDir.h"

#include <QSettings>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

using namespace arraw;
using app::ExportSettings;

TEST_CASE("Without resizing the request has no size", "[app][export]") {
    ExportSettings settings;
    settings.resize = false;
    settings.width = 800;
    settings.height = 600;

    const RenderRequest request = app::requestOf(settings);

    REQUIRE_FALSE(request.size.has_value());
    REQUIRE_FALSE(request.region.has_value());
}

TEST_CASE("Resizing fits the box, enlarging only when allowed", "[app][export]") {
    ExportSettings settings;
    settings.resize = true;
    settings.width = 800;
    settings.height = 600;

    settings.allowEnlarging = false;
    RenderRequest request = app::requestOf(settings);
    REQUIRE(request.size.has_value());
    const auto* box = std::get_if<RenderRequest::FitInside>(&*request.size);
    REQUIRE(box != nullptr);
    REQUIRE(box->width == 800);
    REQUIRE(box->height == 600);
    REQUIRE(request.upscale == Upscale::Never);

    settings.allowEnlarging = true;
    request = app::requestOf(settings);
    REQUIRE(request.upscale == Upscale::Allowed);
}

TEST_CASE("JPEG is always written at 8 bits", "[app][export]") {
    ExportSettings settings;
    settings.sixteenBit = true;
    settings.format = ImageFileFormat::Jpeg;
    REQUIRE(app::optionsOf(settings).bitDepth == 8);

    settings.format = ImageFileFormat::Png;
    REQUIRE(app::optionsOf(settings).bitDepth == 16);
    settings.format = ImageFileFormat::Tiff;
    REQUIRE(app::optionsOf(settings).bitDepth == 16);

    settings.sixteenBit = false;
    REQUIRE(app::optionsOf(settings).bitDepth == 8);
}

TEST_CASE("Format, colour, quality and sharpening pass through", "[app][export]") {
    ExportSettings settings;
    settings.format = ImageFileFormat::Tiff;
    settings.encoding = NamedEncoding::AdobeRgb;
    settings.quality = 42;
    settings.sharpening = 17;

    const ExportOptions options = app::optionsOf(settings);

    REQUIRE(options.format == ImageFileFormat::Tiff);
    REQUIRE(options.encoding == NamedEncoding::AdobeRgb);
    REQUIRE(options.quality == 42);
    REQUIRE(options.sharpening == 17);
}

TEST_CASE("A known image extension is replaced, any other is kept and extended",
          "[app][export][suffix]") {
    using std::filesystem::path;
    REQUIRE(app::withSuffix("a/shot.png", ImageFileFormat::Jpeg) == path("a/shot.jpg"));
    REQUIRE(app::withSuffix("shot.TIF", ImageFileFormat::Png) == path("shot.png"));
    REQUIRE(app::withSuffix("shot.tiff", ImageFileFormat::Jpeg) == path("shot.jpg"));
    REQUIRE(app::withSuffix("shot.JPEG", ImageFileFormat::Tiff) == path("shot.tif"));
    REQUIRE(app::withSuffix("shot.v2", ImageFileFormat::Png) == path("shot.v2.png"));
    REQUIRE(app::withSuffix("shot", ImageFileFormat::Tiff) == path("shot.tif"));
}

TEST_CASE("An extension that already belongs to the format is left alone",
          "[app][export][suffix]") {
    using std::filesystem::path;
    REQUIRE(app::withSuffix("shot.jpeg", ImageFileFormat::Jpeg) == path("shot.jpeg"));
    REQUIRE(app::withSuffix("shot.JPG", ImageFileFormat::Jpeg) == path("shot.JPG"));
    REQUIRE(app::withSuffix("shot.tiff", ImageFileFormat::Tiff) == path("shot.tiff"));
    REQUIRE(app::withSuffix("shot.PNG", ImageFileFormat::Png) == path("shot.PNG"));
}

TEST_CASE("The suggested name is the source's, in its folder", "[app][export][suffix]") {
    using std::filesystem::path;
    REQUIRE(app::suggestedPath("/photos/day/IMG_001.CR2", ImageFileFormat::Jpeg) ==
            path("/photos/day/IMG_001.jpg"));
    REQUIRE(app::suggestedPath("IMG_001.dng", ImageFileFormat::Tiff) == path("IMG_001.tif"));
}

TEST_CASE("The source is recognised however its path is spelled", "[app][export]") {
    const test::TempDir dir;
    const auto file = dir.file("a.jpg");
    std::ofstream(file).put('x');

    REQUIRE(isSameFile(file, dir.path() / "." / "a.jpg"));
    REQUIRE(isSameFile(file, dir.path() / "sub" / ".." / "a.jpg"));
    REQUIRE_FALSE(isSameFile(file, dir.file("b.jpg")));
    // Not yet existing paths are compared as spelled, normalised.
    REQUIRE(isSameFile(dir.file("new.png"), dir.path() / "x" / ".." / "new.png"));
}

TEST_CASE("Settings are remembered, the custom size included", "[app][export][settings]") {
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    ExportSettings settings;
    settings.format = ImageFileFormat::Tiff;
    settings.encoding = NamedEncoding::DisplayP3;
    settings.sixteenBit = true;
    settings.resize = true;
    settings.width = 123;
    settings.height = 456;
    settings.allowEnlarging = true;
    settings.quality = 55;
    settings.sharpening = 33;
    {
        QSettings store(file, QSettings::IniFormat);
        app::saveSettings(settings, store);
    }

    QSettings store(file, QSettings::IniFormat);
    const ExportSettings restored = app::restoreSettings(store);

    REQUIRE(restored.format == ImageFileFormat::Tiff);
    REQUIRE(restored.encoding == NamedEncoding::DisplayP3);
    REQUIRE(restored.sixteenBit);
    REQUIRE(restored.resize);
    REQUIRE(restored.allowEnlarging);
    REQUIRE(restored.quality == 55);
    REQUIRE(restored.sharpening == 33);
    REQUIRE(restored.width == 123);
    REQUIRE(restored.height == 456);
}

TEST_CASE("The original size is stored as no resizing, and is the default",
          "[app][export][settings]") {
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    QSettings store(file, QSettings::IniFormat);
    REQUIRE_FALSE(app::restoreSettings(store).resize);

    app::saveSettings(ExportSettings{.resize = true, .width = 2048, .height = 2048}, store);
    ExportSettings restored = app::restoreSettings(store);
    REQUIRE(restored.resize);
    REQUIRE(restored.width == 2048);
    REQUIRE(restored.height == 2048);

    app::saveSettings(ExportSettings{}, store);
    restored = app::restoreSettings(store);
    REQUIRE_FALSE(restored.resize);
}

TEST_CASE("A bad stored size means the original size", "[app][export][settings]") {
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    QSettings store(file, QSettings::IniFormat);
    store.setValue("export/resize", true); // As older versions stored it: no box.
    REQUIRE_FALSE(app::restoreSettings(store).resize);
    store.setValue("export/width", 0);
    store.setValue("export/height", 500);
    REQUIRE_FALSE(app::restoreSettings(store).resize);
    store.setValue("export/width", "wide");
    REQUIRE_FALSE(app::restoreSettings(store).resize);
    store.setValue("export/width", 100000);
    REQUIRE_FALSE(app::restoreSettings(store).resize);
}

TEST_CASE("Metadata choices default, map to a selection, and are remembered",
          "[app][export][settings][metadata]") {
    const ExportSettings defaults;
    REQUIRE(app::selectionOf(defaults) == MetadataSelection{});
    REQUIRE(defaults.captureInfo);
    REQUIRE_FALSE(defaults.location);
    REQUIRE(defaults.descriptive);

    ExportSettings settings;
    settings.captureInfo = false;
    settings.location = true;
    settings.descriptive = false;
    REQUIRE(app::selectionOf(settings) ==
            MetadataSelection{.capture = false, .location = true, .descriptive = false});

    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    {
        QSettings store(file, QSettings::IniFormat);
        app::saveSettings(settings, store);
    }
    QSettings store(file, QSettings::IniFormat);
    const ExportSettings restored = app::restoreSettings(store);
    REQUIRE_FALSE(restored.captureInfo);
    REQUIRE(restored.location);
    REQUIRE_FALSE(restored.descriptive);
}

TEST_CASE("Nothing stored, or nonsense, gives the defaults", "[app][export][settings]") {
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    QSettings store(file, QSettings::IniFormat);
    const ExportSettings defaults;

    ExportSettings restored = app::restoreSettings(store);
    REQUIRE(restored.format == defaults.format);
    REQUIRE(restored.quality == defaults.quality);

    store.setValue("export/format", 99);
    store.setValue("export/encoding", 0); // The working encoding is no output.
    store.setValue("export/quality", 1000);
    store.setValue("export/sharpening", "much");
    restored = app::restoreSettings(store);
    REQUIRE(restored.format == defaults.format);
    REQUIRE(restored.encoding == defaults.encoding);
    REQUIRE(restored.quality == defaults.quality);
    REQUIRE(restored.sharpening == defaults.sharpening);
}

namespace {

using Paths = std::vector<std::filesystem::path>;

}

TEST_CASE("Targets are the stems in the folder with the format's suffix", "[app][export]") {
    const Paths sources{"/a/one.nef", "/b/two.cr2"};
    const Paths targets = app::targetPaths(sources, "/out", ImageFileFormat::Tiff);
    CHECK(targets == Paths{"/out/one.tif", "/out/two.tif"});
    CHECK(app::targetPaths({}, "/out", ImageFileFormat::Jpeg).empty());
}

TEST_CASE("Targets of equal stems are numbered from 2, in the order given", "[app][export]") {
    const Paths sources{"/a/x.nef", "/b/x.nef", "/c/x.cr2"};
    CHECK(app::targetPaths(sources, "/out", ImageFileFormat::Jpeg) ==
          Paths{"/out/x.jpg", "/out/x_2.jpg", "/out/x_3.jpg"});
    // Deterministic.
    CHECK(app::targetPaths(sources, "/out", ImageFileFormat::Jpeg) ==
          app::targetPaths(sources, "/out", ImageFileFormat::Jpeg));
}

TEST_CASE("Targets collide whatever the letter case of the stem", "[app][export]") {
    const Paths sources{"/a/IMG_1.NEF", "/b/img_1.nef"};
    CHECK(app::targetPaths(sources, "/out", ImageFileFormat::Png) ==
          Paths{"/out/IMG_1.png", "/out/img_1_2.png"});
}

TEST_CASE("A numbered target does not take a name another stem needs", "[app][export]") {
    const Paths sources{"/a/x.nef", "/b/x.nef", "/c/x_2.nef"};
    const Paths targets = app::targetPaths(sources, "/out", ImageFileFormat::Jpeg);
    CHECK(targets == Paths{"/out/x.jpg", "/out/x_2.jpg", "/out/x_2_2.jpg"});
}

TEST_CASE("A target is never one of the sources", "[app][export]") {
    // A JPEG shot exported as JPEG into its own folder.
    const Paths sources{"/out/a.jpg", "/out/B.JPG", "/x/a.nef"};
    const Paths targets = app::targetPaths(sources, "/out", ImageFileFormat::Jpeg);
    CHECK(targets == Paths{"/out/a_2.jpg", "/out/B_2.jpg", "/out/a_3.jpg"});
}
