#pragma once

#include <DevelopState.h>

#include <QImage>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace arraw::app {

/// @brief Disk cache of film strip thumbnails.
///
/// One JPEG (quality 85, long edge at most ::arraw::app::ThumbnailCache::maxEdge) per entry,
/// under `<root>/<first two characters of the key>/<key>.jpg`. There are two kinds of key, and
/// the key functions below are the one place that says what a thumbnail depends on:
/// the camera's embedded preview depends on the file alone, a developed thumbnail also on the
/// saved develop state. A key contains the file's size and modification time, so an entry for a
/// file that changed is simply never asked for again; nothing needs invalidating by hand, and
/// the size cap removes the leftovers.
///
/// Writes go through a temporary file that is renamed over the entry, so a reader never sees a
/// half-written one, and a damaged entry (a full disk, a foreign file) is treated as absent and
/// deleted. Every member is safe to call from several threads. Uses no event loop.
class ThumbnailCache {
public:
    /// Longest side of a stored thumbnail, in pixels.
    static constexpr int maxEdge = 512;
    /// JPEG quality of a stored thumbnail.
    static constexpr int jpegQuality = 85;
    /// Default size cap, in bytes.
    static constexpr std::uint64_t defaultCapBytes = 512ULL * 1024 * 1024;

    /// @brief Opens the cache at a root; the directory is made when first written to.
    /// @param root Directory that holds the entries.
    /// @param capBytes Size above which ::arraw::app::ThumbnailCache::prune removes entries.
    explicit ThumbnailCache(std::filesystem::path root, std::uint64_t capBytes = defaultCapBytes);

    /// @brief Names the application's cache directory.
    /// @return `arraw/thumbnails` under the platform's generic cache location (`$XDG_CACHE_HOME` on
    /// Linux).
    [[nodiscard]] static std::filesystem::path defaultRoot();

    /// @brief Makes the key of a file's embedded preview.
    /// @param file Photograph.
    /// @return A hash of the absolute path, the size and the modification time, or nothing if the
    /// file cannot be looked at.
    [[nodiscard]] static std::optional<std::string> embeddedKey(const std::filesystem::path& file);

    /// @brief Makes the key of a file's developed thumbnail.
    ///
    /// The embedded key's ingredients plus everything in the saved state that changes the
    /// picture: the settings as ::arraw::settingsToJson writes them, and, when there are any, the
    /// local adjustments (without their id counter). Whatever else joins ::arraw::DevelopState
    /// later (spots) is added here and nowhere else.
    /// @param file Photograph.
    /// @param state Saved develop state of the photograph.
    /// @return The key, or nothing if the file cannot be looked at.
    [[nodiscard]] static std::optional<std::string> developedKey(const std::filesystem::path& file,
                                                                 const DevelopState& state);

    /// @brief Reads an entry and marks it as just used.
    /// @param key Key from one of the key functions.
    /// @return The thumbnail, or a null image if there is none or it is damaged; a damaged one is
    /// deleted.
    [[nodiscard]] QImage load(const std::string& key) const;

    /// @brief Writes an entry, replacing any, atomically.
    ///
    /// The image is reduced to fit ::arraw::app::ThumbnailCache::maxEdge first, never enlarged.
    /// @param key Key from one of the key functions.
    /// @param thumbnail Image to keep; a null one is ignored.
    /// @return Whether the entry was written. A cache that cannot write is not an error.
    bool store(const std::string& key, const QImage& thumbnail) const;

    /// @brief Deletes the least recently used entries until the cache fits its cap.
    ///
    /// "Used" is the entry's modification time, which ::arraw::app::ThumbnailCache::load and
    /// ::arraw::app::ThumbnailCache::store both refresh: access times are not kept reliably
    /// (`noatime`, `relatime`). Also removes the temporary files an interrupted write left.
    /// @return Number of entries deleted.
    int prune() const;

    /// @brief Sums the sizes of the entries.
    /// @return Bytes in use.
    [[nodiscard]] std::uint64_t sizeBytes() const;

private:
    /// @brief Names the file of an entry.
    [[nodiscard]] std::filesystem::path pathOf(const std::string& key) const;

    /// Directory that holds the entries.
    std::filesystem::path root_;
    /// Size above which entries are pruned.
    std::uint64_t capBytes_;
};

} // namespace arraw::app
