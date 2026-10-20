#include "AppSettings.h"
#include "support/TempDir.h"

#include <ExportSettings.h>

#include <QSettings>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

using namespace arraw;

TEST_CASE("GUI startup restores the file and falls back to the remembered folder",
          "[app][settings]") {
    const test::TempDir directory;
    QSettings store(QString::fromStdU16String(directory.file("settings.ini").u16string()),
                    QSettings::IniFormat);
    REQUIRE_FALSE(app::restoreOpenPath(store));
    const auto folder = directory.path() / "photos";
    std::filesystem::create_directory(folder);
    const auto file = folder / std::filesystem::path(std::u16string(u"caf\u00e9.png"));
    std::ofstream(file) << "fixture";
    store.setValue("lastFolder", QString::fromStdU16String(folder.u16string()));
    store.setValue("lastFile", QString::fromStdU16String(file.u16string()));
    store.sync();
    QSettings restored(store.fileName(), QSettings::IniFormat);
    REQUIRE(app::restoreOpenPath(restored) == file);

    std::filesystem::remove(file);
    REQUIRE(app::restoreOpenPath(restored) == folder);
    std::filesystem::remove(folder);
    REQUIRE_FALSE(app::restoreOpenPath(restored));
}

TEST_CASE("GUI startup does not reopen a file from a previously visited folder",
          "[app][settings]") {
    const test::TempDir directory;
    QSettings store(QString::fromStdU16String(directory.file("settings.ini").u16string()),
                    QSettings::IniFormat);
    const auto file = directory.file("old.png");
    std::ofstream(file) << "fixture";
    const auto folder = directory.path() / "new";
    std::filesystem::create_directory(folder);
    store.setValue("lastFile", QString::fromStdU16String(file.u16string()));
    store.setValue("lastFolder", QString::fromStdU16String(folder.u16string()));
    REQUIRE(app::restoreOpenPath(store) == folder);
}

namespace {

const GpuAdapterInfo integrated{.name = "Integrated GPU",
                                .kind = GpuDeviceKind::Integrated,
                                .vendorId = 0x8086,
                                .deviceId = 0x1234};
const GpuAdapterInfo discrete{.name = "Discrete GPU",
                              .kind = GpuDeviceKind::Discrete,
                              .vendorId = 0x10de,
                              .deviceId = 0x5678};

} // namespace

TEST_CASE("Desktop preferences persist independently of other settings", "[app][settings]") {
    const test::TempDir directory;
    const QString path = QString::fromStdU16String(directory.file("settings.ini").u16string());
    {
        QSettings store(path, QSettings::IniFormat);
        const auto defaults = app::restoreAppSettings(store);
        REQUIRE_FALSE(defaults.cpuOnly);
        REQUIRE_FALSE(defaults.gpu);
        store.setValue("lastFolder", "photos");
        store.setValue("export/quality", 75);
        app::saveAppSettings({.cpuOnly = false, .gpu = discrete}, store);
        store.sync();
        REQUIRE(store.status() == QSettings::NoError);
    }
    {
        QSettings store(path, QSettings::IniFormat);
        const auto settings = app::restoreAppSettings(store);
        REQUIRE_FALSE(settings.cpuOnly);
        REQUIRE(settings.gpu);
        REQUIRE(settings.gpu->name == discrete.name);
        REQUIRE(settings.gpu->vendorId == discrete.vendorId);
        REQUIRE(settings.gpu->deviceId == discrete.deviceId);
        REQUIRE(store.value("lastFolder").toString() == "photos");
        REQUIRE(store.value("export/quality").toInt() == 75);
        app::saveAppSettings({.cpuOnly = true, .gpu = discrete}, store);
        REQUIRE(app::restoreAppSettings(store).cpuOnly);
        REQUIRE_FALSE(app::restoreAppSettings(store).gpu);
        REQUIRE_FALSE(store.contains("processing/gpuName"));
        app::saveAppSettings({}, store);
        REQUIRE_FALSE(app::restoreAppSettings(store).cpuOnly);
        REQUIRE_FALSE(app::restoreAppSettings(store).gpu);
    }
}

