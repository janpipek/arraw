#pragma once

#include <filesystem>
#include <string>
#include <system_error>
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

/// @brief Tells whether a directory entry is a folder that the recursive listing enters.
///
/// A real directory (not a symbolic link to one) that is not hidden (a name that begins
/// with a dot) and not the clutter of NAS and desktop systems (`@eaDir`, `#recycle`,
/// `$RECYCLE.BIN`, `System Volume Information`). The one rule ::arraw::listFolder,
/// ::arraw::listShots and the folder watcher share.
/// @param entry Entry to judge.
/// @return Whether to enter it; false for files and for entries that cannot be examined.
[[nodiscard]] bool isListedFolder(const std::filesystem::directory_entry& entry);

/// @brief Orders folders component by component in natural order, so that a subtree follows its
/// root.
///
/// Components compare as ::arraw::groupShots compares names (case-insensitive, runs of digits
/// as numbers), ties broken by the exact bytes of the component; a folder precedes everything
/// under it. A strict total order, and the order of a pre-order walk that enters each
/// folder's subfolders in this order, so every subtree is one contiguous run. The film strip
/// and ::arraw::listShots both order folders by it.
/// @param a Folder that might come first.
/// @param b Folder to compare it with.
/// @return Whether @p a comes before @p b.
[[nodiscard]] bool folderPrecedes(const std::filesystem::path& a, const std::filesystem::path& b);

/// @brief One directory, read once.
struct FolderListing {
    /// @brief Shots of the directory itself, in ::arraw::groupShots order.
    std::vector<Shot> shots;

    /// @brief Subdirectories ::arraw::isListedFolder accepts, in ::arraw::folderPrecedes order.
    std::vector<std::filesystem::path> folders;

    friend bool operator==(const FolderListing&, const FolderListing&) = default;
};

/// @brief Reads one directory: its shots and the subfolders a recursive listing enters.
///
/// Not recursive. The shots are those of the regular files of @p folder (symbolic links to
/// regular files included), without hidden ones (names that begin with a dot), through
/// ::arraw::groupShots. Each entry is examined once, from the type the directory read
/// reports where the platform caches it; only a symbolic link costs a further look.
/// @param folder Directory to read.
/// @return Its shots and listed subfolders.
/// @throws std::filesystem::filesystem_error if @p folder cannot be opened or read to the end,
/// carrying @p folder as `path1()` and the system's error code (see
/// ::arraw::classifyFolderError).
[[nodiscard]] FolderListing listFolder(const std::filesystem::path& folder);

/// @brief Kinds of reasons a folder could not be read, as the app reacts to them.
enum class FolderError {
    Gone,         ///< It does not exist, or is no directory (ENOENT, ENOTDIR).
    NoPermission, ///< Access is denied (EACCES, EPERM).
    Other,        ///< Anything else, often transient: EIO, ENOTCONN, ESTALE, a timeout.
};

/// @brief Classifies the error code of a folder that could not be read.
/// @param error Code from the std::filesystem::filesystem_error that ::arraw::listFolder or
/// ::arraw::listShots threw.
/// @return The kind of failure.
[[nodiscard]] FolderError classifyFolderError(const std::error_code& error);

/// @brief Lists the shots of a folder.
///
/// The shots of ::arraw::listFolder for @p folder.
///
/// With @p recursive the subfolders are listed too, to any depth: a pre-order walk over
/// ::arraw::listFolder that enters each folder's listed subfolders in
/// ::arraw::folderPrecedes order. Hidden folders, symbolic links to folders and the clutter
/// of NAS and desktop systems are not entered (see ::arraw::isListedFolder), and a subfolder
/// that cannot be read is skipped without a word, its siblings still listed. The shots thus
/// come folder by folder in ::arraw::folderPrecedes order (a folder first, then its
/// subtree), and in the order of ::arraw::groupShots within a folder. Files pair only within
/// one folder, so the same name in two folders is two shots.
/// @param folder Folder to list.
/// @param recursive Whether to list its subfolders as well.
/// @return Its shots; none for a folder without photographs.
/// @throws std::filesystem::filesystem_error (a std::runtime_error) if @p folder itself cannot
/// be read, naming it, as ::arraw::listFolder does.
[[nodiscard]] std::vector<Shot> listShots(const std::filesystem::path& folder,
                                          bool recursive = false);

/// @brief Names the formats of a shot, as the film strip labels it.
///
/// The canonical format of the primary, then of each companion, each format
/// once, joined by `+`: `ARW`, `JPEG`, `ARW+JPEG`. `jpg` and `jpeg` are JPEG,
/// `tif` and `tiff` are TIFF, any other extension is upper-cased.
/// @param shot Shot to label.
/// @return The label.
[[nodiscard]] std::string formatLabel(const Shot& shot);

} // namespace arraw
