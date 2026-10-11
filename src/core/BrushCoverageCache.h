#pragma once

#include "BrushRaster.h"

#include <BrushStrokes.h>
#include <ImageBuffer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace arraw {

/// One tile of float coverage.
struct CoverageTile {
    std::uint32_t width;       ///< Columns.
    std::uint32_t height;      ///< Rows.
    std::vector<float> values; ///< Coverage, row-major, width times height values.
    /// Number unique in the process, given when the tile is made or cloned for painting: a tile
    /// that differs from another has a different serial, and no serial comes back (no ABA, as a
    /// pointer can). Zero for a tile made outside the cache.
    std::uint64_t serial = 0;
};

/// One raster of coverage as a grid of shared tiles; an absent tile is all zero.
class CoverageTiles {
public:
    /// Makes a grid.
    /// @param tiles columns() * rows() tiles in row-major order, null for an all-zero tile.
    /// @throws std::invalid_argument for an empty size, a zero tile size or a wrong tile count.
    CoverageTiles(ImageSize size, std::uint32_t tileSize,
                  std::vector<std::shared_ptr<const CoverageTile>> tiles);

    /// Pixel dimensions of the raster.
    [[nodiscard]] ImageSize size() const noexcept {
        return size_;
    }

    /// Edge of a full tile, in pixels.
    [[nodiscard]] std::uint32_t tileSize() const noexcept {
        return tileSize_;
    }

    /// Tiles per row.
    [[nodiscard]] std::uint32_t columns() const noexcept {
        return (size_.width + tileSize_ - 1) / tileSize_;
    }

    /// Rows of tiles.
    [[nodiscard]] std::uint32_t rows() const noexcept {
        return (size_.height + tileSize_ - 1) / tileSize_;
    }

    /// Reads one tile, null when it is all zero.
    [[nodiscard]] const std::shared_ptr<const CoverageTile>& tile(std::uint32_t index) const {
        return tiles_.at(index);
    }

    /// Reads one pixel.
    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const;

    /// Copies the tiles into one plane, banded by rows.
    [[nodiscard]] CoveragePlane gathered() const;

    /// Bytes of the tiles that exist.
    [[nodiscard]] std::size_t tileBytes() const noexcept;

private:
    ImageSize size_;
    std::uint32_t tileSize_;
    std::vector<std::shared_ptr<const CoverageTile>> tiles_;
};

/// How a coverage request was answered.
enum class CoverageLookup {
    Hit,        ///< The same list and size were held.
    ContentHit, ///< An equal list behind other pointers was held.
    Extended,   ///< A held list was a prefix, and only the new strokes were painted.
    Grown,      ///< A held list had the same strokes but a shorter last path: only the new dabs
                ///< of the last stroke were painted.
    Miss,       ///< Everything was drawn.
};

/// Number of ::arraw::CoverageLookup kinds.
inline constexpr std::size_t coverageLookupKinds =
    static_cast<std::size_t>(CoverageLookup::Miss) + 1;

/// Keeps the coverage of stroke lists, tile by tile, and extends it when a stroke is appended.
class BrushCoverageCache {
public:
    /// Makes an empty cache.
    /// @param budgetBytes Most bytes of distinct tiles to keep.
    /// @param tileSize Edge of a tile, in pixels.
    explicit BrushCoverageCache(std::size_t budgetBytes = std::size_t{512} << 20,
                                std::uint32_t tileSize = 128);

    /// What a request gave.
    struct Result {
        std::shared_ptr<const CoverageTiles> tiles; ///< The coverage.
        CoverageLookup lookup;                      ///< How it was found.
        /// Tiles drawn by this call, ascending, with those a `Grown` took back to their settled
        /// contents (the previous end dab's): relative to the entry it was built on, so only a
        /// hint. To find what differs from a grid already uploaded, compare tile serials.
        std::vector<std::uint32_t> dirty;
    };

    /// Finds or makes the coverage of a list at a raster size. Safe to call from many threads.
    /// @param retain Whether to keep the result. A live stroke's updates are kept too: each
    /// replaces the entry it grew from, so a stroke leaves one entry. False for a one-off.
    [[nodiscard]] Result coverage(const std::shared_ptr<const StrokeList>& strokes,
                                  ImageSize raster, bool retain = true);

    /// Finds the coverage of a list at a raster size only if it is held, whole or as an equal list
    /// behind other pointers: no insert, no extension, no drawing.
    /// @return The tiles of a `Hit` or `ContentHit`, empty otherwise. The LRU order is touched.
    [[nodiscard]] std::shared_ptr<const CoverageTiles>
    find(const std::shared_ptr<const StrokeList>& strokes, ImageSize raster);

