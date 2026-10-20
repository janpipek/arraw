#pragma once

#include <ImageExport.h>

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace arraw::app {

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

} // namespace arraw::app
