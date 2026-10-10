#include "BrushCoverageCache.h"

#include "RowBands.h"

#include <algorithm>
#include <atomic>
#include <optional>
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

BrushCoverageCache::Drawn
BrushCoverageCache::drawn(const std::shared_ptr<const CoverageTiles>& base,
                          std::span<const PlacedStroke> placed, ImageSize raster,
                          CoverageLookup lookup, const EndDab* end, bool keepSettled) const {
    const std::uint32_t columns = (raster.width + tileSize_ - 1) / tileSize_;
    const std::uint32_t rows = (raster.height + tileSize_ - 1) / tileSize_;
    const std::size_t cells = static_cast<std::size_t>(columns) * rows;
    // Marks the tiles a dab at a centre, of a stroke of a radius, can reach.
    const auto mark = [&](std::vector<bool>& marks, const DabCentre& centre, float radius) {
        const PixelSpan xs = dabSpan(centre.x, radius);
        const PixelSpan ys = dabSpan(centre.y, radius);
        const std::int64_t x0 = std::max<std::int64_t>(xs.first, 0);
        const std::int64_t x1 = std::min<std::int64_t>(xs.last, raster.width);
        const std::int64_t y0 = std::max<std::int64_t>(ys.first, 0);
        const std::int64_t y1 = std::min<std::int64_t>(ys.last, raster.height);
        if (x0 >= x1 || y0 >= y1) {
            return;
        }
        for (std::int64_t row = y0 / tileSize_; row <= (y1 - 1) / tileSize_; ++row) {
            for (std::int64_t column = x0 / tileSize_; column <= (x1 - 1) / tileSize_; ++column) {
                marks[static_cast<std::size_t>(row) * columns + column] = true;
            }
        }
    };
    std::vector<bool> touched(cells, false);
    std::vector<bool> endTouched(cells, false);
    for (const PlacedStroke& stroke : placed) {
        for (const DabCentre& dab : stroke.dabs) {
            mark(touched, dab, stroke.radius);
        }
    }
    if (end != nullptr) {
        mark(endTouched, end->centre, end->style->radius);
    }
    Result result;
    result.lookup = lookup;
    std::vector<std::shared_ptr<const CoverageTile>> grid(cells);
    if (base) {
        for (std::uint32_t i = 0; i < grid.size(); ++i) {
            grid[i] = base->tile(i);
        }
    }
    for (std::uint32_t i = 0; i < cells; ++i) {
        if (touched[i] || endTouched[i]) {
            result.dirty.push_back(i);
        }
    }
    if (result.dirty.empty()) {
        // No loop to run, but an observer counting the drawing as a unit still has it done.
        detail::completeUnit();
    }
    // One bucket per tile row, so that a tile paints only the dabs that reach its rows.
    const DabBuckets buckets(placed, raster.height, tileSize_);
    // One pass paints the dabs and then the end dab, so that a large brush's end dab runs in
    // parallel too.
    std::vector<std::shared_ptr<const CoverageTile>> settledTiles(result.dirty.size());
    std::vector<std::shared_ptr<const CoverageTile>> finalTiles(result.dirty.size());
    detail::forEachRowBand(
        static_cast<std::uint32_t>(result.dirty.size()), tileSize_ * tileSize_,
        [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t n = first; n < last; ++n) {
                const std::uint32_t index = result.dirty[n];
                const std::uint32_t x = index % columns * tileSize_;
                const std::uint32_t y = index / columns * tileSize_;
                const std::uint32_t width = std::min(tileSize_, raster.width - x);
                const std::uint32_t height = std::min(tileSize_, raster.height - y);
                const PixelRect rect{x, y, width, height};
                std::shared_ptr<CoverageTile> tile =
                    grid[index]
                        ? std::make_shared<CoverageTile>(*grid[index])
                        : std::make_shared<CoverageTile>(CoverageTile{
                              width, height,
                              std::vector<float>(static_cast<std::size_t>(width) * height)});
                if (touched[index]) {
                    paintBucket(placed, buckets, index / columns, rect, tile->values);
                    tile->serial = nextSerial();
                }
                if (!endTouched[index]) {
                    settledTiles[n] = tile;
                    finalTiles[n] = std::move(tile);
                    continue;
                }
                if (keepSettled && touched[index]) {
                    // The tile as it was before the end dab, and a copy for the end dab.
                    settledTiles[n] = tile;
                    tile = std::make_shared<CoverageTile>(*tile);
                } else if (keepSettled) {
                    // Only the end dab reaches it: the base tile stays settled, and the copy
                    // made above takes the end dab.
                    settledTiles[n] = grid[index];
                }
                paintDab(*end->style, end->centre, rect, tile->values);
                tile->serial = nextSerial();
                finalTiles[n] = std::move(tile);
            }
        });
    Drawn out;
    auto settledGrid = grid;
    for (std::size_t n = 0; n < finalTiles.size(); ++n) {
        const std::uint32_t index = result.dirty[n];
        if (touched[index] || !endTouched[index]) {
            settledGrid[index] = settledTiles[n];
        }
        grid[index] = std::move(finalTiles[n]);
    }
    out.result = std::move(result);
    out.result.tiles = std::make_shared<const CoverageTiles>(raster, tileSize_, std::move(grid));
    if (end == nullptr) {
        out.settled = out.result.tiles;
    } else if (keepSettled) {
        out.settled =
            std::make_shared<const CoverageTiles>(raster, tileSize_, std::move(settledGrid));
    }
    return out;
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