    /// What ::arraw::BrushCoverageCache::peek found.
    struct Peek {
        CoverageLookup lookup = CoverageLookup::Miss; ///< What coverage() would answer.
        /// Strokes of the list the cache already holds drawn: the prefix an `Extended` answer
        /// builds on, the whole list for a `Hit` or `ContentHit`, all but the last for a
        /// `Grown`, else 0.
        std::size_t cachedStrokes = 0;
    };

    /// Tells what ::arraw::BrushCoverageCache::coverage would answer, without drawing, inserting
    /// or touching the LRU order: the entries, the bytes and their order stay as they are.
    /// @return `Hit`, `ContentHit`, `Grown`, `Extended` or `Miss` (also for a list the cache would
    /// refuse), with the strokes already drawn (all but the last for a `Grown`).
    [[nodiscard]] Peek peek(const std::shared_ptr<const StrokeList>& strokes,
                            ImageSize raster) const;

    /// Counts the answers of coverage() of one kind since the cache was made; `clear` keeps them.
    [[nodiscard]] std::uint64_t lookups(CoverageLookup kind) const noexcept;

    /// Counts the entries evicted by the budgets since the cache was made.
    [[nodiscard]] std::uint64_t evictions() const noexcept;

    /// Bytes of the distinct tiles across entries.
    [[nodiscard]] std::size_t memoryBytes() const;

    /// Number of entries.
    [[nodiscard]] std::size_t entryCount() const;

    /// Forgets everything.
    void clear();

private:
    /// A held coverage: a list at a raster size.
    struct Entry {
        std::shared_ptr<const StrokeList> list;     ///< The strokes the coverage is of.
        ImageSize size;                             ///< The raster size, in pixels.
        std::shared_ptr<const CoverageTiles> tiles; ///< The coverage.
        std::uint64_t lastUse;                      ///< Value of the cache clock at the last use.
        /// The coverage without the end dab of the list's last stroke, sharing every tile but
        /// those the end dab touched (a few for a small brush, many for a wash brush); null for an
        /// empty list. What a `Grown` builds on.
        std::shared_ptr<const CoverageTiles> settled;
        /// How many of the last stroke's dabs without the end dab `settled` holds.
        std::size_t settledDabs = 0;
    };

    /// A draw: the coverage, and the same without the last stroke's end dab.
    struct Drawn {
        Result result;                                ///< The coverage.
        std::shared_ptr<const CoverageTiles> settled; ///< Same tiles as `result` if no end dab.
    };

    /// The dab that ends a path, and the stroke style to paint it with.
    struct EndDab {
        const PlacedStroke* style; ///< Radius, core, flow and erase.
        DabCentre centre;          ///< Where it goes.
    };

    /// Draws the tiles that @p placed strokes touch on top of @p base (null for zero), then the
    /// end dab, in one parallel pass; each tile the end dab touches is copied first, so that the
    /// settled grid keeps it without the end dab.
    /// @param keepSettled Whether to make the settled grid; false skips the copies (a one-off).
    [[nodiscard]] Drawn drawn(const std::shared_ptr<const CoverageTiles>& base,
                              std::span<const PlacedStroke> placed, ImageSize raster,
                              CoverageLookup lookup, const EndDab* end = nullptr,
                              bool keepSettled = true) const;

    /// Finds the held entry a list grows from, the best one for `Grown`: its index among the
    /// entries, or nothing. Holds the lock.
    [[nodiscard]] std::optional<std::size_t> growthBase(const StrokeList& wanted,
                                                        ImageSize raster) const;

    /// Adds an entry, replacing one with the same key, and evicts what the budget does not
    /// allow. Holds the lock.
    /// @param replaces The list of an entry of the same size that this one grew from, which goes;
    /// null to keep every entry.
    void insert(Entry entry, const StrokeList* replaces = nullptr);

    /// Stops counting the tiles of an entry. Holds the lock.
    void releaseEntry(const Entry& entry);

    /// Counts the tiles of a grid as held. Holds the lock.
    void retainTiles(const CoverageTiles& tiles);

    /// Stops counting the tiles of a grid, freeing the bytes of those no one else holds.
    void releaseTiles(const CoverageTiles& tiles);

    std::size_t budget_;
    std::uint32_t tileSize_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::unordered_map<const CoverageTile*, std::size_t> references_;
    std::size_t bytes_ = 0;
    std::uint64_t clock_ = 0;
    std::array<std::atomic<std::uint64_t>, coverageLookupKinds> lookups_{};
    std::atomic<std::uint64_t> evictions_{0};
};

} // namespace arraw
