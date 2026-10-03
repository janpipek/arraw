#pragma once

#include "GpuContext.h"

#include <QSettings>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace arraw::app {

/// @brief Persistent desktop preferences, separate from photograph and export settings.
struct AppSettings {
    /// @brief Whether previews and exports always use the CPU.
    bool cpuOnly = false;

    /// @brief Preferred hardware identity, or automatic selection when absent.
    ///
    /// List positions are deliberately not stored: driver updates can reorder adapters.
    std::optional<GpuAdapterInfo> gpu;
};

/// @brief Reads desktop preferences, using defaults for missing or invalid values.
[[nodiscard]] AppSettings restoreAppSettings(QSettings& store);

/// @brief Stores desktop preferences without touching other settings groups.
void saveAppSettings(const AppSettings& settings, QSettings& store);

/// @brief Restores the last photograph, falling back to its folder when the file is gone.
/// @return An existing file or folder, or nothing when neither is available.
[[nodiscard]] std::optional<std::filesystem::path> restoreOpenPath(QSettings& store);

/// @brief Finds a stored hardware identity in the currently enumerated adapters.
[[nodiscard]] std::optional<std::size_t> findPreferredGpu(const GpuAdapterInfo& preferred,
                                                          std::span<const GpuAdapterInfo> adapters);

/// @brief Creates the preferred hardware context on the calling worker thread.
/// @param settings Desktop preferences captured before the worker starts.
/// @param problem Receives the reason to fall back to the CPU, if no context can be made.
[[nodiscard]] std::unique_ptr<GpuContext> createAppGpuContext(const AppSettings& settings,
                                                              std::string& problem);

} // namespace arraw::app