BrushCoverageCache::Peek BrushCoverageCache::peek(const std::shared_ptr<const StrokeList>& strokes,
                                                  ImageSize raster) const {
    if (!strokes || raster.empty() || strokes->rasteriser() != brushRasteriserVersion) {
        return {};
    }
    const std::scoped_lock lock(mutex_);
    for (const Entry& entry : entries_) {
        if (entry.size == raster && entry.list == strokes) {
            return {CoverageLookup::Hit, strokes->strokes().size()};
        }
    }
    for (const Entry& entry : entries_) {
        if (entry.size == raster && entry.list->contentHash() == strokes->contentHash() &&
            *entry.list == *strokes) {
            return {CoverageLookup::ContentHit, strokes->strokes().size()};
        }
    }
    if (growthBase(*strokes, raster)) {
        return {CoverageLookup::Grown, strokes->strokes().size() - 1};
    }
    // The same test as ::arraw::BrushCoverageCache::coverage makes for the longest prefix to
    // extend.
    const auto wanted = strokes->strokes();
    std::size_t prefix = 0;
    for (const Entry& entry : entries_) {
        const auto held = entry.list->strokes();
        if (entry.size == raster && entry.list->rasteriser() == strokes->rasteriser() &&
            !held.empty() && held.size() < wanted.size() && held.size() > prefix &&
            std::equal(held.begin(), held.end(), wanted.begin(),
                       [](const auto& a, const auto& b) { return a.get() == b.get(); })) {
            prefix = held.size();
        }
    }
    if (prefix != 0) {
        return {CoverageLookup::Extended, prefix};
    }
    return {};
}

