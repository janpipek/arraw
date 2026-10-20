#pragma once

#include <Develop.h>
#include <ImageExport.h>

#include <QSettings>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace arraw::app {

/// @brief What the export dialog gathers: the choices behind one export.
///
/// Free of widgets, so that what the dialog means is testable without one.
struct ExportSettings {
    ImageFileFormat format = ImageFileFormat::Jpeg; ///< File format to write.
    NamedEncoding encoding = NamedEncoding::Srgb;   ///< Output colour: sRGB, Display P3, Adobe RGB.
    bool sixteenBit = false;                        ///< Whether to write 16 bits per channel.
    bool resize = false;                            ///< Whether the size below applies.
    std::uint32_t width = 0;                        ///< Widest result, in pixels, when resizing.
    std::uint32_t height = 0;                       ///< Tallest result, in pixels, when resizing.
    bool allowEnlarging = false;                    ///< Whether the size may exceed the frame's.
    int quality = 90;                               ///< JPEG quality, 0-100.
    int sharpening = 0;                             ///< Output sharpening amount, 0-100.
    bool captureInfo = true;                        ///< Whether to carry camera and capture info.
    bool location = false;                          ///< Whether to carry the GPS location.
    bool descriptive = true; ///< Whether to carry rating, label, title, rights.
};

/// @brief Makes the render request for settings.
/// @param settings What the user chose.
/// @return A request for the whole frame: fitting the box when resizing, else at its own size.
[[nodiscard]] RenderRequest requestOf(const ExportSettings& settings);

/// @brief Makes the export options for settings.
///
/// 16 bits per channel is ignored for JPEG, which has only 8.
/// @param settings What the user chose.
[[nodiscard]] ExportOptions optionsOf(const ExportSettings& settings);

/// @brief Maps the metadata choices of settings to the groups an export carries.
/// @param settings What the user chose.
[[nodiscard]] MetadataSelection selectionOf(const ExportSettings& settings);

/// @brief Gives the extension a format is written with, without the dot.
/// @param format File format.
/// @return "jpg", "png" or "tif".
[[nodiscard]] std::string_view suffixOf(ImageFileFormat format);

/// @brief Gives a path the extension of a format.
///
/// Replaces a known image extension (jpg, jpeg, png, tif, tiff, in any case)
/// that is not the format's own; keeps one that is; appends otherwise, so
/// "shot.v2" becomes "shot.v2.jpg".
/// @param path Path the user chose.
/// @param format Format being written.
[[nodiscard]] std::filesystem::path withSuffix(const std::filesystem::path& path,
                                               ImageFileFormat format);

/// @brief Suggests where to export a photograph: beside it, under its own name.
/// @param source File the photograph was read from.
/// @param format Format being written.
/// @return `<folder of source>/<stem of source>.<suffix>`.
[[nodiscard]] std::filesystem::path suggestedPath(const std::filesystem::path& source,
                                                  ImageFileFormat format);

/// @brief Names the files of an export of several photographs into one folder.
///
/// Each file is `<stem of its source>.<suffix>`; a stem met again, in any letter case, gets `_2`,
/// `_3`, … so that two photographs from different folders never write the same file, and no name
/// is a path in sources (the export never replaces a photograph). Deterministic: the first
/// photograph of a stem keeps the plain name.
/// @param sources Files the photographs were read from, in the order to name them.
/// @param folder Folder to write to.
/// @param format Format being written.
/// @return One path per source, in the same order; empty for no sources.
[[nodiscard]] std::vector<std::filesystem::path>
targetPaths(std::span<const std::filesystem::path> sources, const std::filesystem::path& folder,
            ImageFileFormat format);

/// @brief Lists the long edges, in pixels, the export dialog offers besides the original size.
[[nodiscard]] std::span<const std::uint32_t> presetLongEdges();

/// @brief Stores settings for the next time.
///
/// The size choice is kept as the settings carry it: resizing off is the
/// original size; a square box of a preset long edge is that preset; any other
/// box is the custom one. The box is stored only while resizing.
/// @param settings What the user chose.
/// @param store Where to keep them.
void saveSettings(const ExportSettings& settings, QSettings& store);

/// @brief Reads the settings stored last time.
///
/// A value that is missing or not one the dialog offers is the default. Resizing
/// is off (the original size) unless a stored box is valid: both sides from 1 to 99999.
/// @param store Where they were kept.
[[nodiscard]] ExportSettings restoreSettings(QSettings& store);

} // namespace arraw::app
