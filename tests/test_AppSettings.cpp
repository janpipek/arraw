#include "AppSettings.h"
#include "support/TempDir.h"

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
