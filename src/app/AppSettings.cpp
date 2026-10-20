#include "AppSettings.h"

#include "TimingTrace.h"
#include "WatchBudget.h"

#include <ExportSettings.h>

#include <QString>
#include <QVariant>

#include <algorithm>
#include <exception>
#include <system_error>

namespace arraw::app {

namespace {

/// Group of the stored export keys.
const QString exportGroup = QStringLiteral("export/");

/// @brief Reads a stored export integer within a range, or a fallback.
int storedExportInt(QSettings& store, const char* key, int fallback, int low, int high) {
    bool ok = false;
    const int value = store.value(exportGroup + QLatin1String(key), fallback).toInt(&ok);
    return ok && value >= low && value <= high ? value : fallback;
}

} // namespace

std::optional<std::filesystem::path> restoreOpenPath(QSettings& store) {
    const std::filesystem::path file(store.value("lastFile").toString().toStdU16String());
    const std::filesystem::path folder(store.value("lastFolder").toString().toStdU16String());
    std::error_code error;
    // With subfolders listed the remembered folder is the strip's, and may hold the file below.
    const auto holds = [&] {
        if (folder.empty()) {
            return true;
        }
        const auto parent = file.parent_path().lexically_normal();
        if (parent == folder.lexically_normal()) {
            return true;
        }
        const auto relative = parent.lexically_relative(folder.lexically_normal());
        return restoreIncludeSubfolders(store) && !relative.empty() && *relative.begin() != "..";
    };
    if (!file.empty() && std::filesystem::is_regular_file(file, error) && holds()) {
        return file;
    }
    if (!folder.empty() && std::filesystem::is_directory(folder, error)) {
        return folder;
    }
    return std::nullopt;
}

bool restoreIncludeSubfolders(QSettings& store) {
    return store.value("filmStrip/includeSubfolders", false).toBool();
}

void saveIncludeSubfolders(bool include, QSettings& store) {
    store.setValue("filmStrip/includeSubfolders", include);
}

int restoreMaxWatchedFolders(QSettings& store) {
    bool valid = false;
    const qlonglong value = store.value("filmStrip/maxWatchedFolders").toLongLong(&valid);
    if (!valid) {
        return defaultMaxWatchedFolders;
    }
    return static_cast<int>(
        std::clamp<qlonglong>(value, minWatchedFolders, maxWatchedFoldersLimit));
}

void saveMaxWatchedFolders(int limit, QSettings& store) {
    store.setValue("filmStrip/maxWatchedFolders", limit);
}

AppSettings restoreAppSettings(QSettings& store) {
    AppSettings settings;
    const QString mode = store.value("processing/device", "auto").toString();
    settings.cpuOnly = mode == "cpu";
    if (mode != "gpu") {
        return settings;
    }
    const QString name = store.value("processing/gpuName").toString();
    const QString backend = store.value("processing/backend").toString();
    bool vendorOk = false;
    bool deviceOk = false;
    const auto vendor = store.value("processing/vendorId").toULongLong(&vendorOk);
    const auto device = store.value("processing/deviceId").toULongLong(&deviceOk);
    if (!name.isEmpty() && vendorOk && deviceOk &&
        backend.toStdString() == gpuBackendName(defaultGpuBackend())) {
        settings.gpu =
            GpuAdapterInfo{.name = name.toStdString(), .vendorId = vendor, .deviceId = device};
    }
    return settings;
}

void saveAppSettings(const AppSettings& settings, QSettings& store) {
    store.setValue("processing/device", settings.cpuOnly ? "cpu" : settings.gpu ? "gpu" : "auto");
    if (!settings.cpuOnly && settings.gpu) {
        store.setValue("processing/backend",
                       QString::fromUtf8(gpuBackendName(defaultGpuBackend())));
        store.setValue("processing/gpuName", QString::fromStdString(settings.gpu->name));
        store.setValue("processing/vendorId", QString::number(settings.gpu->vendorId));
        store.setValue("processing/deviceId", QString::number(settings.gpu->deviceId));
    } else {
        store.remove("processing/backend");
        store.remove("processing/gpuName");
        store.remove("processing/vendorId");
        store.remove("processing/deviceId");
    }
}

std::optional<std::size_t> findPreferredGpu(const GpuAdapterInfo& preferred,
                                            std::span<const GpuAdapterInfo> adapters) {
    for (std::size_t index = 0; index < adapters.size(); ++index) {
        const auto& adapter = adapters[index];
        if (adapter.kind != GpuDeviceKind::Software && adapter.name == preferred.name &&
            adapter.vendorId == preferred.vendorId && adapter.deviceId == preferred.deviceId) {
            return index;
        }
    }
    return std::nullopt;
}

std::unique_ptr<GpuContext> createAppGpuContext(const AppSettings& settings, std::string& problem) {
    const detail::TimingSpan timing("gpu.select-device");
    if (settings.cpuOnly) {
        problem = "CPU selected in Settings";
        return nullptr;
    }
    if (!settings.gpu) {
        return createHardwareContext(problem);
    }
    try {
        const auto adapters = listGpuAdapters(defaultGpuBackend());
        const auto index = findPreferredGpu(*settings.gpu, adapters);
        if (!index) {
            problem = "Selected GPU is unavailable: " + settings.gpu->name;
            return nullptr;
        }
        auto context = std::make_unique<GpuContext>(defaultGpuBackend(), *index);
        if (context->info().kind == GpuDeviceKind::Software) {
            problem = "Software rasteriser refused: " + context->info().deviceName;
            return nullptr;
        }
        return context;
    } catch (const std::exception& error) {
        problem = error.what();
    } catch (...) {
        problem = "Unknown error while creating the selected GPU device";
    }
    return nullptr;
}

void saveExportSettings(const ExportSettings& settings, QSettings& store) {
    store.setValue(exportGroup + "format", static_cast<int>(settings.format));
    store.setValue(exportGroup + "encoding", static_cast<int>(settings.encoding));
    store.setValue(exportGroup + "sixteenBit", settings.sixteenBit);
    store.setValue(exportGroup + "resize", settings.resize);
    if (settings.resize) {
        store.setValue(exportGroup + "width", settings.width);
        store.setValue(exportGroup + "height", settings.height);
    }
    store.setValue(exportGroup + "allowEnlarging", settings.allowEnlarging);
    store.setValue(exportGroup + "quality", settings.quality);
    store.setValue(exportGroup + "sharpening", settings.sharpening);
    store.setValue(exportGroup + "captureInfo", settings.captureInfo);
    store.setValue(exportGroup + "location", settings.location);
    store.setValue(exportGroup + "descriptive", settings.descriptive);
}

ExportSettings restoreExportSettings(QSettings& store) {
    ExportSettings settings;
    switch (storedExportInt(store, "format", static_cast<int>(settings.format), 0, 2)) {
    case static_cast<int>(ImageFileFormat::Png):
        settings.format = ImageFileFormat::Png;
        break;
    case static_cast<int>(ImageFileFormat::Tiff):
        settings.format = ImageFileFormat::Tiff;
        break;
    default:
        break;
    }
    const int encoding = storedExportInt(store, "encoding", static_cast<int>(settings.encoding),
                                         static_cast<int>(NamedEncoding::Srgb),
                                         static_cast<int>(NamedEncoding::AdobeRgb));
    settings.encoding = static_cast<NamedEncoding>(encoding);
    settings.sixteenBit = store.value(exportGroup + "sixteenBit", settings.sixteenBit).toBool();
    const int width = storedExportInt(store, "width", 0, 1, 99999);
    const int height = storedExportInt(store, "height", 0, 1, 99999);
    if (store.value(exportGroup + "resize", false).toBool() && width > 0 && height > 0) {
        settings.resize = true;
        settings.width = static_cast<std::uint32_t>(width);
        settings.height = static_cast<std::uint32_t>(height);
    }
    settings.allowEnlarging =
        store.value(exportGroup + "allowEnlarging", settings.allowEnlarging).toBool();
    settings.quality = storedExportInt(store, "quality", settings.quality, 0, 100);
    settings.sharpening = storedExportInt(store, "sharpening", settings.sharpening, 0, 100);
    settings.captureInfo = store.value(exportGroup + "captureInfo", settings.captureInfo).toBool();
    settings.location = store.value(exportGroup + "location", settings.location).toBool();
    settings.descriptive = store.value(exportGroup + "descriptive", settings.descriptive).toBool();
    return settings;
}

} // namespace arraw::app
