#include "core/SystemInfo.h"

#include <QSysInfo>

namespace sysinfo {

namespace {
const QString kNotAvailable = QStringLiteral("(not yet available)");
} // namespace

QString backendName(QRhi::Implementation backend) {
    switch (backend) {
    case QRhi::Null:
        return QStringLiteral("Software (Null)");
    case QRhi::Vulkan:
        return QStringLiteral("Vulkan");
    case QRhi::OpenGLES2:
        return QStringLiteral("OpenGL");
    case QRhi::D3D11:
        return QStringLiteral("Direct3D 11");
    case QRhi::Metal:
        return QStringLiteral("Metal");
    case QRhi::D3D12:
        return QStringLiteral("Direct3D 12");
    }
    return QStringLiteral("Unknown");
}

QString deviceTypeName(QRhiDriverInfo::DeviceType type) {
    switch (type) {
    case QRhiDriverInfo::UnknownDevice:
        return QStringLiteral("Unknown");
    case QRhiDriverInfo::IntegratedDevice:
        return QStringLiteral("Integrated");
    case QRhiDriverInfo::DiscreteDevice:
        return QStringLiteral("Discrete");
    case QRhiDriverInfo::ExternalDevice:
        return QStringLiteral("External");
    case QRhiDriverInfo::VirtualDevice:
        return QStringLiteral("Virtual");
    case QRhiDriverInfo::CpuDevice:
        return QStringLiteral("Software (CPU)");
    }
    return QStringLiteral("Unknown");
}

Info gather(
    std::optional<QRhi::Implementation> backend,
    std::optional<QRhiDriverInfo> driverInfo,
    QString settingsPath,
    QString presetsPath,
    QString cachePath,
    QString appVersion) {
    Info info;

    if (backend && driverInfo) {
        info.gpuBackend = backendName(*backend);
        info.gpuDeviceName = QString::fromUtf8(driverInfo->deviceName);
        if (info.gpuDeviceName.isEmpty())
            info.gpuDeviceName = kNotAvailable;
        info.gpuDeviceType = deviceTypeName(driverInfo->deviceType);
        info.gpuDeviceIds = QStringLiteral("vendor 0x%1, device 0x%2")
                                .arg(driverInfo->vendorId, 0, 16)
                                .arg(driverInfo->deviceId, 0, 16);
    } else {
        info.gpuBackend = kNotAvailable;
        info.gpuDeviceName = kNotAvailable;
        info.gpuDeviceType = kNotAvailable;
    }

    info.settingsPath = std::move(settingsPath);
    info.presetsPath = std::move(presetsPath);
    info.cachePath = std::move(cachePath);
    info.appVersion = std::move(appVersion);
    info.qtVersion = QString::fromLatin1(qVersion());
    info.osName = QSysInfo::prettyProductName();
    info.cpuArchitecture = QSysInfo::currentCpuArchitecture();
#ifdef QT_DEBUG
    info.buildType = QStringLiteral("Debug");
#else
    info.buildType = QStringLiteral("Release");
#endif

    return info;
}

QString toPlainText(const Info& info) {
    return QStringLiteral(
               "Rendering\n"
               "  Backend: %1\n"
               "  GPU: %2 (%3)\n"
               "  Device IDs: %4\n"
               "\n"
               "File Locations\n"
               "  Settings: %5\n"
               "  Presets: %6\n"
               "  Thumbnail cache: %7\n"
               "\n"
               "System\n"
               "  App version: %8\n"
               "  Qt version: %9\n"
               "  OS: %10\n"
               "  CPU architecture: %11\n"
               "  Build type: %12\n")
        .arg(info.gpuBackend)
        .arg(info.gpuDeviceName)
        .arg(info.gpuDeviceType)
        .arg(info.gpuDeviceIds)
        .arg(info.settingsPath)
        .arg(info.presetsPath)
        .arg(info.cachePath)
        .arg(info.appVersion)
        .arg(info.qtVersion)
        .arg(info.osName)
        .arg(info.cpuArchitecture)
        .arg(info.buildType);
}

} // namespace sysinfo