std::optional<std::size_t> BrushCoverageCache::growthBase(const StrokeList& wanted,
                                                          ImageSize raster) const {
    const auto w = wanted.strokes();
    if (w.empty()) {
        return std::nullopt;
    }
    const Stroke& last = *w.back();
    std::optional<std::size_t> best;
    std::size_t bestPoints = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        if (!(entry.size == raster) || !entry.settled ||
            entry.list->rasteriser() != wanted.rasteriser()) {
            continue;
        }
        const auto h = entry.list->strokes();
        if (h.size() != w.size() || h.back().get() == w.back().get()) {
            continue;
        }
        const Stroke& held = *h.back();
        if (held.radius != last.radius || held.hardness != last.hardness ||
            held.flow != last.flow || held.erase != last.erase ||
            held.points.size() >= last.points.size() || held.points.size() <= bestPoints) {
            continue;
        }
        const bool sameEarlier =
            std::equal(h.begin(), h.end() - 1, w.begin(),
                       [](const auto& a, const auto& b) { return a.get() == b.get(); });
        if (sameEarlier &&
            std::equal(held.points.begin(), held.points.end(), last.points.begin())) {
            best = i;
            bestPoints = held.points.size();
        }
    }
    return best;
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
    const auto counted = [&](CoverageLookup kind) {
        lookups_[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    };
    std::shared_ptr<const CoverageTiles> base;
    std::size_t prefix = 0;
    // What a Grown builds on: the entry's list is held so that its identity stays unique.
    std::shared_ptr<const StrokeList> parent;
    std::shared_ptr<const CoverageTiles> parentSettled;
    std::shared_ptr<const CoverageTiles> parentTiles;
    std::size_t settledDabs = 0;
    {
        const std::scoped_lock lock(mutex_);
        for (Entry& entry : entries_) {
            if (entry.list == strokes && sameSize(entry)) {
                entry.lastUse = ++clock_;
                counted(CoverageLookup::Hit);
                return {entry.tiles, CoverageLookup::Hit, {}};
            }
        }
        for (Entry& entry : entries_) {
            if (sameSize(entry) && entry.list->contentHash() == strokes->contentHash() &&
                *entry.list == *strokes) {
                entry.lastUse = ++clock_;
                counted(CoverageLookup::ContentHit);
                Result result{entry.tiles, CoverageLookup::ContentHit, {}};
                if (retain) {
                    insert(
                        {strokes, raster, entry.tiles, ++clock_, entry.settled, entry.settledDabs});
                }
                return result;
            }
        }
        if (const auto grown = growthBase(*strokes, raster)) {
            Entry& entry = entries_[*grown];
            parent = entry.list;
            parentSettled = entry.settled;
            parentTiles = entry.tiles;
            settledDabs = entry.settledDabs;
            entry.lastUse = ++clock_;
        } else {
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
    }
    Drawn made;
    std::size_t madeDabs = 0;
    if (parent) {
        // Only the dabs the path gained since the settled grid, then the end dab.
        PlacedStroke placed = placedStroke(*strokes->strokes().back(), raster);
        const std::size_t regular = placed.regular;
        if (regular < settledDabs) {
            throw std::logic_error("a grown stroke lost dabs");
        }
        std::vector<DabCentre> all = std::move(placed.dabs);
        PlacedStroke tail = placed;
        tail.dabs.assign(all.begin() + static_cast<std::ptrdiff_t>(settledDabs),
                         all.begin() + static_cast<std::ptrdiff_t>(regular));
        const EndDab end{&placed, all.size() > regular ? all.back() : DabCentre{}};
        made = drawn(parentSettled, {&tail, 1}, raster, CoverageLookup::Grown,
                     all.size() > regular ? &end : nullptr, retain);
        madeDabs = regular;
        // The tiles the previous update's end dab touched went back to the settled grid.
        const ImageSize cells{made.result.tiles->columns(), made.result.tiles->rows()};
        for (std::uint32_t i = 0; i < cells.width * cells.height; ++i) {
            if (parentTiles->tile(i) != parentSettled->tile(i)) {
                made.result.dirty.push_back(i);
            }
        }
        std::ranges::sort(made.result.dirty);
        made.result.dirty.erase(std::ranges::unique(made.result.dirty).begin(),
                                made.result.dirty.end());
    } else {
        std::vector<PlacedStroke> placed = placedStrokes(*strokes, raster, prefix);
        const EndDab* end = nullptr;
        EndDab endDab{};
        if (!placed.empty()) {
            PlacedStroke& last = placed.back();
            madeDabs = last.regular;
            if (last.dabs.size() > last.regular) {
                endDab = {&last, last.dabs.back()};
                end = &endDab;
                last.dabs.resize(last.regular);
            }
        }
        made = drawn(base, placed, raster, base ? CoverageLookup::Extended : CoverageLookup::Miss,
                     end, retain);
    }
    counted(made.result.lookup);
    if (retain) {
        Entry entry{strokes, raster, made.result.tiles, 0, made.settled, madeDabs};
        if (strokes->strokes().empty()) {
            entry.settled = nullptr;
        }
        const std::scoped_lock lock(mutex_);
        entry.lastUse = ++clock_;
        insert(std::move(entry), parent.get());
    }
    return std::move(made.result);
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

void BrushCoverageCache::releaseEntry(const Entry& entry) {
    releaseTiles(*entry.tiles);
    if (entry.settled && entry.settled != entry.tiles) {
        releaseTiles(*entry.settled);
    }
}

void BrushCoverageCache::insert(Entry entry, const StrokeList* replaces) {
    // Two threads that missed on the same request both arrive here: keep one entry per key,
    // or the same coverage would count twice towards the budget.
    const auto duplicate = std::ranges::find_if(entries_, [&](const Entry& e) {
        return e.list == entry.list && e.size.width == entry.size.width &&
               e.size.height == entry.size.height;
    });
    if (duplicate != entries_.end()) {
        releaseEntry(*duplicate);
        entries_.erase(duplicate);
    }
    if (replaces != nullptr) {
        // The entry this one grew from, if no one replaced or evicted it meanwhile: a stroke of
        // 200 updates leaves one entry.
        const auto parent = std::ranges::find_if(entries_, [&](const Entry& e) {
            return e.list.get() == replaces && e.size.width == entry.size.width &&
                   e.size.height == entry.size.height;
        });
        if (parent != entries_.end()) {
            releaseEntry(*parent);
            entries_.erase(parent);
        }
    }
    retainTiles(*entry.tiles);
    if (entry.settled && entry.settled != entry.tiles) {
        retainTiles(*entry.settled);
    }
    entries_.push_back(std::move(entry));
    while (entries_.size() > 1 && (bytes_ > budget_ || entries_.size() > maximumEntries)) {
        // The entry just made is the last one and is never the one to go.
        const auto oldest =
            std::min_element(entries_.begin(), entries_.end() - 1,
                             [](const Entry& a, const Entry& b) { return a.lastUse < b.lastUse; });
        releaseEntry(*oldest);
        entries_.erase(oldest);
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t BrushCoverageCache::lookups(CoverageLookup kind) const noexcept {
    return lookups_[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed);
}

std::uint64_t BrushCoverageCache::evictions() const noexcept {
    return evictions_.load(std::memory_order_relaxed);
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