TEST_CASE("Invalid desktop preferences use automatic processing", "[app][settings]") {
    const test::TempDir directory;
    QSettings store(QString::fromStdU16String(directory.file("settings.ini").u16string()),
                    QSettings::IniFormat);
    app::saveAppSettings({.cpuOnly = false, .gpu = discrete}, store);
    SECTION("Unknown processing mode") {
        store.setValue("processing/device", "invalid");
    }
    SECTION("Empty GPU name") {
        store.setValue("processing/gpuName", "");
    }
    SECTION("Missing vendor") {
        store.remove("processing/vendorId");
    }
    SECTION("Malformed device") {
        store.setValue("processing/deviceId", "invalid");
    }
    SECTION("Negative device") {
        store.setValue("processing/deviceId", "-1");
    }
    SECTION("Other platform backend") {
        store.setValue("processing/backend", "other");
    }
    const auto restored = app::restoreAppSettings(store);
    REQUIRE_FALSE(restored.cpuOnly);
    REQUIRE_FALSE(restored.gpu);
}

TEST_CASE("GPU identity survives enumeration reordering and rejects substitutions",
          "[app][settings]") {
    std::array adapters{integrated, discrete};
    REQUIRE(app::findPreferredGpu(discrete, adapters) == 1);
    std::ranges::reverse(adapters);
    REQUIRE(app::findPreferredGpu(discrete, adapters) == 0);
    adapters[0].deviceId += 1;
    REQUIRE_FALSE(app::findPreferredGpu(discrete, adapters));
    adapters[0] = discrete;
    adapters[0].kind = GpuDeviceKind::Software;
    REQUIRE_FALSE(app::findPreferredGpu(discrete, adapters));
    REQUIRE_FALSE(app::findPreferredGpu(discrete, {}));
}

TEST_CASE("CPU preferences never create a graphics context", "[app][settings]") {
    std::string reason;
    REQUIRE_FALSE(app::createAppGpuContext({.cpuOnly = true, .gpu = discrete}, reason));
    REQUIRE(reason == "CPU selected in Settings");
}

TEST_CASE("GPU identifiers retain all their bits in settings", "[app][settings]") {
    const test::TempDir directory;
    QSettings store(QString::fromStdU16String(directory.file("settings.ini").u16string()),
                    QSettings::IniFormat);
    auto gpu = discrete;
    gpu.vendorId = std::numeric_limits<std::uint64_t>::max();
    gpu.deviceId = std::numeric_limits<std::uint64_t>::max() - 1;
    app::saveAppSettings({.cpuOnly = false, .gpu = gpu}, store);
    const auto restored = app::restoreAppSettings(store);
    REQUIRE(restored.gpu);
    REQUIRE(restored.gpu->vendorId == gpu.vendorId);
    REQUIRE(restored.gpu->deviceId == gpu.deviceId);
}

TEST_CASE("Including subfolders in the film strip is remembered, and off by default",
          "[app][settings]") {
    const test::TempDir directory;
    const QString path = QString::fromStdU16String(directory.file("settings.ini").u16string());
    {
        QSettings store(path, QSettings::IniFormat);
        REQUIRE_FALSE(app::restoreIncludeSubfolders(store));
        app::saveIncludeSubfolders(true, store);
        store.sync();
    }
    {
        QSettings store(path, QSettings::IniFormat);
        REQUIRE(app::restoreIncludeSubfolders(store));
        // Desktop preferences do not disturb it.
        app::saveAppSettings({.cpuOnly = true}, store);
        REQUIRE(app::restoreIncludeSubfolders(store));
        app::saveIncludeSubfolders(false, store);
        REQUIRE_FALSE(app::restoreIncludeSubfolders(store));
    }
}

TEST_CASE("The cap on watched folders defaults, round-trips and stays within bounds",
          "[app][settings]") {
    const test::TempDir directory;
    QSettings store(QString::fromStdU16String(directory.file("settings.ini").u16string()),
                    QSettings::IniFormat);
    REQUIRE(app::restoreMaxWatchedFolders(store) == 1000);

    app::saveMaxWatchedFolders(5000, store);
    REQUIRE(app::restoreMaxWatchedFolders(store) == 5000);
    REQUIRE(store.value("filmStrip/maxWatchedFolders").toInt() == 5000);

    // A hand-edited value outside the bounds is brought into them; a word is no number.
    store.setValue("filmStrip/maxWatchedFolders", 0);
    REQUIRE(app::restoreMaxWatchedFolders(store) == app::minWatchedFolders);
    store.setValue("filmStrip/maxWatchedFolders", -7);
    REQUIRE(app::restoreMaxWatchedFolders(store) == app::minWatchedFolders);
    store.setValue("filmStrip/maxWatchedFolders", 99999999999LL);
    REQUIRE(app::restoreMaxWatchedFolders(store) == app::maxWatchedFoldersLimit);
    store.setValue("filmStrip/maxWatchedFolders", "many");
    REQUIRE(app::restoreMaxWatchedFolders(store) == 1000);
    store.setValue("filmStrip/maxWatchedFolders", "250");
    REQUIRE(app::restoreMaxWatchedFolders(store) == 250);
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
        app::saveExportSettings(settings, store);
    }

    QSettings store(file, QSettings::IniFormat);
    const ExportSettings restored = app::restoreExportSettings(store);

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
    REQUIRE_FALSE(app::restoreExportSettings(store).resize);

    app::saveExportSettings(ExportSettings{.resize = true, .width = 2048, .height = 2048}, store);
    ExportSettings restored = app::restoreExportSettings(store);
    REQUIRE(restored.resize);
    REQUIRE(restored.width == 2048);
    REQUIRE(restored.height == 2048);

    app::saveExportSettings(ExportSettings{}, store);
    restored = app::restoreExportSettings(store);
    REQUIRE_FALSE(restored.resize);
}

