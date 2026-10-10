#include "BrushCoverageCache.h"

#include "RowBands.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace arraw {

namespace {

constexpr std::size_t maximumEntries = 512;

/// Gives the next tile serial, unique in the process.
std::uint64_t nextSerial() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::size_t bytesOf(const CoverageTile& tile) noexcept {
    return tile.values.size() * sizeof(float);
}

} // namespace

CoverageTiles::CoverageTiles(ImageSize size, std::uint32_t tileSize,
                             std::vector<std::shared_ptr<const CoverageTile>> tiles)
    : size_(size), tileSize_(tileSize), tiles_(std::move(tiles)) {
    if (size.empty() || tileSize == 0) {
        throw std::invalid_argument("a tile grid needs a size and a tile size");
    }
    if (tiles_.size() != static_cast<std::size_t>(columns()) * rows()) {
        throw std::invalid_argument("a tile grid needs one tile per cell");
    }
}

float CoverageTiles::at(std::uint32_t x, std::uint32_t y) const {
    if (x >= size_.width || y >= size_.height) {
        throw std::out_of_range("pixel outside the raster");
    }
    const auto& tile = tiles_[static_cast<std::size_t>(y / tileSize_) * columns() + x / tileSize_];
    if (!tile) {
        return 0.0F;
    }
    return tile->values[static_cast<std::size_t>(y % tileSize_) * tile->width + x % tileSize_];
}

CoveragePlane CoverageTiles::gathered() const {
    CoveragePlane plane{size_,
                        std::vector<float>(static_cast<std::size_t>(size_.width) * size_.height)};
    detail::forEachRowBand(size_.height, size_.width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y) {
            float* out = plane.values.data() + static_cast<std::size_t>(y) * size_.width;
            const std::uint32_t tileRow = y / tileSize_;
            for (std::uint32_t column = 0; column < columns(); ++column) {
                const auto& tile = tiles_[static_cast<std::size_t>(tileRow) * columns() + column];
                if (!tile) {
                    continue;
                }
                const float* in =
                    tile->values.data() + static_cast<std::size_t>(y % tileSize_) * tile->width;
                std::copy_n(in, tile->width, out + static_cast<std::size_t>(column) * tileSize_);
            }
        }
    });
    return plane;
}

std::size_t CoverageTiles::tileBytes() const noexcept {
    std::size_t total = 0;
    for (const auto& tile : tiles_) {
        if (tile) {
            total += bytesOf(*tile);
        }
    }
    return total;
}

BrushCoverageCache::BrushCoverageCache(std::size_t budgetBytes, std::uint32_t tileSize)
    : budget_(budgetBytes), tileSize_(tileSize) {
    if (tileSize == 0) {
        throw std::invalid_argument("a tile needs a size");
    }
}

BrushCoverageCache::Result
BrushCoverageCache::drawn(const std::shared_ptr<const CoverageTiles>& base,
                          std::span<const PlacedStroke> placed, ImageSize raster,
                          CoverageLookup lookup) const {
    const std::uint32_t columns = (raster.width + tileSize_ - 1) / tileSize_;
    const std::uint32_t rows = (raster.height + tileSize_ - 1) / tileSize_;
    std::vector<bool> touched(static_cast<std::size_t>(columns) * rows, false);
    for (const PlacedStroke& stroke : placed) {
        for (const DabCentre& dab : stroke.dabs) {
            const PixelSpan xs = dabSpan(dab.x, stroke.radius);
            const PixelSpan ys = dabSpan(dab.y, stroke.radius);
            const std::int64_t x0 = std::max<std::int64_t>(xs.first, 0);
            const std::int64_t x1 = std::min<std::int64_t>(xs.last, raster.width);
            const std::int64_t y0 = std::max<std::int64_t>(ys.first, 0);
            const std::int64_t y1 = std::min<std::int64_t>(ys.last, raster.height);
            if (x0 >= x1 || y0 >= y1) {
                continue;
            }
            for (std::int64_t row = y0 / tileSize_; row <= (y1 - 1) / tileSize_; ++row) {
                for (std::int64_t column = x0 / tileSize_; column <= (x1 - 1) / tileSize_;
                     ++column) {
                    touched[static_cast<std::size_t>(row) * columns + column] = true;
                }
            }
        }
    }
    Result result;
    result.lookup = lookup;
    std::vector<std::shared_ptr<const CoverageTile>> grid(touched.size());
    if (base) {
        for (std::uint32_t i = 0; i < grid.size(); ++i) {
            grid[i] = base->tile(i);
        }
    }
    for (std::uint32_t i = 0; i < touched.size(); ++i) {
        if (touched[i]) {
            result.dirty.push_back(i);
        }
    }
    // One bucket per tile row, so that a tile paints only the dabs that reach its rows.
    const DabBuckets buckets(placed, raster.height, tileSize_);
    std::vector<std::shared_ptr<CoverageTile>> fresh(result.dirty.size());
    detail::forEachRowBand(
        static_cast<std::uint32_t>(result.dirty.size()), tileSize_ * tileSize_,
        [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t n = first; n < last; ++n) {
                const std::uint32_t index = result.dirty[n];
                const std::uint32_t x = index % columns * tileSize_;
                const std::uint32_t y = index / columns * tileSize_;
                const std::uint32_t width = std::min(tileSize_, raster.width - x);
                const std::uint32_t height = std::min(tileSize_, raster.height - y);
                std::shared_ptr<CoverageTile> tile;
                if (grid[index]) {
                    tile = std::make_shared<CoverageTile>(*grid[index]);
                } else {
                    tile = std::make_shared<CoverageTile>(
                        CoverageTile{width, height,
                                     std::vector<float>(static_cast<std::size_t>(width) * height)});
                }
                paintBucket(placed, buckets, index / columns, {x, y, width, height}, tile->values);
                tile->serial = nextSerial();
                fresh[n] = std::move(tile);
            }
        });
    for (std::size_t n = 0; n < fresh.size(); ++n) {
        grid[result.dirty[n]] = std::move(fresh[n]);
    }
    result.tiles = std::make_shared<const CoverageTiles>(raster, tileSize_, std::move(grid));
    return result;
}

