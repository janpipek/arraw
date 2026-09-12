#include "cli/SystemInfoCommand.h"
#include "ThumbnailCache.h"
#include "core/SystemInfo.h"
#include "io/PresetStore.h"
#include "render/HeadlessRenderContext.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace cli {

namespace {

void writeRow(QTextStream& out, const TextStyle& style, const QString& label, const QString& value) {
    out << "  " << style.dim(label + ":") << " " << value << "\n";
}

QJsonObject toJson(const sysinfo::Info& info) {
    QJsonObject o;
    o["gpuBackend"] = info.gpuBackend;
    o["gpuDeviceName"] = info.gpuDeviceName;
    o["gpuDeviceType"] = info.gpuDeviceType;
    o["gpuDeviceIds"] = info.gpuDeviceIds;
    o["settingsPath"] = info.settingsPath;
    o["presetsPath"] = info.presetsPath;
    o["cachePath"] = info.cachePath;
    o["appVersion"] = info.appVersion;
    o["qtVersion"] = info.qtVersion;
    o["osName"] = info.osName;
    o["cpuArchitecture"] = info.cpuArchitecture;
    o["buildType"] = info.buildType;
    return o;
}

void writeTable(const sysinfo::Info& info, QTextStream& out, const TextStyle& style) {
    out << style.bold(QStringLiteral("Rendering")) << "\n";
    writeRow(out, style, QStringLiteral("Backend"), info.gpuBackend);
    writeRow(
        out,
        style,
        QStringLiteral("GPU"),
        QStringLiteral("%1 (%2)").arg(info.gpuDeviceName, info.gpuDeviceType));
    writeRow(out, style, QStringLiteral("Device IDs"), info.gpuDeviceIds);

    out << style.bold(QStringLiteral("File Locations")) << "\n";
    writeRow(out, style, QStringLiteral("Settings"), info.settingsPath);
    writeRow(out, style, QStringLiteral("Presets"), info.presetsPath);
    writeRow(out, style, QStringLiteral("Thumbnail cache"), info.cachePath);

    out << style.bold(QStringLiteral("System")) << "\n";
    writeRow(out, style, QStringLiteral("App version"), info.appVersion);
    writeRow(out, style, QStringLiteral("Qt version"), info.qtVersion);
    writeRow(out, style, QStringLiteral("OS"), info.osName);
    writeRow(out, style, QStringLiteral("CPU architecture"), info.cpuArchitecture);
    writeRow(out, style, QStringLiteral("Build type"), info.buildType);
}

} // namespace

int runSystemInfo(bool json, QTextStream& out, QTextStream& err, const TextStyle& style) {
    QString error;
    const auto ctx = HeadlessRenderContext::create(&error);
    if (!ctx)
        err << "arraw system-info: no GPU backend available: " << error << "\n";

    const sysinfo::Info info = sysinfo::gather(
        ctx ? std::optional(ctx->rhi()->backend()) : std::nullopt,
        ctx ? std::optional(ctx->rhi()->driverInfo()) : std::nullopt,
        QSettings().fileName(),
        defaultPresetStore().directoryPath(),
        ThumbnailCache::cacheRootPath(),
        // Not qApp->applicationVersion(): main.cpp never calls
        // setApplicationVersion() for any CLI verb (only GuiMain.cpp does, for
        // the dialog) — `version` already reads this same compile-time macro.
        QStringLiteral(ARRAW_VERSION));

    if (json) {
        out << QJsonDocument(toJson(info)).toJson(QJsonDocument::Compact) << "\n";
        return 0;
    }

    writeTable(info, out, style);
    return 0;
}

} // namespace cli
