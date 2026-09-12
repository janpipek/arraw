#include "core/SystemInfo.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QSysInfo>

TEST_CASE("backendName maps every QRhi::Implementation to a distinct readable string", "[sysinfo]") {
    const QRhi::Implementation backends[]
        = {QRhi::Null, QRhi::Vulkan, QRhi::OpenGLES2, QRhi::D3D11, QRhi::Metal, QRhi::D3D12};
    QSet<QString> names;
    for (const auto backend : backends) {
        const QString name = sysinfo::backendName(backend);
        REQUIRE_FALSE(name.isEmpty());
        names.insert(name);
    }
    REQUIRE(names.size() == 6); // no two backends collide on the same label
}

TEST_CASE(
    "deviceTypeName maps every QRhiDriverInfo::DeviceType to a distinct readable string",
    "[sysinfo]") {
    const QRhiDriverInfo::DeviceType types[] = {
        QRhiDriverInfo::UnknownDevice,
        QRhiDriverInfo::IntegratedDevice,
        QRhiDriverInfo::DiscreteDevice,
        QRhiDriverInfo::ExternalDevice,
        QRhiDriverInfo::VirtualDevice,
        QRhiDriverInfo::CpuDevice,
    };
    QSet<QString> names;
    for (const auto type : types) {
        const QString name = sysinfo::deviceTypeName(type);
        REQUIRE_FALSE(name.isEmpty());
        names.insert(name);
    }
    REQUIRE(names.size() == 6);
}

TEST_CASE("gather reports GPU details when backend and driver info are known", "[sysinfo]") {
    QRhiDriverInfo driver;
    driver.deviceName = "Test GPU 9000";
    driver.vendorId = 0x10de;
    driver.deviceId = 0x2782;
    driver.deviceType = QRhiDriverInfo::DiscreteDevice;

    const sysinfo::Info info = sysinfo::gather(
        QRhi::D3D11,
        driver,
        QStringLiteral("/settings/arraw.conf"),
        QStringLiteral("/data/presets"),
        QStringLiteral("/home/.arraw/cache"),
        QStringLiteral("1.2.3"));

    REQUIRE(info.gpuBackend == sysinfo::backendName(QRhi::D3D11));
    REQUIRE(info.gpuDeviceName == QStringLiteral("Test GPU 9000"));
    REQUIRE(info.gpuDeviceType == sysinfo::deviceTypeName(QRhiDriverInfo::DiscreteDevice));
    REQUIRE(info.gpuDeviceIds.contains("10de"));
    REQUIRE(info.gpuDeviceIds.contains("2782"));
}

TEST_CASE(
    "gather leaves Device IDs empty when the backend reports none (vendorId and "
    "deviceId both zero, as Mesa's OpenGL backend does)",
    "[sysinfo]") {
    QRhiDriverInfo driver;
    driver.deviceName = "Intel Mesa Intel(R) UHD Graphics";
    driver.vendorId = 0;
    driver.deviceId = 0;
    driver.deviceType = QRhiDriverInfo::UnknownDevice;

    const sysinfo::Info info = sysinfo::gather(
        QRhi::OpenGLES2,
        driver,
        QStringLiteral("/settings/arraw.conf"),
        QStringLiteral("/data/presets"),
        QStringLiteral("/home/.arraw/cache"),
        QStringLiteral("1.2.3"));

    CHECK(info.gpuDeviceName == QStringLiteral("Intel Mesa Intel(R) UHD Graphics"));
    CHECK(info.gpuDeviceIds.isEmpty());
}

TEST_CASE("gather reports a placeholder when GPU info is not yet available", "[sysinfo]") {
    const sysinfo::Info info = sysinfo::gather(
        std::nullopt,
        std::nullopt,
        QStringLiteral("/settings/arraw.conf"),
        QStringLiteral("/data/presets"),
        QStringLiteral("/home/.arraw/cache"),
        QStringLiteral("1.2.3"));

    REQUIRE_FALSE(info.gpuBackend.isEmpty());
    REQUIRE_FALSE(info.gpuDeviceName.isEmpty());
    REQUIRE_FALSE(info.gpuDeviceType.isEmpty());
    REQUIRE(info.gpuDeviceIds.isEmpty());
}

TEST_CASE(
    "gather passes through injected paths and version unchanged, and fills OS/CPU/build facts",
    "[sysinfo]") {
    const sysinfo::Info info = sysinfo::gather(
        std::nullopt,
        std::nullopt,
        QStringLiteral("/settings/arraw.conf"),
        QStringLiteral("/data/presets"),
        QStringLiteral("/home/.arraw/cache"),
        QStringLiteral("1.2.3"));

    REQUIRE(info.settingsPath == QStringLiteral("/settings/arraw.conf"));
    REQUIRE(info.presetsPath == QStringLiteral("/data/presets"));
    REQUIRE(info.cachePath == QStringLiteral("/home/.arraw/cache"));
    REQUIRE(info.appVersion == QStringLiteral("1.2.3"));
    REQUIRE(info.qtVersion == QString::fromLatin1(qVersion()));
    REQUIRE(info.osName == QSysInfo::prettyProductName());
    REQUIRE(info.cpuArchitecture == QSysInfo::currentCpuArchitecture());
    REQUIRE(
        (info.buildType == QStringLiteral("Debug") || info.buildType == QStringLiteral("Release")));
}

TEST_CASE("toPlainText includes every field's value for bug-report copying", "[sysinfo]") {
    sysinfo::Info info;
    info.gpuBackend = "MarkerBackend";
    info.gpuDeviceName = "MarkerDevice";
    info.gpuDeviceType = "MarkerType";
    info.gpuDeviceIds = "MarkerIds";
    info.settingsPath = "MarkerSettingsPath";
    info.presetsPath = "MarkerPresetsPath";
    info.cachePath = "MarkerCachePath";
    info.appVersion = "MarkerAppVersion";
    info.qtVersion = "MarkerQtVersion";
    info.osName = "MarkerOsName";
    info.cpuArchitecture = "MarkerCpuArch";
    info.buildType = "MarkerBuildType";

    const QString text = sysinfo::toPlainText(info);

    for (const QString& marker : {
             info.gpuBackend,
             info.gpuDeviceName,
             info.gpuDeviceType,
             info.gpuDeviceIds,
             info.settingsPath,
             info.presetsPath,
             info.cachePath,
             info.appVersion,
             info.qtVersion,
             info.osName,
             info.cpuArchitecture,
             info.buildType,
         })
        REQUIRE(text.contains(marker));
}