std::shared_ptr<const CoverageTiles>
BrushCoverageCache::find(const std::shared_ptr<const StrokeList>& strokes, ImageSize raster) {
    if (!strokes || raster.empty()) {
        return nullptr;
    }
    const std::scoped_lock lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.size == raster && entry.list == strokes) {
            entry.lastUse = ++clock_;
            return entry.tiles;
        }
    }
    for (Entry& entry : entries_) {
        if (entry.size == raster && entry.list->contentHash() == strokes->contentHash() &&
            *entry.list == *strokes) {
            entry.lastUse = ++clock_;
            return entry.tiles;
        }
    }
    return nullptr;
}

BrushCoverageCache::Result
BrushCoverageCache::coverage(const std::shared_ptr<const StrokeList>& strokes, ImageSize raster,
                             bool retain) {
    if (!strokes || raster.empty()) {
        throw std::invalid_argument("coverage needs a list and a size");
    }
    if (strokes->rasteriser() != brushRasteriserVersion) {
        throw std::invalid_argument("unknown brush rasteriser version");
    }
    const auto sameSize = [&](const Entry& e) {
        return e.size.width == raster.width && e.size.height == raster.height;
    };
    std::shared_ptr<const CoverageTiles> base;
    std::size_t prefix = 0;
    {
        const std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_) {
            if (entry.list == strokes && sameSize(entry)) {
                entry.lastUse = ++clock_;
                return {entry.tiles, CoverageLookup::Hit, {}};
            }
        }
        for (Entry& entry : entries_) {
            if (sameSize(entry) && entry.list->contentHash() == strokes->contentHash() &&
                *entry.list == *strokes) {
                entry.lastUse = ++clock_;
                Result result{entry.tiles, CoverageLookup::ContentHit, {}};
                if (retain) {
                    insert({strokes, raster, entry.tiles, ++clock_});
                }
                return result;
            }
        }
        const auto wanted = strokes->strokes();
        for (Entry& entry : entries_) {
            const auto held = entry.list->strokes();
            if (!sameSize(entry) || entry.list->rasteriser() != strokes->rasteriser() ||
                held.empty() || held.size() >= wanted.size() || held.size() <= prefix) {
                continue;
            }
            if (std::equal(held.begin(), held.end(), wanted.begin(),
                           [](const auto& a, const auto& b) { return a.get() == b.get(); })) {
                prefix = held.size();
                base = entry.tiles;
                entry.lastUse = ++clock_;
            }
        }
    }
    const std::vector<PlacedStroke> placed = placedStrokes(*strokes, raster, prefix);
    Result result =
        drawn(base, placed, raster, base ? CoverageLookup::Extended : CoverageLookup::Miss);
    if (retain) {
        const std::scoped_lock lock(mutex_);
        insert({strokes, raster, result.tiles, ++clock_});
    }
    return result;
}

void BrushCoverageCache::retainTiles(const CoverageTiles& tiles) {
    for (std::uint32_t i = 0; i < tiles.columns() * tiles.rows(); ++i) {
        const auto& tile = tiles.tile(i);
        if (tile && references_[tile.get()]++ == 0) {
            bytes_ += bytesOf(*tile);
        }
    }
}

void BrushCoverageCache::releaseTiles(const CoverageTiles& tiles) {
    for (std::uint32_t i = 0; i < tiles.columns() * tiles.rows(); ++i) {
        const auto& tile = tiles.tile(i);
        if (tile) {
            const auto found = references_.find(tile.get());
            if (--found->second == 0) {
                bytes_ -= bytesOf(*tile);
                references_.erase(found);
            }
        }
    }
}

void BrushCoverageCache::insert(Entry entry) {
    // Two threads that missed on the same request both arrive here: keep one entry per key,
    // or the same coverage would count twice towards the budget.
    const auto duplicate = std::ranges::find_if(entries_, [&](const Entry& e) {
        return e.list == entry.list && e.size.width == entry.size.width &&
               e.size.height == entry.size.height;
    });
    if (duplicate != entries_.end()) {
        releaseTiles(*duplicate->tiles);
        entries_.erase(duplicate);
    }
    retainTiles(*entry.tiles);
    entries_.push_back(std::move(entry));
    while (entries_.size() > 1 && (bytes_ > budget_ || entries_.size() > maximumEntries)) {
        // The entry just made is the last one and is never the one to go.
        const auto oldest =
            std::min_element(entries_.begin(), entries_.end() - 1,
                             [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; });
        releaseTiles(*oldest->tiles);
        entries_.erase(oldest);
    }
}

std::size_t BrushCoverageCache::memoryBytes() const {
    const std::scoped_lock lock(mutex_);
    return bytes_;
}

std::size_t BrushCoverageCache::entryCount() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

void BrushCoverageCache::clear() {
    const std::scoped_lock lock(mutex_);
    entries_.clear();
    references_.clear();
    bytes_ = 0;
}

} // namespace arraw
