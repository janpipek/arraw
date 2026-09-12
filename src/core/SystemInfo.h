#pragma once

#include <optional>
#include <rhi/qrhi.h>
#include <QString>

// Pure diagnostic snapshot for Help > System Info. Nothing here touches a live
// QRhi, QStandardPaths, or QSettings: gather() takes already-known facts —
// backend/driver info the caller pulled from an initialized QRhi (or nullopt
// before that happens), and paths/version strings the caller already resolved
// — the same inject-the-facts pattern PresetStore uses for its directory, so
// this unit-tests headlessly with plain values.
namespace sysinfo {

// Every field is display-ready: SystemInfoDialog only renders rows from this,
// it never touches QRhi types itself.
struct Info {
    // Rendering
    QString gpuBackend;    // e.g. "OpenGL"; a placeholder if not yet known
    QString gpuDeviceName; // e.g. "NVIDIA GeForce RTX 3080 Ti"
    QString gpuDeviceType; // e.g. "Discrete"
    QString gpuDeviceIds;  // e.g. "vendor 0x10de, device 0x2782"; empty if unavailable

    // File Locations
    QString settingsPath;
    QString presetsPath;
    QString cachePath;

    // System
    QString appVersion;
    QString qtVersion;
    QString osName;
    QString cpuArchitecture;
    QString buildType; // "Debug" or "Release"
};

// Readable label for a QRhi backend, e.g. "Direct3D 11", "Software (Null)".
QString backendName(QRhi::Implementation backend);

// Readable label for a QRhiDriverInfo::DeviceType, e.g. "Discrete", "Integrated".
QString deviceTypeName(QRhiDriverInfo::DeviceType type);

// Assembles the snapshot. `backend`/`driverInfo` are nullopt until the
// viewport's QRhi has initialized (normally happens before first paint); the
// GPU fields then read as "not yet available" instead of crashing on a null
// QRhi.
Info gather(
    std::optional<QRhi::Implementation> backend,
    std::optional<QRhiDriverInfo> driverInfo,
    QString settingsPath,
    QString presetsPath,
    QString cachePath,
    QString appVersion);

// Renders `info` as "Label: value" lines grouped by section, for pasting into
// a bug report.
QString toPlainText(const Info& info);

} // namespace sysinfo
