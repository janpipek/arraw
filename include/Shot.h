#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace arraw {

/// @brief One capture as a photographer sees it: the file that is developed and what came with it.
///
/// A camera shooting RAW+JPEG writes two files with one stem, and for culling
/// they are one shot, not two (ADR 029).
struct Shot {
    /// @brief File that is shown and developed: the RAW when the shot has one.
    std::filesystem::path primary;

    /// @brief Standard images of the same capture, in natural order of their names.
    std::vector<std::filesystem::path> companions;

    friend bool operator==(const Shot&, const Shot&) = default;
};

/// @brief Groups files into shots.
///
/// Pure: only the path strings are looked at, never the filesystem. Files pair
/// when they share a parent folder and a stem, the stem compared
/// case-insensitively. A RAW is the primary of its shot, and the standard
/// images (JPEG, PNG, TIFF) of that stem are its companions. A file without a
/// RAW partner stands alone, and so does every file of a stem that two RAWs
/// share, because there is no telling which of them a JPEG belongs to. Files
/// ::arraw::isSupportedImage declines, sidecars among them, are dropped.
///
/// The shots come in natural order of the primary's file name: case-insensitive,
/// runs of digits compared as numbers, so `IMG_2` precedes `IMG_10`. Ties are
/// broken by folder and then by the exact name, so the order never depends on
/// the order of @p paths.
/// @param paths Files to group, in any order.
/// @return The shots.
[[nodiscard]] std::vector<Shot> groupShots(std::vector<std::filesystem::path> paths);

/// @brief Lists the shots of a folder.
///
/// The regular files of the folder itself, not of its subfolders, without
/// hidden ones (names that begin with a dot), through ::arraw::groupShots.
/// @param folder Folder to list.
/// @return Its shots; none for a folder without photographs.
/// @throws std::runtime_error if the folder cannot be read, naming it.
[[nodiscard]] std::vector<Shot> listShots(const std::filesystem::path& folder);

/// @brief Names the formats of a shot, as the film strip labels it.
///
/// The canonical format of the primary, then of each companion, each format
/// once, joined by `+`: `ARW`, `JPEG`, `ARW+JPEG`. `jpg` and `jpeg` are JPEG,
/// `tif` and `tiff` are TIFF, any other extension is upper-cased.
/// @param shot Shot to label.
/// @return The label.
[[nodiscard]] std::string formatLabel(const Shot& shot);

} // namespace arraw