TEST_CASE("A bad stored size means the original size", "[app][export][settings]") {
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    QSettings store(file, QSettings::IniFormat);
    store.setValue("export/resize", true); // As older versions stored it: no box.
    REQUIRE_FALSE(app::restoreExportSettings(store).resize);
    store.setValue("export/width", 0);
    store.setValue("export/height", 500);
    REQUIRE_FALSE(app::restoreExportSettings(store).resize);
    store.setValue("export/width", "wide");
    REQUIRE_FALSE(app::restoreExportSettings(store).resize);
    store.setValue("export/width", 100000);
    REQUIRE_FALSE(app::restoreExportSettings(store).resize);
}

TEST_CASE("Nothing stored, or nonsense, gives the defaults", "[app][export][settings]") {
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    QSettings store(file, QSettings::IniFormat);
    const ExportSettings defaults;

    ExportSettings restored = app::restoreExportSettings(store);
    REQUIRE(restored.format == defaults.format);
    REQUIRE(restored.quality == defaults.quality);

    store.setValue("export/format", 99);
    store.setValue("export/encoding", 0); // The working encoding is no output.
    store.setValue("export/quality", 1000);
    store.setValue("export/sharpening", "much");
    restored = app::restoreExportSettings(store);
    REQUIRE(restored.format == defaults.format);
    REQUIRE(restored.encoding == defaults.encoding);
    REQUIRE(restored.quality == defaults.quality);
    REQUIRE(restored.sharpening == defaults.sharpening);
}

TEST_CASE("Metadata choices are remembered", "[app][export][settings][metadata]") {
    ExportSettings settings;
    settings.captureInfo = false;
    settings.location = true;
    settings.descriptive = false;
    const test::TempDir dir;
    const QString file = QString::fromStdU16String(dir.file("settings.ini").u16string());
    {
        QSettings store(file, QSettings::IniFormat);
        app::saveExportSettings(settings, store);
    }
    QSettings store(file, QSettings::IniFormat);
    const ExportSettings restored = app::restoreExportSettings(store);
    REQUIRE_FALSE(restored.captureInfo);
    REQUIRE(restored.location);
    REQUIRE_FALSE(restored.descriptive);
}

TEST_CASE("Export settings stored by an earlier build are restored", "[app][export][settings]") {
    const test::TempDir dir;
    QSettings store(QString::fromStdU16String(dir.file("settings.ini").u16string()),
                    QSettings::IniFormat);
    // The keys and ints an earlier build wrote: the enumerations as their positions.
    store.setValue("export/format", 2);   // Tiff
    store.setValue("export/encoding", 3); // AdobeRgb
    store.setValue("export/sixteenBit", true);
    store.setValue("export/resize", true);
    store.setValue("export/width", 2048);
    store.setValue("export/height", 2048);
    store.setValue("export/allowEnlarging", true);
    store.setValue("export/quality", 77);
    store.setValue("export/sharpening", 25);
    store.setValue("export/captureInfo", false);
    store.setValue("export/location", true);
    store.setValue("export/descriptive", false);

    const ExportSettings expected{.format = ImageFileFormat::Tiff,
                                  .encoding = NamedEncoding::AdobeRgb,
                                  .sixteenBit = true,
                                  .resize = true,
                                  .width = 2048,
                                  .height = 2048,
                                  .allowEnlarging = true,
                                  .quality = 77,
                                  .sharpening = 25,
                                  .captureInfo = false,
                                  .location = true,
                                  .descriptive = false};
    CHECK(app::restoreExportSettings(store) == expected);
}
